// The American or Japanese cartridge alone (sm64ds_builder.py's
// standalone_extract, made to give the same files to the bit): what
// Extract.Run takes out of a European dump, made out of the American (ASME) or
// Japanese (ASMJ) one instead, by builder-files\region-ASME.tsv
// (region-ASMJ.tsv), which region_kit.py makes from all three cartridges. It
// says where each piece of romdata.bin (the European layout) is in that
// cartridge's program, the numbers the European layout has in place of the
// cartridge's own (its pointers and file numbers), the European file system's
// names, and which of the cartridge's files each European file is. The
// format is in sm64ds_builder.py. Languages.ApplyRegion then makes the edition
// as it does with the two dumps.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace Sm64dsEnhanced
{
    static class Regions
    {
        static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
        static readonly UTF8Encoding Utf8 = new UTF8Encoding(false);
        const uint HandleTableAddress = 0x020BD4B8;
        const int HandleCount = 0x80A;
        const uint Ov0Address = 0x020AA420;   // overlay 0 in the European program

        // the edition a cartridge alone makes; null: it needs a European dump beside it
        public static string Edition(string gameCode)
        {
            return gameCode == "ASME" ? "en-us" : gameCode == "ASMJ" ? "ja" : null;
        }

        static void Fail(string text) { throw new BuildError(text); }
        static uint U32(byte[] b, long o) { return BitConverter.ToUInt32(b, (int)o); }
        static ushort U16(byte[] b, long o) { return BitConverter.ToUInt16(b, (int)o); }
        static string Sha(byte[] b) { return Languages.Hex(Languages.Sha(b, 0, b.Length)); }

        static byte[] Cut(byte[] b, long offset, long length)
        {
            var r = new byte[length];
            if (length > 0) Array.Copy(b, offset, r, 0, length);
            return r;
        }

        // ---- the cartridge's program, decompressed ------------------------------

        public static Dictionary<string, byte[]> Modules(byte[] rb)
        {
            var o = new Dictionary<string, byte[]>();
            uint a9 = U32(rb, 0x20);
            try
            {
                o["arm9"] = Extract.ExpandBlz(Cut(rb, a9, U32(rb, 0x2C)));
            }
            catch (InvalidDataException e)
            {
                Fail("Could not read this dump's program data (" + e.Message + "). Re-dump the cartridge.");
            }
            uint fat = U32(rb, 0x48), table = U32(rb, 0x50), size = U32(rb, 0x54);
            for (uint i = 0; i < size / 32; i++)
            {
                uint e = table + i * 32;
                uint ident = U32(rb, e), f = U32(rb, e + 24), flags = U32(rb, e + 28);
                uint start = U32(rb, fat + f * 8), end = U32(rb, fat + f * 8 + 4);
                if (f >= U32(rb, 0x4C) / 8 || end < start || end > rb.Length)
                    Fail("This dump is damaged: an overlay runs past the end of the file. Re-dump the cartridge.");
                try
                {
                    var raw = Cut(rb, start, end - start);
                    o["ov" + ident.ToString("D3", Inv)] = (flags & 0x1000000) != 0 ? Extract.ExpandBlz(raw) : raw;
                }
                catch (InvalidDataException x)
                {
                    Fail("Could not read overlay " + ident + " of this dump (" + x.Message + "). Re-dump the cartridge.");
                }
            }
            return o;
        }

        // ---- file name tables -----------------------------------------------------

        class Dir
        {
            public int First, Parent;
            public List<Ent> Ents = new List<Ent>();
        }

        class Ent
        {
            public bool IsDir;
            public string Name;
            public int Sub;
        }

        static byte[] FntBuild(List<Dir> dirs)
        {
            var subs = new List<byte[]>();
            foreach (var d in dirs)
            {
                var b = new MemoryStream();
                foreach (var e in d.Ents)
                {
                    var nb = Encoding.ASCII.GetBytes(e.Name);
                    b.WriteByte((byte)(nb.Length | (e.IsDir ? 0x80 : 0)));
                    b.Write(nb, 0, nb.Length);
                    if (e.IsDir) b.Write(BitConverter.GetBytes((ushort)e.Sub), 0, 2);
                }
                b.WriteByte(0);
                subs.Add(b.ToArray());
            }
            var o = new MemoryStream();
            uint off = (uint)(dirs.Count * 8);
            for (int i = 0; i < dirs.Count; i++)
            {
                o.Write(BitConverter.GetBytes(off), 0, 4);
                o.Write(BitConverter.GetBytes((ushort)dirs[i].First), 0, 2);
                o.Write(BitConverter.GetBytes((ushort)dirs[i].Parent), 0, 2);
                off += (uint)subs[i].Length;
            }
            foreach (var s in subs) o.Write(s, 0, s.Length);
            return o.ToArray();
        }

        // [(file id, path)] of a file name table's files
        static List<KeyValuePair<int, string>> FntWalk(List<Dir> dirs)
        {
            var o = new List<KeyValuePair<int, string>>();
            var queue = new Queue<KeyValuePair<int, string>>();
            queue.Enqueue(new KeyValuePair<int, string>(0, ""));
            while (queue.Count > 0)
            {
                var item = queue.Dequeue();
                int fid = dirs[item.Key].First;
                foreach (var e in dirs[item.Key].Ents)
                {
                    if (e.IsDir) queue.Enqueue(new KeyValuePair<int, string>(e.Sub & 0xFFF, item.Value + e.Name + "/"));
                    else o.Add(new KeyValuePair<int, string>(fid++, item.Value + e.Name));
                }
            }
            return o;
        }

        static List<Dir> FntDirs(byte[] rb)
        {
            long fntOff = U32(rb, 0x40), fntSize = U32(rb, 0x44);
            var fnt = Cut(rb, fntOff, fntSize);
            var dirs = new List<Dir>();
            for (int d = 0; d < U16(fnt, 6); d++)
            {
                var dir = new Dir { First = U16(fnt, d * 8 + 4), Parent = U16(fnt, d * 8 + 6) };
                int o = (int)U32(fnt, d * 8);
                while (fnt[o] != 0)
                {
                    int t = fnt[o];
                    var e = new Ent { Name = Encoding.ASCII.GetString(fnt, o + 1, t & 0x7F) };
                    o += 1 + (t & 0x7F);
                    if ((t & 0x80) != 0)
                    {
                        e.IsDir = true;
                        e.Sub = U16(fnt, o);
                        o += 2;
                    }
                    dir.Ents.Add(e);
                }
                dirs.Add(dir);
            }
            return dirs;
        }

        static Dir DirOf(string[] f, int from)
        {
            var d = new Dir { First = int.Parse(f[from], Inv), Parent = int.Parse(f[from + 1], Inv) };
            for (int i = from + 2; i < f.Length; i++)
            {
                int c = f[i].IndexOf(':');
                string k = f[i].Substring(0, c), rest = f[i].Substring(c + 1);
                if (k == "d")
                {
                    int r = rest.LastIndexOf(':');
                    d.Ents.Add(new Ent { IsDir = true, Name = rest.Substring(0, r), Sub = int.Parse(rest.Substring(r + 1), Inv) });
                }
                else d.Ents.Add(new Ent { Name = rest });
            }
            return d;
        }

        static byte[] RbFile(byte[] rb, int fid)
        {
            uint fat = U32(rb, 0x48);
            uint start = U32(rb, fat + fid * 8L), end = U32(rb, fat + fid * 8L + 4);
            if (end < start || end > rb.Length)
                Fail("This dump is damaged: its file table points past the end of the file. Re-dump the cartridge.");
            return Cut(rb, start, end - start);
        }

        // ---- archives and the texture bank ----------------------------------------

        static byte[] NarcBuild(List<Dir> dirs, byte[][] data)
        {
            var btaf = new MemoryStream();
            var gmif = new MemoryStream();
            btaf.Write(Encoding.ASCII.GetBytes("BTAF"), 0, 4);
            btaf.Write(BitConverter.GetBytes((uint)(12 + 8 * data.Length)), 0, 4);
            btaf.Write(BitConverter.GetBytes((ushort)data.Length), 0, 2);
            btaf.Write(new byte[2], 0, 2);
            foreach (var b in data)
            {
                btaf.Write(BitConverter.GetBytes((uint)gmif.Length), 0, 4);
                btaf.Write(BitConverter.GetBytes((uint)(gmif.Length + b.Length)), 0, 4);
                gmif.Write(b, 0, b.Length);
                for (int i = 0; i < (-b.Length & 3); i++) gmif.WriteByte(0xFF);
            }
            var names = new MemoryStream();
            var fnt = FntBuild(dirs);
            names.Write(fnt, 0, fnt.Length);
            for (int i = 0; i < (-fnt.Length & 3); i++) names.WriteByte(0xFF);
            var body = new MemoryStream();
            btaf.WriteTo(body);
            body.Write(Encoding.ASCII.GetBytes("BTNF"), 0, 4);
            body.Write(BitConverter.GetBytes((uint)(8 + names.Length)), 0, 4);
            names.WriteTo(body);
            body.Write(Encoding.ASCII.GetBytes("GMIF"), 0, 4);
            body.Write(BitConverter.GetBytes((uint)(8 + gmif.Length)), 0, 4);
            gmif.WriteTo(body);
            var o = new MemoryStream();
            o.Write(new byte[] { (byte)'N', (byte)'A', (byte)'R', (byte)'C', 0xFE, 0xFF, 0x00, 0x01 }, 0, 8);
            o.Write(BitConverter.GetBytes((uint)(16 + body.Length)), 0, 4);
            o.Write(BitConverter.GetBytes((ushort)16), 0, 2);
            o.Write(BitConverter.GetBytes((ushort)3), 0, 2);
            body.WriteTo(o);
            return o.ToArray();
        }

        class Tex
        {
            public string Name;
            public int W, H;
            public uint Params;
            public byte[] Data;
        }

        static string CString(byte[] b, int at)
        {
            int e = Array.IndexOf(b, (byte)0, at);
            return Encoding.ASCII.GetString(b, at, e - at);
        }

        static void TexbankParts(byte[] bmd, Dictionary<string, Tex> tex, Dictionary<string, byte[]> pal)
        {
            var b = Face.Lz77(bmd);
            for (int i = 0; i < U32(b, 0x14); i++)
            {
                int e = (int)U32(b, 0x18) + i * 20;
                var t = new Tex { Name = CString(b, (int)U32(b, e)), W = U16(b, e + 12), H = U16(b, e + 14), Params = U32(b, e + 16) };
                t.Data = Cut(b, U32(b, e + 4), U32(b, e + 8));
                tex[t.Name] = t;
            }
            for (int i = 0; i < U32(b, 0x1C); i++)
            {
                int e = (int)U32(b, 0x20) + i * 16;
                pal[CString(b, (int)U32(b, e))] = Cut(b, U32(b, e + 4), U32(b, e + 8));
            }
        }

        static void Pad4(MemoryStream o) { while (o.Length % 4 != 0) o.WriteByte(0); }

        static void Put(MemoryStream o, long at, params uint[] words)
        {
            long keep = o.Position;
            o.Position = at;
            foreach (var w in words) o.Write(BitConverter.GetBytes(w), 0, 4);
            o.Position = keep;
        }

        // a texture bank (a BMD of one material) with these textures and palettes
        // in place of src's, laid out as the cartridge lays its out
        static byte[] TexbankBuild(byte[] src, List<Tex> textures, List<KeyValuePair<string, byte[]>> palettes)
        {
            var b = Face.Lz77(src);
            int matCount = (int)U32(b, 0x24), matAt = (int)U32(b, 0x28);
            var o = new MemoryStream();
            o.Write(b, 0, 0xE0);
            long texAt = o.Length;
            o.Write(new byte[20 * textures.Count], 0, 20 * textures.Count);
            var names = new List<long>();
            foreach (var t in textures)
            {
                names.Add(o.Length);
                var nb = Encoding.ASCII.GetBytes(t.Name);
                o.Write(nb, 0, nb.Length);
                o.WriteByte(0);
            }
            Pad4(o);
            long palAt = o.Length;
            o.Write(new byte[16 * palettes.Count], 0, 16 * palettes.Count);
            var pnames = new List<long>();
            foreach (var p in palettes)
            {
                pnames.Add(o.Length);
                var nb = Encoding.ASCII.GetBytes(p.Key);
                o.Write(nb, 0, nb.Length);
                o.WriteByte(0);
            }
            Pad4(o);
            long mats = o.Length;
            o.Write(b, matAt, 48 * matCount);
            for (int i = 0; i < matCount; i++)
            {
                int n = (int)U32(b, matAt + i * 48);
                Put(o, mats + i * 48, (uint)o.Length);
                int e = Array.IndexOf(b, (byte)0, n);
                o.Write(b, n, e + 1 - n);
            }
            Pad4(o);
            long dataAt = o.Length;
            for (int i = 0; i < textures.Count; i++)
            {
                var t = textures[i];
                Put(o, texAt + i * 20, (uint)names[i], (uint)o.Length, (uint)t.Data.Length,
                    (uint)(t.W | t.H << 16), t.Params);
                o.Write(t.Data, 0, t.Data.Length);
            }
            for (int i = 0; i < palettes.Count; i++)
            {
                var d = palettes[i].Value;
                Put(o, palAt + i * 16, (uint)pnames[i], (uint)o.Length, (uint)d.Length, 0xFFFFFFFF);
                o.Write(d, 0, d.Length);
            }
            Put(o, 0x14, (uint)textures.Count, (uint)texAt, (uint)palettes.Count, (uint)palAt, (uint)matCount, (uint)mats);
            Put(o, 0x38, (uint)dataAt);
            return Languages.Lz77Wrap(o.ToArray());
        }

        // ---- the kit --------------------------------------------------------------

        static List<string[]> Kit(string code)
        {
            var text = File.ReadAllText(Sm64dsEnhanced.Kit.Need("region-" + code + ".tsv"), Utf8).Replace("\r\n", "\n");
            var o = new List<string[]>();
            foreach (var line in text.Split('\n'))
                if (line.Trim().Length > 0 && !line.StartsWith("#")) o.Add(line.Split('\t'));
            return o;
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

        // the European module's bytes romdata.bin has (the rest zero), as one image
        static byte[] EuModule(byte[] blob, string recipePath, string module)
        {
            var rows = new List<int[]>();
            int size = 0;
            foreach (var line in File.ReadAllText(recipePath, Utf8).Replace("\r\n", "\n").Split('\n'))
            {
                if (line.Length == 0 || line.StartsWith("#")) continue;
                var f = line.Split('\t');
                if (f[2].ToLowerInvariant() != module) continue;
                var r = new[] { int.Parse(f[0], Inv), int.Parse(f[1], Inv), int.Parse(f[3], Inv) };
                rows.Add(r);
                size = Math.Max(size, r[2] + r[1]);
            }
            var img = new byte[size];
            foreach (var r in rows) Array.Copy(blob, r[0], img, r[2], r[1]);
            return img;
        }

        // ---- the build ------------------------------------------------------------

        // progress: 0..100, as Extract.Run's
        public static void Run(string rom, string recipePath, string manifestPath, string destination,
                               Action<string> log, Action<int, string> progress)
        {
            byte[] rb;
            try
            {
                rb = File.ReadAllBytes(rom);
            }
            catch (IOException e)
            {
                Fail("The dump could not be read: " + e.Message);
                return;
            }
            string code = Encoding.ASCII.GetString(rb, 0x0C, 4);
            string language = Edition(code), who = Languages.Region(code);
            log("Super Mario 64 DS (" + code + ", " + (code == "ASMJ" ? "Japan" : "USA") + "), " +
                Math.Round(rb.Length / 1048576.0) + " MB");
            var lines = Kit(code);
            var head = new Dictionary<string, string[]>();
            foreach (var f in lines)
                if (f[0] == "code" || f[0] == "layout" || f[0] == "blob" || f[0] == "leaves" || f[0] == "ovt") head[f[0]] = f;
            var damaged = new BuildError("This builder's region-" + code + ".tsv is damaged. Download the builder again.");
            if (!head.ContainsKey("code") || head["code"].Length != 2 || head["code"][1] != code ||
                !head.ContainsKey("layout") || !head.ContainsKey("blob"))
                throw damaged;
            progress(5, "Decompressing the program");
            var mods = Modules(rb);
            foreach (var f in lines)
                if (f[0] == "img" && (!mods.ContainsKey(f[1]) || mods[f[1]].Length != int.Parse(f[2], Inv) || Sha(mods[f[1]]) != f[3]))
                    throw Languages.NotThatCartridge(language);

            // romdata.bin
            progress(20, "Rebuilding romdata.bin");
            int total = int.Parse(head["layout"][2], Inv);
            var blob = new byte[total];
            try
            {
                foreach (var f in lines)
                {
                    if (f[0] == "r")
                    {
                        int off = int.Parse(f[1], Inv), n = int.Parse(f[2], Inv), from = int.Parse(f[4], Inv);
                        var m = mods[f[3]];
                        if (from + n > m.Length || off + n > total) throw Languages.NotThatCartridge(language);
                        Array.Copy(m, from, blob, off, n);
                    }
                    else if (f[0] == "w")
                    {
                        int off = int.Parse(f[1], Inv), i = 0;
                        foreach (var w in f[2].Split(' '))
                        {
                            Array.Copy(BitConverter.GetBytes(uint.Parse(w, NumberStyles.HexNumber, Inv)), 0, blob, off + i * 4, 4);
                            i++;
                        }
                    }
                    else if (f[0] == "b")
                    {
                        int off = int.Parse(f[1], Inv);
                        if (off + f[2].Length / 2 > total) throw Languages.NotThatCartridge(language);
                        for (int i = 0; i < f[2].Length / 2; i++)
                            blob[off + i] = byte.Parse(f[2].Substring(i * 2, 2), NumberStyles.HexNumber, Inv);
                    }
                }
            }
            catch (FormatException) { throw damaged; }
            catch (KeyNotFoundException) { throw damaged; }
            catch (IndexOutOfRangeException) { throw damaged; }
            catch (ArgumentException) { throw damaged; }
            if (Sha(blob) != head["blob"][1]) throw Languages.NotThatCartridge(language);
            log("    romdata.bin made (" + total + " bytes, checksum OK)");
            var man = File.ReadAllText(manifestPath, Utf8).Replace("\r\n", "\n").Split('\n');
            int count = man.Length > 0 && man[man.Length - 1].Length == 0 ? man.Length - 1 : man.Length;
            var mh = count > 0 ? Regex.Match(man[0], "^# romdata-manifest v1 ([0-9a-f]{64}) (\\d+)$") : Match.Empty;
            if (!mh.Success || mh.Groups[1].Value != head["layout"][1] || int.Parse(mh.Groups[2].Value, Inv) != total)
                Fail("This builder's region-" + code + ".tsv belongs to another build of the game. " +
                     "Download the builder again.");
            // the manifest of this blob, its layout the European one's (game/language.cpp)
            var mo = new StringBuilder("# romdata-manifest v1 " + Sha(blob) + " " + total + " layout " + mh.Groups[1].Value + "\n");
            for (int i = 1; i < count; i++)
            {
                var f = man[i].Split('\t');
                if (f[0] == "part" && f.Length == 5)
                    f[4] = Sha(Cut(blob, int.Parse(f[2], Inv), int.Parse(f[3], Inv)));
                else if (f[0] == "sym" && f.Length == 6)
                    f[5] = Sha(Cut(blob, int.Parse(f[3], Inv), int.Parse(f[4], Inv)));
                mo.Append(string.Join("\t", f)).Append('\n');
            }
            string assetsDir = Path.Combine(destination, "build", "assets");
            Directory.CreateDirectory(assetsDir);
            File.WriteAllBytes(Path.Combine(assetsDir, "romdata.bin"), blob);
            File.WriteAllText(Path.Combine(assetsDir, "romdata.manifest"), mo.ToString(), Utf8);

            // the cartridge's files
            progress(40, "Reading the file tables");
            uint fntOff = U32(rb, 0x40), ovtOff = U32(rb, 0x50), ovtSize = U32(rb, 0x54);
            uint fatOff = U32(rb, 0x48);
            var have = new Dictionary<string, byte[]>();
            foreach (var kv in FntWalk(FntDirs(rb))) have[kv.Value] = RbFile(rb, kv.Key);
            var narcs = new Dictionary<string, KeyValuePair<Dictionary<string, int>, List<byte[]>>>();
            Func<string, string, byte[]> member = delegate (string narc, string name)
            {
                if (!narcs.ContainsKey(narc))
                {
                    Dictionary<string, int> s;
                    var names = Languages.NarcMembers(have[narc], out s);
                    narcs[narc] = new KeyValuePair<Dictionary<string, int>, List<byte[]>>(names, Languages.NarcFiles(have[narc], s));
                }
                var nn = narcs[narc];
                return nn.Key.ContainsKey(name) ? nn.Value[nn.Key[name]] : null;
            };
            using (var used = SHA256.Create())
            {
                foreach (var f in lines)
                {
                    if (f[0] != "F" && f[0] != "M") continue;
                    int h = f[0] == "F" ? 2 : 3;
                    byte[] b;
                    if (f[h] == "=" || f[h] == "t") b = have.ContainsKey(f[h + 1]) ? have[f[h + 1]] : null;
                    else if (f[h] == "m") b = have.ContainsKey(f[h + 1]) ? member(f[h + 1], f[h + 2]) : null;
                    else continue;
                    if (b == null) Fail("The " + who + " dump has no " + f[f.Length - 1] + ". Re-dump the cartridge.");
                    using (var s = SHA256.Create())
                    {
                        var d = s.ComputeHash(b);
                        used.TransformBlock(d, 0, d.Length, null, 0);
                    }
                }
                used.TransformFinalBlock(new byte[0], 0, 0);
                if (Languages.Hex(used.Hash) != head["leaves"][1] || Sha(Cut(rb, ovtOff, ovtSize)) != head["ovt"][1])
                    throw Languages.NotThatCartridge(language);
            }

            // the European file system
            progress(60, "Making the European file system");
            var dirs = new List<Dir>();
            var adirs = new Dictionary<string, List<Dir>>();
            var howF = new Dictionary<string, string[]>();
            var howM = new Dictionary<string, string[]>();
            var tex = new Dictionary<string, List<string[]>>();
            var pal = new Dictionary<string, List<string[]>>();
            foreach (var f in lines)
            {
                if (f[0] == "D") dirs.Add(DirOf(f, 1));
                else if (f[0] == "N")
                {
                    if (!adirs.ContainsKey(f[1])) adirs[f[1]] = new List<Dir>();
                    adirs[f[1]].Add(DirOf(f, 2));
                }
                else if (f[0] == "F") howF[f[1]] = Rest(f, 2);
                else if (f[0] == "M") howM[f[1] + ":" + f[2]] = Rest(f, 3);
                else if (f[0] == "T" || f[0] == "P")
                {
                    var into = f[0] == "T" ? tex : pal;
                    if (!into.ContainsKey(f[1])) into[f[1]] = new List<string[]>();
                    into[f[1]].Add(Rest(f, 2));
                }
            }
            var done = new Dictionary<string, byte[]>();
            Func<string, string[], byte[]> make = null;
            make = delegate (string key, string[] how)
            {
                byte[] b;
                if (done.TryGetValue(key, out b)) return b;
                switch (how[0])
                {
                    case "=": b = have[how[1]]; break;
                    case "m": b = member(how[1], how[2]); break;
                    case "c": b = make(how[1], how[1].Contains(":") ? howM[how[1]] : howF[how[1]]); break;
                    case "z":
                        var raw = new byte[int.Parse(how[2], Inv)];
                        if (how[1] == "lz77") b = Languages.Lz77Wrap(raw);
                        else if (how[1] == "lz10") b = Languages.Lz10Wrap(raw);
                        else if (how[1] == "raw") b = raw;
                        else throw damaged;
                        break;
                    case "t":
                        var st = new Dictionary<string, Tex>();
                        var sp = new Dictionary<string, byte[]>();
                        TexbankParts(have[how[1]], st, sp);
                        var ts = new List<Tex>();
                        foreach (var t in tex[key])
                            ts.Add(new Tex { Name = t[0], W = int.Parse(t[2], Inv), H = int.Parse(t[3], Inv),
                                             Params = uint.Parse(t[4], NumberStyles.HexNumber, Inv),
                                             Data = t[5] != "-" ? st[t[5]].Data : new byte[int.Parse(t[1], Inv)] });
                        var ps = new List<KeyValuePair<string, byte[]>>();
                        foreach (var p in pal[key])
                        {
                            int size = int.Parse(p[1], Inv);
                            ps.Add(new KeyValuePair<string, byte[]>(p[0], p[2] != "-" ? Cut(sp[p[2]], 0, Math.Min(size, sp[p[2]].Length)) : new byte[size]));
                        }
                        b = TexbankBuild(have[how[1]], ts, ps);
                        break;
                    case "A":
                        var fl = FntWalk(adirs[key]);
                        var data = new byte[fl.Count][];
                        foreach (var kv in fl)
                        {
                            string mk = key + ":" + kv.Value;
                            data[kv.Key - adirs[key][0].First] = make(mk, howM[mk]);
                        }
                        b = NarcBuild(adirs[key], data);
                        break;
                    default: throw damaged;
                }
                if (b == null) throw damaged;
                done[key] = b;
                return b;
            };
            var eu = FntWalk(dirs);
            var files = new Dictionary<string, byte[]>();
            string filesRoot = Path.Combine(destination, "extracted", "dsd", "files");
            long bytes = 0;
            try
            {
                foreach (var kv in eu)
                {
                    if (!howF.ContainsKey(kv.Value)) throw damaged;
                    files[kv.Value] = make(kv.Value, howF[kv.Value]);
                }
            }
            catch (KeyNotFoundException) { throw damaged; }
            catch (FormatException) { throw damaged; }
            var madeDirs = new HashSet<string>();
            foreach (var kv in files)
            {
                string target = Path.Combine(filesRoot, kv.Key.Replace('/', '\\'));
                string dir = Path.GetDirectoryName(target);
                if (madeDirs.Add(dir)) Directory.CreateDirectory(dir);
                File.WriteAllBytes(target, kv.Value);
                bytes += kv.Value.Length;
            }
            log("    " + files.Count + " files, " + Math.Round(bytes / 1048576.0, 1).ToString(Inv) + " MB");

            // the tables: the overlays' entries as the cartridge's, then the files
            progress(80, "Writing the file tables");
            var fnt = FntBuild(dirs);
            int first = dirs[0].First;
            int fcount = first + eu.Count;
            var fat = new byte[fcount * 8];
            Array.Copy(rb, fatOff, fat, 0, first * 8);
            uint newFat = (uint)((fntOff + fnt.Length + 0x1FF) & ~0x1FF);
            uint at = (uint)((newFat + fcount * 8 + 0x1FF) & ~0x1FF);
            var byId = new SortedDictionary<int, string>();
            foreach (var kv in eu) byId[kv.Key] = kv.Value;
            foreach (var kv in byId)
            {
                uint len = (uint)files[kv.Value].Length;
                Array.Copy(BitConverter.GetBytes(at), 0, fat, kv.Key * 8, 4);
                Array.Copy(BitConverter.GetBytes(at + len), 0, fat, kv.Key * 8 + 4, 4);
                at = (at + len + 0x1FF) & ~0x1FFu;
            }
            if (at > 0x8000000) Fail("The " + who + " files don't fit the cartridge's address space.");
            File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_fnt.bin"), fnt);
            File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_fat.bin"), fat);
            File.WriteAllBytes(Path.Combine(assetsDir, "nitrofs_ovt9.bin"), Cut(rb, ovtOff, ovtSize));
            File.WriteAllText(Path.Combine(assetsDir, "nitrofs.tsv"), string.Format(Inv,
                "key\tvalue\nfnt_offset\t{0}\nfnt_size\t{1}\nfat_offset\t{2}\nfat_size\t{3}\novt9_offset\t{4}\n" +
                "ovt9_size\t{5}\novt7_offset\t0\novt7_size\t0\n", fntOff, fnt.Length, newFat, fat.Length, ovtOff,
                ovtSize), Utf8);
            var fileRows = new StringBuilder("file_id\thex_id\tpath\tkind\tsize\n");
            foreach (var kv in byId)
                fileRows.Append(string.Format(Inv, "{0}\t0x{0:x4}\t{1}\t{2}\t{3}\n", kv.Key, kv.Value, Kind(kv.Value),
                                              files[kv.Value].Length));
            // the handles: overlay 0's table (romdata.bin's first part) and its names
            var ov0 = EuModule(blob, recipePath, "ov000");
            var idByPath = new Dictionary<string, int>();
            foreach (var kv in eu) idByPath[kv.Value.ToLowerInvariant()] = kv.Key;
            var handleRows = new StringBuilder("handle\thex_handle\tfile_id\thex_file_id\tpath\tkind\tsize\n");
            for (int h = 0; h < HandleCount; h++)
            {
                int so = (int)(U32(ov0, HandleTableAddress - Ov0Address + h * 4) - Ov0Address);
                int end = Array.IndexOf(ov0, (byte)0, so);
                string asset = Encoding.ASCII.GetString(ov0, so, end - so);
                int i;
                if (!idByPath.TryGetValue(asset.ToLowerInvariant(), out i)) throw damaged;
                handleRows.Append(string.Format(Inv, "{0}\t0x{0:x4}\t{1}\t0x{1:x4}\t{2}\t{3}\t{4}\n", h, i, asset,
                                                Kind(asset), files[byId[i]].Length));
            }
            File.WriteAllText(Path.Combine(assetsDir, "files.tsv"), fileRows.ToString(), Utf8);
            File.WriteAllText(Path.Combine(assetsDir, "handles.tsv"), handleRows.ToString(), Utf8);
            log("    " + eu.Count + " files and " + HandleCount + " handles catalogued");
            progress(100, "Done");
        }

        static string[] Rest(string[] f, int from)
        {
            var o = new string[f.Length - from];
            Array.Copy(f, from, o, 0, o.Length);
            return o;
        }
    }
}
