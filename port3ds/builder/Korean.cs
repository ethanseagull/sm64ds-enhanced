// The Korean edition (sm64ds_builder.py's korean_*, made to give the same files
// to the bit; Languages.cs has the other editions). The game is the European
// ROM's code, so the Korean edition is built from both dumps: the European one
// as for the European edition, and from the Korean cartridge (ASMK) its message
// bank, the graphics it changed (the Korean art is in the Spanish slot, which
// the game then uses), its title menu's pictures, and korean.bin, its arm9's
// font and text tables (the game's game/korean.cpp reads it).
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Sm64dsEnhanced
{
    static class Korean
    {
        public const string Title = "Super Mario 64 DS Enhanced (Korean)";
        public const string TitleHangul = "\uC288\uD37C \uB9C8\uB9AC\uC624 64 DS Enhanced";   // in the Korean slot

        static readonly string[] Files = {
            "ARCHIVE/c2d.narc", "ARCHIVE/ces.narc", "ARCHIVE/en1.narc",
            "data/2D_cad/EUR/SPA/d_2d_cmn_icon_S_ncg.bin", "data/2D_cad/EUR/SPA/d_2d_staff_roll_cake_8bit_S_ncg.bin",
            "data/2D_cad/EUR/d_2d_scenario_player_select_S_ncg.bin",
            "data/2D_cad/EUR/d_2d_scenario_player_select_course_number_BG_S_ncg.bin",
            "data/2D_cad/EUR/d_2d_staff_roll_end_EUR_ncg.bin", "data/2D_cad/d_2d_game_over_ncg.bin",
            "data/2D_cad/d_2d_mario_3Dfont_ncl.bin", "data/2D_cad/d_2d_scenario_player_select_course_number_BG_nsc.bin",
            "data/2D_cad/d_2d_upr_touch_to_select_nsc.bin",
            "data/DSMT/EUR/file_menu_S_ncg.bin", "data/DSMT/EUR/title_demo_S_ncg.bin",
            "data/DSMT/EUR/texBank_E.bmd",
            "data/message/msg_data_hn.bin",
        };
        // every language's bank becomes the Korean one
        const string Bank = "data/message/msg_data_hn.bin";
        static readonly string[] BankSlots = {
            "data/message/msg_data_eng.bin", "data/message/msg_data_frn.bin", "data/message/msg_data_gmn.bin",
            "data/message/msg_data_itl.bin", "data/message/msg_data_spn.bin",
        };
        const string TexBank = "data/DSMT/EUR/texBank_E.bmd";   // the Korean cartridge's (all its menu art)
        const string TexBankS = "data/DSMT/EUR/texBank_S.bmd";
        // korean.bin's parts out of the Korean arm9: address, size, padded size, sha256
        static readonly uint[] PartAddress = { 0x020891E8, 0x020900FC, 0x020901FC, 0x0208FEEC, 0x0208FE64, 0x0208FE78, 0x0208FF54 };
        static readonly int[] PartSize = { 0x6C40, 0x100, 0x100, 0x68, 7, 0x14, 0x1A8 };
        static readonly int[] PartPadded = { 0x6C40, 0x100, 0x100, 0x68, 8, 0x14, 0x1A8 };
        static readonly string[] PartSha = {
            "d00ef99b3d932d96a9028a918ec1743228f9d8c58ab723989477a2615b72308d",   // the font
            "432081527a8d6515bcb8c241a291adc3a0b5361450d7041b027921d8e55eb291",   // its widths
            "6dc062862651f497461da6abd52887a1baa2cf20f2fb2400425879ae47b211a3",   // the big font's
            "41260aa68d9300ee3b58696a3df4b85b93912d1e48be209cf05bca475a063569",   // the codes it has
            "d86891d09c437337e5582db342a88d38e598ad47ee4ff43a0527868fd0175130",   // the button icons
            "63027dc0325e47496c74e01cfea8e4bed36aabe0acd59074dd5d3e8722fc09aa",   // the digits
            "92cf5aafe424f7ec58490a1d92246f61c4832cc1d769a4a48fa2731372ce95c5",   // the message ids
        };
        // FilesSha of the retail cartridge
        const string RetailFilesSha = "040ed1bab9155b35457bc54335e1816943623f41635b9101b630811e1eac1599";

        public class Data
        {
            public Dictionary<string, byte[]> Files;
            public byte[] Bin;   // korean.bin
        }

        static void Fail(string text) { throw new BuildError(text); }
        static uint U32(byte[] b, long o) { return BitConverter.ToUInt32(b, (int)o); }
        static ushort U16(byte[] b, long o) { return BitConverter.ToUInt16(b, (int)o); }

        static string Hex(byte[] h) { return Languages.Hex(h); }
        static byte[] Sha(byte[] b, int offset, int count) { return Languages.Sha(b, offset, count); }

        // A dump's game code, or "" if it can't be read.
        public static string Code(string path)
        {
            try
            {
                using (var f = File.OpenRead(path))
                {
                    var h = new byte[0x10];
                    if (f.Read(h, 0, 0x10) < 0x10) return "";
                    return Encoding.ASCII.GetString(h, 0x0C, 4);
                }
            }
            catch (Exception) { return ""; }
        }

        // One hash over the Korean files the build takes, in Files order.
        static string FilesSha(Dictionary<string, byte[]> files)
        {
            using (var s = SHA256.Create())
            {
                foreach (var p in Files)
                {
                    var name = Encoding.ASCII.GetBytes(p + "\0");
                    var h = Sha(files[p], 0, files[p].Length);
                    s.TransformBlock(name, 0, name.Length, null, 0);
                    s.TransformBlock(h, 0, h.Length, null, 0);
                }
                s.TransformFinalBlock(new byte[0], 0, 0);
                return Hex(s.Hash);
            }
        }

        // The Korean dump's files and korean.bin, checked against the retail cartridge.
        public static Data Extract(string path, Action<string> log)
        {
            byte[] arm9;
            var kb = Languages.SecondDump(path, "ko", out arm9);
            log("Super Mario 64 DS (ASMK, Korea), " + Math.Round(kb.Length / 1048576.0) + " MB");
            var files = Languages.NitroFiles(kb, Files, "Korean");
            foreach (var p in Files)
                if (!files.ContainsKey(p)) Fail("The Korean dump has no " + p + ". Re-dump the cartridge.");
            if (FilesSha(files) != RetailFilesSha) throw Languages.NotThatCartridge("ko");
            var body = new MemoryStream();
            for (int i = 0; i < PartAddress.Length; i++)
            {
                int o = (int)(PartAddress[i] - 0x02004000);
                if (o + PartSize[i] > arm9.Length || Hex(Sha(arm9, o, PartSize[i])) != PartSha[i])
                    throw Languages.NotThatCartridge("ko");
                body.Write(arm9, o, PartSize[i]);
                body.Write(new byte[PartPadded[i] - PartSize[i]], 0, PartPadded[i] - PartSize[i]);
            }
            var bin = new MemoryStream();
            var magic = Encoding.ASCII.GetBytes("SM64DS-KOREAN");
            bin.Write(magic, 0, magic.Length);
            bin.Write(new byte[3], 0, 3);
            bin.Write(BitConverter.GetBytes(1u), 0, 4);
            bin.Write(BitConverter.GetBytes((uint)(0x20 + body.Length)), 0, 4);
            bin.Write(new byte[8], 0, 8);
            body.WriteTo(bin);
            log("    the Korean text: " + files.Count + " files and the font, checksums OK");
            return new Data { Files = files, Bin = bin.ToArray() };
        }

        static byte[] Lz77Wrap(byte[] raw) { return Languages.Lz77Wrap(raw); }

        // The data offset of the BMD texture (table 0x18) or palette (0x20)
        // called name, which must be size bytes.
        static int BmdFind(byte[] bmd, int table, int count, int stride, string name, int size)
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
                if (U32(bmd, e + 8) != size) break;
                return (int)U32(bmd, e + 4);
            }
            Fail("The title screen's textures aren't the ones this build knows. Re-dump the cartridge.");
            return 0;
        }
        static int Tex(byte[] b, string n, int size) { return BmdFind(b, 0x18, 0x14, 20, n, size); }
        static int Pal(byte[] b, string n, int size) { return BmdFind(b, 0x20, 0x1C, 16, n, size); }

        static int[][] Colours(byte[] b, int at, int n) { return Languages.Colours(b, at, n); }
        static int Nearest(int[] c, int from, int to, int[][] pal) { return Languages.Nearest(c, from, to, pal); }

        // The title menu's two texture banks for the Korean edition: the European
        // banks, which the game's menu is made for, with the Korean cartridge's
        // pictures in them (sm64ds_builder.py's korean_texbanks says how).
        static void Texbanks(byte[] euE, byte[] euS, byte[] ko, out byte[] outE, out byte[] outS)
        {
            byte[] e = Face.Lz77(euE), s = Face.Lz77(euS), k = Face.Lz77(ko);
            e = (byte[])e.Clone();
            s = (byte[])s.Clone();

            int fm = Tex(k, "file_menu_K", 0x10000), fmp = Pal(k, "file_menu_F_pl", 0x200);
            foreach (var t in new[] { new object[] { e, "file_menu_U", "file_menu_U_pl" },
                                      new object[] { s, "file_menu_S", "file_menu_S_pl" } })
            {
                var b = (byte[])t[0];
                Array.Copy(k, fm, b, Tex(b, (string)t[1], 0x10000), 0x10000);
                Array.Copy(k, fmp, b, Pal(b, (string)t[2], 0x200), 0x200);
            }

            int et = Tex(e, "title_menu_E", 0x4000), ep = Pal(e, "title_menu_E_pl", 0x200);
            int kt = Tex(k, "title_menu_K", 0x4000), kp = Pal(k, "title_menu_K_pl", 0x200);
            var epal = Colours(e, ep, 256);
            var kpal = Colours(k, kp, 256);
            var near = new int[256];
            for (int i = 1; i < 256; i++) near[i] = Nearest(epal[i], 1, 256, kpal);
            var menu = new byte[0x4000];
            for (int i = 0; i < 0x4000; i++) menu[i] = (byte)near[e[et + i]];
            for (int y = 80; y < 120; y++)          // the Rec Room
                for (int x = 0; x < 64; x++) menu[y * 128 + x] = 0;
            for (int y = 40; y < 80; y++)           // ADVENTURE
                for (int x = 0; x < 120; x++) menu[y * 128 + 4 + x] = k[kt + y * 128 + x * 128 / 120];
            foreach (var t in new[] { new object[] { e, "title_menu_E", "title_menu_E_pl" },
                                      new object[] { s, "title_menu_S", "title_menu_S_pl" } })
            {
                var b = (byte[])t[0];
                Array.Copy(menu, 0, b, Tex(b, (string)t[1], 0x4000), 0x4000);
                Array.Copy(k, kp, b, Pal(b, (string)t[2], 0x200), 0x200);
            }

            // the options screen: the European part loads 200 colours
            int ot = Tex(k, "title_option", 0x4000), op = Pal(k, "title_option", 0x200);
            var opal = Colours(k, op, 256);
            var below = new int[256];
            for (int i = 0; i < 256; i++) below[i] = i < 200 ? i : Nearest(opal[i], 1, 200, opal);
            int at = Tex(e, "title_option_EU", 0x4000), pat = Pal(e, "title_option_pl", 0x190);
            for (int i = 0; i < 0x4000; i++) e[at + i] = (byte)below[k[ot + i]];
            Array.Copy(k, op, e, pat, 0x190);
            // its OK button (title_opt_ok's two, 80 and 96 wide): the Korean OK in each
            at = Tex(e, "title_opt_ok", 0x2000);
            pat = Pal(e, "title_opt_ok", 0x200);
            var ok = new byte[0x2000];
            for (int y = 0; y < 32; y++)
            {
                Array.Copy(k, ot + (32 + y) * 256 + 192, ok, y * 256 + 8, 64);
                Array.Copy(k, ot + (32 + y) * 256 + 192, ok, y * 256 + 96, 64);
            }
            Array.Copy(ok, 0, e, at, 0x2000);
            Array.Copy(k, op, e, pat, 0x200);
            outE = Lz77Wrap(e);
            outS = Lz77Wrap(s);
        }

        static string OnDisk(string root, string p) { return Path.Combine(root, p.Replace('/', '\\')); }

        // The Korean files over the European ones in work (Extract.Run's folder).
        public static void Apply(string work, Data ko, Action<string> log)
        {
            string filesRoot = Path.Combine(work, "extracted", "dsd", "files");
            var neu = new Dictionary<string, byte[]>();
            foreach (var p in Files)
                if (p != Bank && p != TexBank) neu[p] = ko.Files[p];
            foreach (var p in BankSlots) neu[p] = ko.Files[Bank];
            byte[] e, s;
            Texbanks(File.ReadAllBytes(OnDisk(filesRoot, TexBank)), File.ReadAllBytes(OnDisk(filesRoot, TexBankS)),
                     ko.Files[TexBank], out e, out s);
            neu[TexBank] = e;
            neu[TexBankS] = s;

            int changed = Languages.ReplaceFiles(work, neu, "Korean");
            string assets = Path.Combine(work, "build", "assets");
            File.WriteAllBytes(Path.Combine(assets, "korean.bin"), ko.Bin);
            File.WriteAllBytes(Path.Combine(assets, "langpack.bin"), Languages.Langpack("ko", null));
            log("    " + neu.Count + " files Korean, " + changed + " entries in the file table updated");
        }
    }
}
