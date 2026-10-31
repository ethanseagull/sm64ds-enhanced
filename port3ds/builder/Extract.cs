// The game data out of the player's dump: upstream's extract_assets.ps1
// (port-0.9.1/tools/portable_kit), done in the builder itself instead of in a
// PowerShell run beside it. It writes the same files, byte for byte:
//
//   extracted\dsd\files\<path>     every NitroFS file, as the card has it
//   build\assets\files.tsv         file id -> path
//   build\assets\handles.tsv       game handle -> file id (overlay 0's table)
//   build\assets\nitrofs.tsv       where the card's FNT, FAT and overlay tables are
//   build\assets\nitrofs_fnt.bin, nitrofs_fat.bin, nitrofs_ovt9.bin   verbatim cuts
//   build\assets\romdata.bin       the code-side tables, from the recipe, hash-checked
//   build\assets\romdata.manifest  the kit's, where the game looks for it
//
// The messages are the script's, so a player sees the same reasons.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Sm64dsEnhanced
{
    static class Extract
    {
        static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);

        // The asset-handle table: a property of the game, not of the dump (the
        // matched initialiser func_ov000_020aa420 walks exactly this many entries).
        const uint HandleTableAddress = 0x020BD4B8;
        const int HandleCount = 0x80A;

        static void Fail(string text) { throw new BuildError(text); }

        static uint U32(byte[] b, long o) { return BitConverter.ToUInt32(b, (int)o); }
        static ushort U16(byte[] b, long o) { return BitConverter.ToUInt16(b, (int)o); }

        static byte[] Cut(byte[] b, long offset, long length)
        {
            var r = new byte[length];
            if (length > 0) Array.Copy(b, offset, r, 0, length);
            return r;
        }

        // progress: the script's 0..100 (the builder maps it into its own bar)
        public static void Run(string rom, string recipePath, string manifestPath, string destination,
                               Action<string> log, Action<int, string> progress)
        {
            byte[] romBytes;
            try
            {
                romBytes = File.ReadAllBytes(rom);
            }
            catch (IOException e)
            {
                // OneDrive "Files On-Demand" and the like: the bytes may not be on this PC
                bool cloud = false;
                try { cloud = ((int)File.GetAttributes(rom) & (0x1000 | 0x40000 | 0x400000)) != 0; }
                catch (Exception) { }
                Fail(cloud ? "Your .nds file is in a cloud folder (OneDrive or similar) and is not on this PC " +
                             "yet. Right-click it, choose \"Always keep on this device\", wait for it to " +
                             "download, then try again."
                           : "The dump could not be read: " + e.Message);
                return;
            }

            // ------------------------------------------------- check it is the game
            if (romBytes.Length < 0x4000)
                Fail("That file is only " + romBytes.Length + " bytes, too small to be a DS cartridge dump.");
            string title = Encoding.ASCII.GetString(romBytes, 0, 12).TrimEnd('\0');
            string code = Encoding.ASCII.GetString(romBytes, 0x0C, 4);
            if (title != "S.MARIO64DS" || !code.StartsWith("ASM"))
                Fail("That is not a Super Mario 64 DS dump (it says title '" + Regex.Replace(title, "[^ -~]", ".") +
                     "', game code '" + Regex.Replace(code, "[^ -~]", ".") + "'). Use a dump of a Super " +
                     "Mario 64 DS cartridge.");
            if (code != "ASMP")
                Fail("This is not the European ROM (game code " + code + "). This port is built from the " +
                     "European ROM (ASMP), so its data will not come out of another region's cartridge.");

            long fntOffset = U32(romBytes, 0x40), fntSize = U32(romBytes, 0x44);
            long fatOffset = U32(romBytes, 0x48), fatSize = U32(romBytes, 0x4C);
            long ovtOffset = U32(romBytes, 0x50), ovtSize = U32(romBytes, 0x54);
            long ovt7Offset = U32(romBytes, 0x58), ovt7Size = U32(romBytes, 0x5C);
            long usedSize = U32(romBytes, 0x80);
            foreach (var span in new[] { Tuple.Create("file name table", fntOffset, fntSize),
                                         Tuple.Create("file allocation table", fatOffset, fatSize),
                                         Tuple.Create("overlay table", ovtOffset, ovtSize) })
            {
                if (span.Item2 == 0 || span.Item2 + span.Item3 > romBytes.Length)
                    Fail("This dump is truncated: its " + span.Item1 + " runs past the end of the file. " +
                         "Re-dump the cartridge.");
            }
            // an empty ARM7 overlay pair is legal (this cartridge has none)
            if (!(ovt7Offset == 0 && ovt7Size == 0) &&
                (ovt7Offset == 0 || ovt7Size == 0 || ovt7Offset + ovt7Size > romBytes.Length))
                Fail("This dump is truncated: its ARM7 overlay table runs past the end of the file. " +
                     "Re-dump the cartridge.");
            if (usedSize > romBytes.Length)
                Fail("This dump is incomplete: the header says the cartridge holds " + usedSize + " bytes " +
                     "but the file is only " + romBytes.Length + ". Re-dump the cartridge.");
            log("Super Mario 64 DS (" + code + ", Europe), " + Math.Round(romBytes.Length / 1048576.0) + " MB");
            progress(2, "Reading the file tables");

            // ------------------------------------------------------ the file tables
            // FAT: one {start, end} pair per file id
            int fatCount = (int)(fatSize / 8);
            var fatStart = new long[fatCount];
            var fatEnd = new long[fatCount];
            for (int i = 0; i < fatCount; i++)
            {
                fatStart[i] = U32(romBytes, fatOffset + i * 8);
                fatEnd[i] = U32(romBytes, fatOffset + i * 8 + 4);
            }

            // FNT: walked from the root, so a directory is named before its children;
            // a name with the high bit set is a subdirectory with a trailing id
            const string damagedFnt = "This dump's file name table is damaged. Re-dump the cartridge.";
            var paths = new SortedDictionary<int, string>();
            var queue = new Queue<KeyValuePair<int, string>>();
            queue.Enqueue(new KeyValuePair<int, string>(0, ""));
            var visited = new HashSet<int> { 0 };
            while (queue.Count > 0)
            {
                var item = queue.Dequeue();
                long entry = fntOffset + item.Key * 8L;
                if (entry + 8 > romBytes.Length) Fail(damagedFnt);
                long o = fntOffset + U32(romBytes, entry);
                int fileId = U16(romBytes, entry + 4);
                string bas = item.Value;
                while (true)
                {
                    if (o >= romBytes.Length) Fail(damagedFnt);
                    int type = romBytes[o++];
                    if (type == 0) break;
                    int len = type & 0x7F;
                    if (o + len + ((type & 0x80) != 0 ? 2 : 0) > romBytes.Length) Fail(damagedFnt);
                    string name = Encoding.ASCII.GetString(romBytes, (int)o, len);
                    o += len;
                    string full = bas.Length > 0 ? bas + "/" + name : name;
                    if ((type & 0x80) != 0)
                    {
                        int sub = U16(romBytes, o) & 0x0FFF;
                        o += 2;
                        if (visited.Add(sub)) queue.Enqueue(new KeyValuePair<int, string>(sub, full));
                    }
                    else
                    {
                        paths[fileId++] = full;
                    }
                }
            }
            if (paths.Count == 0) Fail("This dump has no named files in it. Re-dump the cartridge.");

            // ------------------------------------------------------ write them out
            // every entry checked, and the space it needs added up, before anything is written
            string filesRoot = Path.Combine(destination, "extracted", "dsd", "files");
            long needBytes = 0;
            foreach (var pair in paths)
            {
                int id = pair.Key;
                if (id < 0 || id >= fatCount)
                    Fail("This dump is damaged: its file name table names a file (" + pair.Value + ") that " +
                         "its file allocation table does not have. Re-dump the cartridge.");
                long length = fatEnd[id] - fatStart[id];
                if (length < 0 || fatStart[id] + length > romBytes.Length)
                    Fail("This dump is damaged: its file table points at data past the end of the file " +
                         "(file " + id + ", '" + pair.Value + "'). Re-dump the cartridge.");
                needBytes += length;
            }
            // free space: skipped, never fatal, where the drive cannot be measured
            try
            {
                string root = Path.GetPathRoot(Path.GetFullPath(destination));
                long free = new DriveInfo(root).AvailableFreeSpace;
                long want = needBytes + 16L * 1048576;
                if (free < want)
                    Fail("There is not enough free space on " + root + " to unpack the game data. About " +
                         Math.Round(want / 1048576.0) + " MB is needed and " + Math.Round(free / 1048576.0) +
                         " MB is free. Free up some space and try again.");
            }
            catch (ArgumentException) { log("    (could not measure free disk space here; carrying on)"); }
            catch (IOException) { log("    (could not measure free disk space here; carrying on)"); }

            log("Writing " + paths.Count + " files");
            var madeDirs = new HashSet<string>();
            int written = 0, total = paths.Count;
            long bytes = 0;
            foreach (var pair in paths)
            {
                int id = pair.Key;
                string target = Path.Combine(filesRoot, pair.Value.Replace('/', '\\'));
                string dir = Path.GetDirectoryName(target);
                if (madeDirs.Add(dir)) Directory.CreateDirectory(dir);
                long length = fatEnd[id] - fatStart[id];
                using (var f = new FileStream(target, FileMode.Create, FileAccess.Write))
                    f.Write(romBytes, (int)fatStart[id], (int)length);
                written++;
                bytes += length;
                if (written % 500 == 0)
                    progress(2 + 68 * written / total, "Unpacking file " + written + " of " + total);
            }
            log("    " + written + " files, " + Math.Round(bytes / 1048576.0, 1).ToString(Inv) + " MB");
            progress(70, "Unpacked " + written + " files");

            // ------------------------------------------------------ overlay 0's handle table
            // Overlay table entries are 32 bytes: id, ram address, size, bss, two
            // static initialiser bounds, file id, then (compressed size | flags << 24).
            if (ovtSize < 32) Fail("This dump has no ARM9 overlays. Re-dump the cartridge.");
            long ov0Ram = U32(romBytes, ovtOffset + 4);
            long ov0FileId = U32(romBytes, ovtOffset + 24);
            int ov0Flags = (int)(U32(romBytes, ovtOffset + 28) >> 24);
            if (ov0FileId >= fatCount)
                Fail("This dump is damaged: its overlay table points at a file its file allocation table " +
                     "does not have. Re-dump the cartridge.");
            byte[] ov0 = Cut(romBytes, fatStart[ov0FileId], fatEnd[ov0FileId] - fatStart[ov0FileId]);
            progress(71, "Decompressing overlay 0");
            if ((ov0Flags & 1) != 0)
            {
                try { ov0 = ExpandBlz(ov0); }
                catch (InvalidDataException e)
                {
                    Fail("Could not read overlay 0 of this dump (" + e.Message + "). Re-dump the cartridge.");
                }
            }
            long tableOffset = HandleTableAddress - ov0Ram;
            if (tableOffset < 0 || tableOffset + HandleCount * 4 > ov0.Length)
                Fail("This dump is Super Mario 64 DS but not a revision this build knows: its overlay 0 has " +
                     "no asset-handle table where one is expected.");

            // the script's lookups were PowerShell hashtables: case-insensitive
            var idByPath = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (var pair in paths) idByPath[pair.Value] = pair.Key;

            log("Reading the asset-handle table out of overlay 0");
            progress(72, "Reading the asset-handle table");
            var handleRows = new StringBuilder("handle\thex_handle\tfile_id\thex_file_id\tpath\tkind\tsize\n");
            for (int h = 0; h < HandleCount; h++)
            {
                if (h % 400 == 0 && h > 0)
                    progress(72 + 8 * h / HandleCount, "Cataloguing asset handle " + h + " of " + HandleCount);
                long stringOffset = U32(ov0, tableOffset + h * 4) - ov0Ram;
                if (stringOffset < 0 || stringOffset >= ov0.Length)
                    Fail("This dump is Super Mario 64 DS but not a revision this build knows: asset handle " +
                         h + " points outside overlay 0.");
                int end = Array.IndexOf(ov0, (byte)0, (int)stringOffset);
                if (end < 0) end = ov0.Length;
                string assetPath = Encoding.ASCII.GetString(ov0, (int)stringOffset, end - (int)stringOffset);
                int id;
                if (!idByPath.TryGetValue(assetPath, out id))
                    Fail("This dump is Super Mario 64 DS but not a revision this build knows: asset handle " +
                         h + " names '" + assetPath + "', which its filesystem does not have.");
                handleRows.AppendFormat(Inv, "{0}\t0x{0:x4}\t{1}\t0x{1:x4}\t{2}\t{3}\t{4}\n",
                                        h, id, assetPath, Kind(assetPath), fatEnd[id] - fatStart[id]);
            }
            var fileRows = new StringBuilder("file_id\thex_id\tpath\tkind\tsize\n");
            foreach (var pair in paths)
                fileRows.AppendFormat(Inv, "{0}\t0x{0:x4}\t{1}\t{2}\t{3}\n",
                                      pair.Key, pair.Value, Kind(pair.Value), fatEnd[pair.Key] - fatStart[pair.Key]);
            string assetsDir = Path.Combine(destination, "build", "assets");
            Directory.CreateDirectory(assetsDir);
            File.WriteAllText(Path.Combine(assetsDir, "files.tsv"), fileRows.ToString(), Utf8);
            File.WriteAllText(Path.Combine(assetsDir, "handles.tsv"), handleRows.ToString(), Utf8);
            log("    " + paths.Count + " files and " + HandleCount + " handles catalogued");

            // ------------------------------------------------------ the NitroFS tables
            // The game runs the cartridge's own name walker (port\hal\fs_names.cpp), so
            // these are the card's actual bytes, and nitrofs.tsv says where they were.
            if (fntSize == 0)
                Fail("This dump has an empty file name table, so the game could not look up any of its " +
                     "files. Re-dump the cartridge.");
            if (fatSize == 0)
                Fail("This dump has an empty file allocation table, so the game could not look up any of " +
                     "its files. Re-dump the cartridge.");
            File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_fnt.bin"), Cut(romBytes, fntOffset, fntSize));
            File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_fat.bin"), Cut(romBytes, fatOffset, fatSize));
            if (ovtSize != 0)
                File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_ovt9.bin"), Cut(romBytes, ovtOffset, ovtSize));
            if (ovt7Size != 0)
                File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_ovt7.bin"), Cut(romBytes, ovt7Offset, ovt7Size));
            File.WriteAllText(Path.Combine(assetsDir, "nitrofs.tsv"), string.Format(Inv,
                "key\tvalue\nfnt_offset\t{0}\nfnt_size\t{1}\nfat_offset\t{2}\nfat_size\t{3}\n" +
                "ovt9_offset\t{4}\novt9_size\t{5}\novt7_offset\t{6}\novt7_size\t{7}\n",
                fntOffset, fntSize, fatOffset, fatSize, ovtOffset, ovtSize, ovt7Offset, ovt7Size), Utf8);
            log("    name table " + fntSize + " bytes, allocation table " + fatSize + " bytes, overlay table " +
                ovtSize + " bytes");
            progress(80, "Rebuilding romdata.bin");

            // ------------------------------------------------------ romdata.bin
            // The game ships with every code-side table zeroed and fills them from
            // romdata.bin: decompress arm9 and the overlays the recipe names, copy each
            // range, hash-check the result. Ranges past an image's end stay zero (bss).
            const string damagedRecipe = "This builder's romdata.recipe.tsv is damaged, so the game data " +
                                         "cannot be rebuilt. Download the builder again.";
            string[] recipe = File.ReadAllLines(recipePath);
            Match head = recipe.Length >= 2 ? Regex.Match(recipe[0], "^# romdata-recipe v1 ([0-9a-f]{64}) (\\d+)$")
                                            : Match.Empty;
            if (!head.Success) Fail(damagedRecipe);
            string wantSha = head.Groups[1].Value;
            int blobTotal = int.Parse(head.Groups[2].Value, Inv);

            var needed = new List<string>();
            foreach (var line in recipe)
            {
                if (line.StartsWith("#") || line.Trim().Length == 0) continue;
                var f = line.Split('\t');
                if (f.Length < 4) Fail(damagedRecipe);
                if (!needed.Contains(f[2])) needed.Add(f[2]);
            }
            var ovByName = new Dictionary<string, long[]>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < ovtSize / 32; i++)
            {
                long e = ovtOffset + i * 32L;
                ovByName["ov" + U32(romBytes, e).ToString("d3", Inv)] =
                    new long[] { U32(romBytes, e + 24), U32(romBytes, e + 28) >> 24 };
            }

            log("Rebuilding romdata.bin (" + needed.Count + " images to decompress)");
            var images = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            for (int n = 0; n < needed.Count; n++)
            {
                string src = needed[n];
                progress(80 + 17 * n / needed.Count, "Decompressing " + src + " (" + (n + 1) + " of " +
                         needed.Count + ")");
                byte[] raw;
                bool packed;
                if (src == "arm9")
                {
                    // program header: rom offset at 0x20, size at 0x2C; stored BLZ like the overlays
                    raw = Cut(romBytes, U32(romBytes, 0x20), U32(romBytes, 0x2C));
                    packed = true;
                }
                else
                {
                    long[] ov;
                    if (!ovByName.TryGetValue(src, out ov))
                        Fail("This dump is Super Mario 64 DS but not a revision this build knows: it has no " +
                             "overlay '" + src + "'.");
                    long fileId = ov[0];
                    if (fileId >= fatCount)
                        Fail("This dump is damaged: its overlay table points at a file its file allocation " +
                             "table does not have (overlay '" + src + "'). Re-dump the cartridge.");
                    long length = fatEnd[fileId] - fatStart[fileId];
                    if (length < 0 || fatStart[fileId] + length > romBytes.Length)
                        Fail("This dump is damaged: overlay '" + src + "' runs past the end of the file. " +
                             "Re-dump the cartridge.");
                    raw = Cut(romBytes, fatStart[fileId], length);
                    packed = (ov[1] & 1) != 0;
                }
                if (packed)
                {
                    try { raw = ExpandBlz(raw); }
                    catch (InvalidDataException e)
                    {
                        Fail(src == "arm9"
                            ? "Could not read this dump's program data (" + e.Message + "). Re-dump the cartridge."
                            : "Could not read overlay '" + src + "' of this dump (" + e.Message + "). " +
                              "Re-dump the cartridge.");
                    }
                }
                images[src] = raw;
                log("    " + src + " (" + Math.Round(raw.Length / 1024.0) + " KB)");
            }

            progress(98, "Assembling and checking romdata.bin");
            var blob = new byte[blobTotal];
            foreach (var line in recipe)
            {
                if (line.StartsWith("#") || line.Trim().Length == 0) continue;
                var f = line.Split('\t');
                int off, size, srcOff;
                byte[] img;
                if (!int.TryParse(f[0], NumberStyles.Integer, Inv, out off) ||
                    !int.TryParse(f[1], NumberStyles.Integer, Inv, out size) ||
                    !int.TryParse(f[3], NumberStyles.Integer, Inv, out srcOff) ||
                    !images.TryGetValue(f[2], out img) ||
                    off < 0 || size < 0 || srcOff < 0 || (long)off + size > blob.Length)
                    throw new BuildError(damagedRecipe);
                int have = Math.Min(size, Math.Max(0, img.Length - srcOff));
                if (have > 0) Array.Copy(img, srcOff, blob, off, have);
            }
            string sha;
            using (var s = SHA256.Create())
                sha = BitConverter.ToString(s.ComputeHash(blob)).Replace("-", "").ToLowerInvariant();
            if (sha != wantSha)
                // a European dump that still did not rebuild: not a byte-for-byte retail copy
                Fail("This is a European dump (game code " + code + "), but its contents do not match the " +
                     "cartridge this build was made from. That usually means the file has been modified " +
                     "(a patch, a translation or a trimmed dump) or the dump is faulty. Use an untouched " +
                     "dump of your own retail cartridge, then try again.");
            File.WriteAllBytes(Path.Combine(assetsDir, "romdata.bin"), blob);
            log("    romdata.bin verified (" + blobTotal + " bytes, checksum OK)");
            // the game checks romdata.bin at boot against the manifest
            File.Copy(manifestPath, Path.Combine(assetsDir, "romdata.manifest"), true);
            progress(100, "Done");
        }

        static string Kind(string path)
        {
            switch (Path.GetExtension(path).ToLowerInvariant())
            {
                case ".bca": return "animation";
                case ".bmd": return "model";
                case ".btp": return "texture-sequence";
                case ".kcl": return "collision";
                case ".narc": return "archive";
                case ".sdat": return "sound-archive";
                case ".bin": return "data";
                default: return "file";
            }
        }

        // The overlays' and the program's back-to-front LZ ("BLZ"): both streams
        // run backwards from the end; anything before the compressed run is plain.
        static byte[] ExpandBlz(byte[] data)
        {
            try
            {
                return ExpandBlzOrThrow(data);
            }
            catch (IndexOutOfRangeException)
            {
                throw new InvalidDataException("it runs past its own end");
            }
        }

        static byte[] ExpandBlzOrThrow(byte[] data)
        {
            if (data.Length < 8) return data;
            uint footer = BitConverter.ToUInt32(data, data.Length - 8);
            uint extraSize = BitConverter.ToUInt32(data, data.Length - 4);
            if (extraSize == 0) return data;   // flagged, but stored plain
            int headerLen = (int)(footer >> 24);
            int compressedLen = (int)(footer & 0xFFFFFF);
            if (headerLen < 8 || headerLen > data.Length || compressedLen > data.Length)
                throw new InvalidDataException("an unreadable compression header");
            int passthrough = data.Length - compressedLen;
            int compLen = compressedLen - headerLen;
            int outLen = (int)(data.Length + extraSize - passthrough);
            var output = new byte[outLen];
            int read = 0, done = 0, flags = 0, mask = 1;
            while (done < outLen)
            {
                if (mask == 1)
                {
                    if (read >= compressedLen) throw new InvalidDataException("it ended mid-stream");
                    flags = data[passthrough + compLen - 1 - read++];
                    mask = 0x80;
                }
                else
                {
                    mask >>= 1;
                }
                if ((flags & mask) != 0)
                {
                    int b1 = data[passthrough + compLen - 1 - read++];
                    int b2 = data[passthrough + compLen - 1 - read++];
                    int length = (b1 >> 4) + 3;
                    int disp = (((b1 & 0x0F) << 8) | b2) + 3;
                    if (disp > done)
                    {
                        if (done < 2) throw new InvalidDataException("it refers back to data not there yet");
                        disp = 2;
                    }
                    int from = done - disp;
                    for (int i = 0; i < length; i++)
                        output[outLen - 1 - done++] = output[outLen - 1 - from++];
                }
                else
                {
                    output[outLen - 1 - done++] = data[passthrough + compLen - 1 - read++];
                }
            }
            var result = new byte[passthrough + outLen];
            Array.Copy(data, 0, result, 0, passthrough);
            Array.Copy(output, 0, result, passthrough, outLen);
            return result;
        }
    }
}
