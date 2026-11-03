// The language editions (sm64ds_builder.py's LANGUAGES, langpack and region_*,
// made to give the same files to the bit). The game is the European ROM's
// code, which has five languages (English, French, German, Italian and
// Spanish) and picks one by the DS owner's language; the 3DS has none, so the
// game is English unless the .cia says otherwise: --language picks the one it
// runs in. The Japanese, Korean and American English editions also take a
// second dump, of that cartridge (ASMJ, ASMK, ASME), for its text and the
// pictures with words in them, which go in the European files of the language
// slot the game then runs as. build/assets/langpack.bin tells the game
// (game/language.cpp reads it). Korean.cs is the Korean edition's part.
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Sm64dsEnhanced
{
    static class Languages
    {
        public static readonly string[] Codes = { "en", "fr", "de", "it", "es", "ja", "ko", "en-us" };
        static readonly string[] Names = { "English", "French", "German", "Italian", "Spanish", "Japanese", "Korean",
                                           "American English" };
        // the DS language the game runs as (1 English, 2 French, 3 German, 4 Italian, 5 Spanish)
        static readonly int[] Slots = { 1, 2, 3, 4, 5, 1, 5, 1 };
        // the second dump each takes
        static readonly string[] Dumps = { null, null, null, null, null, "ASMJ", "ASMK", "ASME" };
        // the HOME Menu title in the Japanese slot
        public const string TitleJapanese = "スーパーマリオ64DS Enhanced";

        static void Fail(string text) { throw new BuildError(text); }
        static uint U32(byte[] b, long o) { return BitConverter.ToUInt32(b, (int)o); }
        static ushort U16(byte[] b, long o) { return BitConverter.ToUInt16(b, (int)o); }

        static int Index(string code)
        {
            int i = Array.IndexOf(Codes, code);
            if (i < 0) Fail("There is no language '" + code + "'. The languages are: " + string.Join(", ", Codes) + ".");
            return i;
        }
        public static bool Known(string code) { return Array.IndexOf(Codes, code) >= 0; }
        public static string Name(string code) { return Names[Index(code)]; }
        public static string Dump(string code) { return Dumps[Index(code)]; }

        // a second dump's game code: its language (null: none)
        public static string OfDump(string gameCode)
        {
            int i = Array.IndexOf(Dumps, gameCode);
            return i < 0 ? null : Codes[i];
        }

        public static string Region(string gameCode)
        {
            return gameCode == "ASMJ" ? "Japanese" : gameCode == "ASMK" ? "Korean" : "American";
        }

        // The .cia's title (and file name) for a language.
        public static string Title(string code)
        {
            if (code == null || code == "en") return Program.Title;
            if (code == "ko") return Korean.Title;
            return Program.Title + " (" + Name(code) + ")";
        }

        // The language's own slot's titles in an SMDH bannertool made, where it has its own.
        public static void SmdhSlot(string smdh, string code)
        {
            if (code == "ko") SmdhTitle(smdh, 7, Korean.TitleHangul);
            else if (code == "ja") SmdhTitle(smdh, 0, TitleJapanese);
        }

        static void SmdhTitle(string smdh, int slot, string title)
        {
            var b = File.ReadAllBytes(smdh);
            int at = 8 + slot * 0x200;
            var t = Encoding.Unicode.GetBytes(title);
            Array.Clear(b, at, 0x80 + 0x100);
            Array.Copy(t, 0, b, at, Math.Min(t.Length, 0x80));
            Array.Copy(t, 0, b, at + 0x80, Math.Min(t.Length, 0x100));
            File.WriteAllBytes(smdh, b);
        }

        // build/assets/langpack.bin: "SM64DS-LANGPACK\0", u32 version (1), u32
        // size; the language's code (8 bytes); u8 the DS language it runs as,
        // u8 flags (1: the font widths follow); at 0x40 the widths (2 x 256)
        public static byte[] Langpack(string code, byte[] widths)
        {
            if (widths == null) widths = new byte[0];
            var b = new byte[0x40 + widths.Length];
            var magic = Encoding.ASCII.GetBytes("SM64DS-LANGPACK");
            Array.Copy(magic, b, magic.Length);
            Array.Copy(BitConverter.GetBytes(1u), 0, b, 0x10, 4);
            Array.Copy(BitConverter.GetBytes((uint)b.Length), 0, b, 0x14, 4);
            var c = Encoding.ASCII.GetBytes(code);
            Array.Copy(c, 0, b, 0x18, c.Length);
            b[0x20] = (byte)Slots[Index(code)];
            b[0x21] = (byte)(widths.Length > 0 ? 1 : 0);
            Array.Copy(widths, 0, b, 0x40, widths.Length);
            return b;
        }

        public static string Hex(byte[] h)
        {
            return BitConverter.ToString(h).Replace("-", "").ToLowerInvariant();
        }

        public static byte[] Sha(byte[] b, int offset, int count)
        {
            using (var s = SHA256.Create()) return s.ComputeHash(b, offset, count);
        }

        // the NitroFS files named in want (case-blind paths) of the dump "who" names
        public static Dictionary<string, byte[]> NitroFiles(byte[] rb, IEnumerable<string> want, string who)
        {
            long fntOffset = U32(rb, 0x40), fntSize = U32(rb, 0x44), fatOffset = U32(rb, 0x48), fatSize = U32(rb, 0x4C);
            if (fntOffset == 0 || fntOffset + fntSize > rb.Length || fatOffset == 0 || fatOffset + fatSize > rb.Length)
                Fail("The " + who + " dump is truncated: its file tables run past the end of the file. Re-dump the cartridge.");
            string damaged = "The " + who + " dump's file name table is damaged. Re-dump the cartridge.";
            var wanted = new Dictionary<string, string>();
            foreach (var p in want) wanted[p.ToLowerInvariant()] = p;
            var output = new Dictionary<string, byte[]>();
            var queue = new Queue<KeyValuePair<int, string>>();
            queue.Enqueue(new KeyValuePair<int, string>(0, ""));
            var visited = new HashSet<int> { 0 };
            while (queue.Count > 0)
            {
                var item = queue.Dequeue();
                long entry = fntOffset + item.Key * 8L;
                if (entry + 8 > rb.Length) Fail(damaged);
                long o = fntOffset + U32(rb, entry);
                int fileId = U16(rb, entry + 4);
                while (true)
                {
                    if (o >= rb.Length) Fail(damaged);
                    int type = rb[o++];
                    if (type == 0) break;
                    int len = type & 0x7F;
                    if (o + len + ((type & 0x80) != 0 ? 2 : 0) > rb.Length) Fail(damaged);
                    string name = Encoding.ASCII.GetString(rb, (int)o, len);
                    o += len;
                    string full = item.Value.Length > 0 ? item.Value + "/" + name : name;
                    if ((type & 0x80) != 0)
                    {
                        int sub = U16(rb, o) & 0x0FFF;
                        o += 2;
                        if (visited.Add(sub)) queue.Enqueue(new KeyValuePair<int, string>(sub, full));
                    }
                    else
                    {
                        string key;
                        if (wanted.TryGetValue(full.ToLowerInvariant(), out key) && fileId < fatSize / 8)
                        {
                            long start = U32(rb, fatOffset + fileId * 8L), end = U32(rb, fatOffset + fileId * 8L + 4);
                            if (start <= end && end <= rb.Length)
                            {
                                var b = new byte[end - start];
                                Array.Copy(rb, start, b, 0, b.Length);
                                output[key] = b;
                            }
                        }
                        fileId++;
                    }
                }
            }
            return output;
        }

        // Reads the second dump and checks it is the cartridge the language
        // takes; its arm9 (decompressed) through arm9.
        public static byte[] SecondDump(string path, string language, out byte[] arm9)
        {
            string code = Dump(language), who = Region(code);
            byte[] rb = null;
            arm9 = null;
            try { rb = File.ReadAllBytes(path); }
            catch (IOException e) { Fail("The " + who + " dump could not be read: " + e.Message); }
            if (rb.Length < 0x4000)
                Fail("The " + who + " dump is only " + rb.Length + " bytes, too small to be a DS cartridge dump.");
            string title = Encoding.ASCII.GetString(rb, 0, 12).TrimEnd('\0');
            string have = Encoding.ASCII.GetString(rb, 0x0C, 4);
            if (title != "S.MARIO64DS" || have != code)
                Fail("The second dump is not the " + who + " Super Mario 64 DS (game code " +
                     System.Text.RegularExpressions.Regex.Replace(have, "[^ -~]", ".") + ", not " + code + ").");
            try
            {
                var a9 = new byte[U32(rb, 0x2C)];
                Array.Copy(rb, U32(rb, 0x20), a9, 0, a9.Length);
                arm9 = Sm64dsEnhanced.Extract.ExpandBlz(a9);
            }
            catch (Exception e)
            {
                Fail("Could not read the " + who + " dump's program data (" + e.Message + "). Re-dump the cartridge.");
            }
            return rb;
        }

        public static BuildError NotThatCartridge(string language)
        {
            string code = Dump(language);
            return new BuildError("This is a " + Region(code) + " dump (game code " + code + "), but its contents do " +
                                  "not match the cartridge this build was made from. That usually means the file " +
                                  "has been modified (a patch or a trimmed dump), the dump is faulty, or it is " +
                                  "another revision of the cartridge. Use an untouched dump of your own retail " +
                                  "cartridge, then try again.");
        }

        static string OnDisk(string root, string p) { return Path.Combine(root, p.Replace('/', '\\')); }

        public static byte[] ReadFile(string work, string p)
        {
            return File.ReadAllBytes(OnDisk(Path.Combine(work, "extracted", "dsd", "files"), p));
        }

        // neu's files over the European ones in work (Extract.Run's folder), and
        // the file tables to match. A file that grew moves past the end of the
        // cartridge's data (the game's card serves whatever the allocation table
        // points at); one that didn't stays. How many entries changed.
        public static int ReplaceFiles(string work, Dictionary<string, byte[]> neu, string who)
        {
            string filesRoot = Path.Combine(work, "extracted", "dsd", "files");
            string assets = Path.Combine(work, "build", "assets");
            var idByPath = new Dictionary<string, int>();
            var utf8 = new UTF8Encoding(false);
            var rows = File.ReadAllText(Path.Combine(assets, "files.tsv"), utf8).Split('\n');
            for (int i = 1; i < rows.Length; i++)
            {
                if (rows[i].Length == 0) continue;
                var f = rows[i].Split('\t');
                idByPath[f[2].ToLowerInvariant()] = int.Parse(f[0]);
            }
            var fat = File.ReadAllBytes(Path.Combine(assets, "nitrofs_fat.bin"));
            uint top = 0;
            for (int i = 0; i < fat.Length / 8; i++) top = Math.Max(top, U32(fat, i * 8 + 4));
            uint at = (top + 0x1FF) & ~0x1FFu;
            var order = new List<string>(neu.Keys);
            order.Sort(delegate (string a, string b) { return idByPath[a.ToLowerInvariant()].CompareTo(idByPath[b.ToLowerInvariant()]); });
            var sizeById = new Dictionary<int, int>();
            foreach (var p in order)
            {
                int i = idByPath[p.ToLowerInvariant()];
                uint start = U32(fat, i * 8), end = U32(fat, i * 8 + 4);
                uint size = (uint)neu[p].Length;
                if (size > end - start)
                {
                    start = at;
                    at = (at + size + 0x1FF) & ~0x1FFu;
                }
                Array.Copy(BitConverter.GetBytes(start), 0, fat, i * 8, 4);
                Array.Copy(BitConverter.GetBytes(start + size), 0, fat, i * 8 + 4, 4);
                sizeById[i] = (int)size;
                File.WriteAllBytes(OnDisk(filesRoot, p), neu[p]);
            }
            if (at > 0x8000000)   // the game's card reads any 32-bit address; mods' range starts at 0x10000000
                Fail("The " + who + " files don't fit the cartridge's address space.");
            File.WriteAllBytes(Path.Combine(assets, "nitrofs_fat.bin"), fat);
            Resize(Path.Combine(assets, "files.tsv"), 0, sizeById);
            Resize(Path.Combine(assets, "handles.tsv"), 2, sizeById);
            return sizeById.Count;
        }

        static void Resize(string path, int idCol, Dictionary<int, int> sizeById)
        {
            var utf8 = new UTF8Encoding(false);
            var lines = File.ReadAllText(path, utf8).Split('\n');
            var o = new StringBuilder(lines[0] + "\n");
            for (int i = 1; i < lines.Length; i++)
            {
                if (lines[i].Length == 0) continue;
                var f = lines[i].Split('\t');
                int id = int.Parse(f[idCol]);
                int size;
                if (sizeById.TryGetValue(id, out size)) f[f.Length - 1] = size.ToString();
                o.Append(string.Join("\t", f)).Append('\n');
            }
            File.WriteAllText(path, o.ToString(), utf8);
        }

        // The game's LZ77 file around raw bytes, as literal runs ("LZ77" in
        // front, or a bare type-0x10 stream).
        public static byte[] Lz77Wrap(byte[] raw) { return Wrap(raw, true); }
        public static byte[] Lz10Wrap(byte[] raw) { return Wrap(raw, false); }

        static byte[] Wrap(byte[] raw, bool magic)
        {
            var o = new MemoryStream();
            if (magic) o.Write(new byte[] { (byte)'L', (byte)'Z', (byte)'7', (byte)'7' }, 0, 4);
            o.Write(new byte[] { 0x10, (byte)raw.Length, (byte)(raw.Length >> 8), (byte)(raw.Length >> 16) }, 0, 4);
            for (int i = 0; i < raw.Length; i += 8)
            {
                o.WriteByte(0);
                o.Write(raw, i, Math.Min(8, raw.Length - i));
            }
            return o.ToArray();
        }

        // The BMD texture (table 0x18) or palette (0x20) called name: its data
        // offset and size.
        static int BmdFind(byte[] bmd, int table, int count, int stride, string name, out int size)
        {
            var want = Encoding.ASCII.GetBytes(name);
            for (int i = 0; i < U32(bmd, count); i++)
            {
                int e = (int)U32(bmd, table) + i * stride;
                int n = (int)U32(bmd, e);
                int end = Array.IndexOf(bmd, (byte)0, n);
                if (end - n != want.Length) continue;
                bool same = true;
                for (int j = 0; j < want.Length; j++) if (bmd[n + j] != want[j]) { same = false; break; }
                if (!same) continue;
                size = (int)U32(bmd, e + 8);
                return (int)U32(bmd, e + 4);
            }
            Fail("The title screen's textures aren't the ones this build knows. Re-dump the cartridge.");
            size = 0;
            return 0;
        }
        static int Tex(byte[] b, string n, out int size) { return BmdFind(b, 0x18, 0x14, 20, n, out size); }
        static int Pal(byte[] b, string n, out int size) { return BmdFind(b, 0x20, 0x1C, 16, n, out size); }

        public static int[][] Colours(byte[] b, int at, int n)
        {
            var c = new int[n][];
            for (int i = 0; i < n; i++)
            {
                int v = U16(b, at + i * 2);
                c[i] = new[] { v & 31, v >> 5 & 31, v >> 10 & 31 };
            }
            return c;
        }

        // the first of choices (from..to-1) nearest c
        public static int Nearest(int[] c, int from, int to, int[][] pal)
        {
            int best = from, bestD = int.MaxValue;
            for (int j = from; j < to; j++)
            {
                int d = 0;
                for (int k = 0; k < 3; k++) d += (c[k] - pal[j][k]) * (c[k] - pal[j][k]);
                if (d < bestD) { bestD = d; best = j; }
            }
            return best;
        }

        // ---- NARC archives -----------------------------------------------------

        // A NARC's members by path (their index), and its sections' offsets.
        internal static Dictionary<string, int> NarcMembers(byte[] narc, out Dictionary<string, int> sections)
        {
            const string bad = "One of the game's archives isn't one this build can read. Re-dump the cartridge.";
            sections = new Dictionary<string, int>();
            if (narc.Length < 16 || Encoding.ASCII.GetString(narc, 0, 4) != "NARC") Fail(bad);
            int at = U16(narc, 0x0C);
            for (int i = 0; i < U16(narc, 0x0E); i++)
            {
                if (at + 8 > narc.Length) Fail(bad);
                sections[Encoding.ASCII.GetString(narc, at, 4)] = at;
                at += (int)U32(narc, at + 4);
            }
            if (!sections.ContainsKey("BTAF") || !sections.ContainsKey("BTNF") || !sections.ContainsKey("GMIF")) Fail(bad);
            int fnt = sections["BTNF"] + 8;
            var names = new Dictionary<string, int>();
            var queue = new Queue<KeyValuePair<int, string>>();
            queue.Enqueue(new KeyValuePair<int, string>(0, ""));
            while (queue.Count > 0)
            {
                var item = queue.Dequeue();
                int o = fnt + (int)U32(narc, fnt + item.Key * 8), fid = U16(narc, fnt + item.Key * 8 + 4);
                while (true)
                {
                    int type = narc[o++];
                    if (type == 0) break;
                    string full = (item.Value.Length > 0 ? item.Value + "/" : "") +
                                  Encoding.ASCII.GetString(narc, o, type & 0x7F);
                    o += type & 0x7F;
                    if ((type & 0x80) != 0)
                    {
                        queue.Enqueue(new KeyValuePair<int, string>(U16(narc, o) & 0x0FFF, full));
                        o += 2;
                    }
                    else names[full] = fid++;
                }
            }
            return names;
        }

        internal static List<byte[]> NarcFiles(byte[] narc, Dictionary<string, int> s)
        {
            int fat = s["BTAF"], img = s["GMIF"] + 8;
            var data = new List<byte[]>();
            for (int i = 0; i < U16(narc, fat + 8); i++)
            {
                int start = (int)U32(narc, fat + 12 + i * 8), end = (int)U32(narc, fat + 16 + i * 8);
                var b = new byte[end - start];
                Array.Copy(narc, img + start, b, 0, b.Length);
                data.Add(b);
            }
            return data;
        }

        static byte[] NarcGet(byte[] narc, string name)
        {
            Dictionary<string, int> s;
            var names = NarcMembers(narc, out s);
            if (!names.ContainsKey(name)) Fail("An archive has no " + name + ". Re-dump the cartridge.");
            return NarcFiles(narc, s)[names[name]];
        }

        // The NARC with the members in neu replaced: the names kept, the data
        // laid out again (each member 4-byte aligned).
        static byte[] NarcReplace(byte[] narc, Dictionary<string, byte[]> neu)
        {
            Dictionary<string, int> s;
            var names = NarcMembers(narc, out s);
            var data = NarcFiles(narc, s);
            foreach (var kv in neu) data[names[kv.Key]] = kv.Value;
            var btaf = new MemoryStream();
            var gmif = new MemoryStream();
            btaf.Write(Encoding.ASCII.GetBytes("BTAF"), 0, 4);
            btaf.Write(BitConverter.GetBytes((uint)(12 + 8 * data.Count)), 0, 4);
            btaf.Write(BitConverter.GetBytes((ushort)data.Count), 0, 2);
            btaf.Write(new byte[2], 0, 2);
            foreach (var b in data)
            {
                btaf.Write(BitConverter.GetBytes((uint)gmif.Length), 0, 4);
                btaf.Write(BitConverter.GetBytes((uint)(gmif.Length + b.Length)), 0, 4);
                gmif.Write(b, 0, b.Length);
                for (int i = 0; i < (-b.Length & 3); i++) gmif.WriteByte(0xFF);
            }
            var o = new MemoryStream();
            o.Write(narc, 0, 8);
            o.Write(new byte[4], 0, 4);
            o.Write(narc, 12, 4);
            btaf.WriteTo(o);
            o.Write(narc, s["BTNF"], (int)U32(narc, s["BTNF"] + 4));
            o.Write(Encoding.ASCII.GetBytes("GMIF"), 0, 4);
            o.Write(BitConverter.GetBytes((uint)(8 + gmif.Length)), 0, 4);
            gmif.WriteTo(o);
            var r = o.ToArray();
            Array.Copy(BitConverter.GetBytes((uint)r.Length), 0, r, 8, 4);
            return r;
        }

        // ---- the Japanese and American editions ---------------------------------
        // The American cartridge (ASME) and the Japanese one (ASMJ) are one game:
        // both have the English and the Japanese text, and each its own pictures.
        // Their code is the European English code, so either edition runs in the
        // English slot: its message bank in every slot's, its pictures in the
        // English files, its fonts and their widths for the European ones. The
        // Japanese font numbers some characters the European code writes itself
        // elsewhere (the space, the stars, the coin, the times sign, the dash and
        // the digits): the Japanese edition moves them to the European numbers
        // (JapaneseMoves: Japanese number, European number), and the characters
        // there to where they were, in both fonts, their widths and the text.

        static readonly int[,] JapaneseMoves = {
            { 0xF1, 0x4D }, { 0xCC, 0xF0 }, { 0xCD, 0xF1 }, { 0xCE, 0xEE }, { 0xCF, 0xEF }, { 0xCA, 0x47 },
            { 0x48, 0x00 }, { 0x49, 0x01 }, { 0x4A, 0x02 }, { 0x4B, 0x03 }, { 0x4C, 0x04 }, { 0x4D, 0x05 },
            { 0x4E, 0x06 }, { 0x4F, 0x07 }, { 0x58, 0x08 }, { 0x59, 0x09 },
        };
        static readonly string[] MgPictures = {
            "MG/d_2d_mg_bg_option_nsc.bin", "MG/d_2d_mg_card_ncg.bin", "MG/d_2d_mg_common_ncg.bin",
            "MG/d_2d_mg_jump_ncg.bin", "MG/d_2d_mg_menu_touch_ncg.bin", "MG/d_2d_mg_option_ncg.bin",
            "MG/d_2d_mg_pazzle_panel_ncg.bin", "MG/d_2d_mg_smart_ball_lay00_ncg.bin",
            "MG/d_2d_mg_smart_ball_lay01_ncg.bin", "MG/d_2d_mg_trampoline_ncg.bin",
        };
        static readonly string[] BankSlots = {
            "data/message/msg_data_eng.bin", "data/message/msg_data_frn.bin", "data/message/msg_data_gmn.bin",
            "data/message/msg_data_itl.bin", "data/message/msg_data_spn.bin",
        };
        const string EuFont = "data/2D_cad/EUR/d_2d_mario_3Dfont_PAL_ncg.bin";
        const string EuBigFont = "data/2D_cad/EUR/d_2d_mario_3Dfont_16_16_PAL_ncg.bin";
        const string EuTexBank = "data/DSMT/EUR/texBank_E.bmd";
        const string Eng = "data/2D_cad/EUR/ENG/";
        // RegionSha of the retail cartridges (Rev 1)
        const string JapaneseSha = "612355421d33bb4b455288e26f951b6a8f7f200d78cf274e87cf113fc37107f2";
        const string AmericanSha = "dcdcf53eb1b2a5fbd749068375c9fcb4a93c8b5176c4424ae3bd4bb3a791e709";

        class Spec
        {
            public List<KeyValuePair<string, string>> Files = new List<KeyValuePair<string, string>>();
            public List<KeyValuePair<string, string>> Members = new List<KeyValuePair<string, string>>();
            public string Font, BigFont, Bank, TexBank, Sha;
            public uint[] Widths;
            public bool Moves;
        }

        static Spec RegionSpec(string language)
        {
            bool j = language == "ja";
            string d = j ? "JPN/" : "USA/", u = j ? "" : "_U", us = j ? "" : "_US";
            var s = new Spec();
            Action<string, string> file = delegate (string a, string b) { s.Files.Add(new KeyValuePair<string, string>(a, b)); };
            Action<string, string> member = delegate (string a, string b) { s.Members.Add(new KeyValuePair<string, string>(a, b)); };
            file("data/2D_cad/" + d + "d_2d_battle_framework_course_select" + u + "_nsc.bin",
                 "data/2D_cad/EUR/d_2d_battle_framework_course_select_P_nsc.bin");
            file("data/2D_cad/" + d + "d_2d_battle_framework_gamestart" + u + "_nsc.bin",
                 "data/2D_cad/EUR/d_2d_battle_framework_gamestart_P_nsc.bin");
            file("data/2D_cad/" + d + "d_2d_cmn_icon" + u + "_ncg.bin", Eng + "d_2d_cmn_icon_E_ncg.bin");
            file("data/DSMT/" + d + "face_demo_big_yoshistar" + u + ".bca", "data/DSMT/EUR/face_demo_big_yoshistar_U.bca");
            file("data/normal_obj/opening/" + d + "peach_letter" + us + ".bmd",
                 "data/normal_obj/opening/EUR/peach_letter_US.bmd");
            if (j) foreach (var p in MgPictures) file(p, p);
            member("data/2D_cad/" + d + "d_2d_battle_btm_course_player_select" + u + "_ncg.bin",
                   Eng + "d_2d_battle_btm_course_player_select_EU_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_battle_cmn_icon" + u + "_ncg.bin", Eng + "d_2d_battle_cmn_icon_E_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_battle_upr_course_player_select" + u + "_ncg.bin",
                   Eng + "d_2d_battle_upr_course_player_select_EU_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_battle_upr_icon" + u + "_ncg.bin", Eng + "d_2d_battle_upr_icon_E_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_game_over_BG" + u + "_ncg.bin", Eng + "d_2d_game_over_BG_E_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_touch_select_4bit" + u + "_ncg.bin", Eng + "d_2d_touch_select_4bit_E_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_upr_icon" + u + "_ncg.bin", Eng + "d_2d_upr_icon_E_ncg.bin");
            member("data/2D_cad/" + d + "d_2d_upr_play_mode_select" + u + "_ncg.bin",
                   Eng + "d_2d_upr_play_mode_select_E_ncg.bin");
            string p_ = j ? "" : "_US_PAL";
            s.Font = "data/2D_cad/" + d + "d_2d_mario_3Dfont" + p_ + "_ncg.bin";
            s.BigFont = "data/2D_cad/" + d + "d_2d_mario_3Dfont_16_16" + p_ + "_ncg.bin";
            s.Bank = j ? "data/message/msg_data_jpn.bin" : "data/message/msg_data_nes.bin";
            s.TexBank = "data/DSMT/" + d + "texBank" + u + ".bmd";
            // its arm9's font widths (data_0208f074 and data_0208f174 in the European one)
            s.Widths = j ? new uint[] { 0x0208DA2C, 0x0208DB2C } : new uint[] { 0x0208D90C, 0x0208DA0C };
            s.Moves = j;
            s.Sha = j ? JapaneseSha : AmericanSha;
            return s;
        }

        static int[] CodeMap(bool moves)
        {
            var m = new int[256];
            for (int c = 0; c < 256; c++) m[c] = c;
            if (!moves) return m;
            var from = new SortedSet<int>();
            var to = new SortedSet<int>();
            for (int i = 0; i < JapaneseMoves.GetLength(0); i++)
            {
                m[JapaneseMoves[i, 0]] = JapaneseMoves[i, 1];
                from.Add(JapaneseMoves[i, 0]);
                to.Add(JapaneseMoves[i, 1]);
            }
            var taken = new List<int>(to);
            taken.RemoveAll(delegate (int c) { return from.Contains(c); });
            var freed = new List<int>(from);
            freed.RemoveAll(delegate (int c) { return to.Contains(c); });
            for (int i = 0; i < Math.Min(taken.Count, freed.Count); i++) m[taken[i]] = freed[i];
            return m;
        }

        // the font (4bpp tiles) with each character c's tiles at character m[c]'s
        static byte[] MoveGlyphs(byte[] font, int[] m, bool big)
        {
            var o = (byte[])font.Clone();
            for (int c = 0; c < 256; c++)
            {
                var a = Tiles(c, big);
                var b = Tiles(m[c], big);
                for (int k = 0; k < a.Length; k++) Array.Copy(font, a[k] * 32, o, b[k] * 32, 32);
            }
            return o;
        }

        static int[] Tiles(int c, bool big)
        {
            if (big)   // 16x16: func_0201b100
            {
                int t = (c & 0xF) * 2 + (c & 0xF0) * 4;
                return new[] { t, t + 1, t + 32, t + 33 };
            }
            int s = (c & 0x1F) + ((c & 0xE0) << 1);   // 8x16: Message::AddChar
            return new[] { s, s + 32 };
        }

        // A message bank (LZ77) with each character c of its text written m[c];
        // 0xFD (a new line), 0xFE (a command, its length next) and 0xFF (the end)
        // stay.
        static byte[] RecodeBank(byte[] bank, int[] m)
        {
            var b = (byte[])Face.Lz77(bank).Clone();
            if (b.Length < 0x30 || Encoding.ASCII.GetString(b, 0, 8) != "GSEM1gmb" ||
                Encoding.ASCII.GetString(b, 0x20, 4) != "1FNI")
                Fail("A message bank isn't one this build can read. Re-dump the cartridge.");
            const int info = 0x20;
            int count = U16(b, info + 8);
            int data = info + (int)U32(b, info + 4) + 8;
            var done = new bool[b.Length];
            for (int i = 0; i < count; i++)
            {
                int p = data + (int)U32(b, info + 0x10 + i * 8);
                while (p < b.Length && !done[p])
                {
                    int c = b[p];
                    if (c == 0xFF) break;
                    if (c == 0xFE)
                    {
                        int n = b[p + 1];
                        if (n < 2) Fail("A message bank isn't one this build can read. Re-dump the cartridge.");
                        for (int k = 0; k < n && p + k < b.Length; k++) done[p + k] = true;
                        p += n;
                        continue;
                    }
                    done[p] = true;
                    if (c != 0xFD) b[p] = (byte)m[c];
                    p++;
                }
            }
            return Lz77Wrap(b);
        }

        static string RegionSha(Dictionary<string, byte[]> files, byte[] arm9, Spec spec)
        {
            var paths = new List<string>(files.Keys);
            paths.Sort(string.CompareOrdinal);
            using (var s = SHA256.Create())
            {
                foreach (var p in paths)
                {
                    var name = Encoding.ASCII.GetBytes(p + "\0");
                    var h = Sha(files[p], 0, files[p].Length);
                    s.TransformBlock(name, 0, name.Length, null, 0);
                    s.TransformBlock(h, 0, h.Length, null, 0);
                }
                foreach (var a in spec.Widths)
                {
                    int o = (int)(a - 0x02004000);
                    s.TransformBlock(arm9, o, 0x100, null, 0);
                }
                s.TransformFinalBlock(new byte[0], 0, 0);
                return Hex(s.Hash);
            }
        }

        public class RegionData
        {
            public Dictionary<string, byte[]> Files;
            public byte[] Widths;   // the font's and the big font's
        }

        // What the Japanese or American edition takes out of its dump.
        public static RegionData ExtractRegion(string path, string language, Action<string> log)
        {
            byte[] arm9;
            var rb = SecondDump(path, language, out arm9);
            string code = Dump(language), who = Region(code);
            log("Super Mario 64 DS (" + code + ", " + (language == "ja" ? "Japan" : "USA") + "), " +
                Math.Round(rb.Length / 1048576.0) + " MB");
            var spec = RegionSpec(language);
            var want = new List<string>();
            foreach (var kv in spec.Files) want.Add(kv.Key);
            want.Add("ARCHIVE/c2d.narc");
            want.Add(spec.Bank);
            want.Add(spec.TexBank);
            var files = NitroFiles(rb, want, who);
            foreach (var p in want)
                if (!files.ContainsKey(p)) Fail("The " + who + " dump has no " + p + ". Re-dump the cartridge.");
            foreach (var a in spec.Widths)
                if (a - 0x02004000 + 0x100 > arm9.Length) throw NotThatCartridge(language);
            if (RegionSha(files, arm9, spec) != spec.Sha) throw NotThatCartridge(language);
            var widths = new byte[0x200];
            for (int i = 0; i < 2; i++) Array.Copy(arm9, (int)(spec.Widths[i] - 0x02004000), widths, i * 0x100, 0x100);
            log("    the " + Name(language) + " text: " + files.Count + " files and the fonts, checksums OK");
            return new RegionData { Files = files, Widths = widths };
        }

        // texBank_E (the title, file and options menus) with the cartridge's
        // pictures; the European title menu has its pieces elsewhere (the
        // American and Japanese have the mini-game button where the European has
        // the wrench, and the wrench and arrows below), so they go where the
        // European has them.
        static byte[] RegionTexBank(byte[] euE, byte[] src, string language)
        {
            byte[] e = (byte[])Face.Lz77(euE).Clone(), k = Face.Lz77(src);
            string u = language == "ja" ? "" : "_U";
            string[,] pics = {
                { "file_menu" + u, "file_menu" + u + "_pl", "file_menu_U", "file_menu_U_pl" },
                { "title_menu" + u, "title_menu" + u + "_pl", "title_menu_E", "title_menu_E_pl" },
                { "wallpaper_logo" + u, "wallpaper_logo" + u, "wallpaper_logo_U", "wallpaper_logo_U" },
            };
            const string notKnown = "The title screen's textures aren't the ones this build knows. Re-dump the cartridge.";
            for (int i = 0; i < pics.GetLength(0); i++)
            {
                int ks, kps, es, eps;
                int ka = Tex(k, pics[i, 0], out ks), kpa = Pal(k, pics[i, 1], out kps);
                int ea = Tex(e, pics[i, 2], out es), epa = Pal(e, pics[i, 3], out eps);
                if (ks != es || kps != eps) Fail(notKnown);
                var pic = new byte[ks];
                Array.Copy(k, ka, pic, 0, ks);
                if (pics[i, 2] == "title_menu_E")
                {
                    // (x, y, w, h) in the cartridge's picture: (x, y) in the European one
                    int[,] pieces = {
                        { 0, 0, 64, 40, 0, 0 },        // VS
                        { 64, 0, 64, 40, 0, 80 },      // the mini-games: the Rec Room
                        { 0, 40, 128, 40, 0, 40 },     // ADVENTURE
                        { 0, 80, 32, 32, 96, 0 },      // the wrench
                        { 32, 80, 48, 16, 80, 112 },   // the arrows
                    };
                    var menu = new byte[ks];
                    for (int q = 0; q < pieces.GetLength(0); q++)
                        for (int y = 0; y < pieces[q, 3]; y++)
                            Array.Copy(pic, (pieces[q, 1] + y) * 128 + pieces[q, 0], menu,
                                       (pieces[q, 5] + y) * 128 + pieces[q, 4], pieces[q, 2]);
                    pic = menu;
                }
                Array.Copy(pic, 0, e, ea, es);
                Array.Copy(k, kpa, e, epa, eps);
            }
            // the options screen: the European picture has 200 colours, so the
            // cartridge's colours past 200 become their nearest below
            {
                int ks, kps, es, eps;
                int ka = Tex(k, "title_option" + u, out ks), kpa = Pal(k, "title_option" + u, out kps);
                int ea = Tex(e, "title_option_EU", out es), epa = Pal(e, "title_option_pl", out eps);
                if (ks != es || kps != 0x200 || eps != 0x190) Fail(notKnown);
                var opal = Colours(k, kpa, 256);
                var below = new int[256];
                for (int i = 0; i < 256; i++) below[i] = i < 200 ? i : Nearest(opal[i], 1, 200, opal);
                for (int i = 0; i < ks; i++) e[ea + i] = (byte)below[k[ka + i]];
                Array.Copy(k, kpa, e, epa, eps);
            }
            return Lz77Wrap(e);
        }

        // The Japanese or American files over the European English ones in work.
        public static void ApplyRegion(string work, RegionData got, string language, Action<string> log)
        {
            var spec = RegionSpec(language);
            var src = got.Files;
            var m = CodeMap(spec.Moves);
            var neu = new Dictionary<string, byte[]>();
            foreach (var kv in spec.Files) neu[kv.Value] = src[kv.Key];
            var bank = spec.Moves ? RecodeBank(src[spec.Bank], m) : src[spec.Bank];
            foreach (var p in BankSlots) neu[p] = bank;
            var c2d = src["ARCHIVE/c2d.narc"];
            var members = new Dictionary<string, byte[]>();
            foreach (var kv in spec.Members) members[kv.Value] = NarcGet(c2d, kv.Key);
            neu["ARCHIVE/cee.narc"] = NarcReplace(ReadFile(work, "ARCHIVE/cee.narc"), members);
            byte[] font = NarcGet(c2d, spec.Font), big = Face.Lz77(NarcGet(c2d, spec.BigFont));
            if (spec.Moves)
            {
                font = MoveGlyphs(font, m, false);
                big = MoveGlyphs(big, m, true);
            }
            var fonts = new Dictionary<string, byte[]>();
            fonts[EuFont] = font;
            fonts[EuBigFont] = Lz10Wrap(big);
            neu["ARCHIVE/c2d.narc"] = NarcReplace(ReadFile(work, "ARCHIVE/c2d.narc"), fonts);
            neu[EuTexBank] = RegionTexBank(ReadFile(work, EuTexBank), src[spec.TexBank], language);
            var widths = new byte[0x200];
            for (int c = 0; c < 256; c++)
            {
                widths[m[c]] = got.Widths[c];
                widths[0x100 + m[c]] = got.Widths[0x100 + c];
            }
            int changed = ReplaceFiles(work, neu, Name(language));
            File.WriteAllBytes(Path.Combine(work, "build", "assets", "langpack.bin"), Langpack(language, widths));
            log("    " + neu.Count + " files " + Name(language) + ", " + changed + " entries in the file table updated");
        }
    }
}
