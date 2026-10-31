// The HOME Menu banner's 3D model: the game's title logo with "ENHANCED"
// under it, and the Power Star flying round it on its path from the opening
// (port3ds/banner/face).
//
// face.template is that banner (a CGFX) with every value that comes from the
// game zeroed; face.recipe says where each value goes and what it is computed
// from. Both are made by port3ds/banner/face/face_recipe.py, and nothing in
// them is the game's. Here the values are computed from the dump's own files
// (the recipe's file lines, checked by size and SHA-256; its model lines say
// which make which model) the same way face_recipe.py computes them, and
// written into the template.
//
// Layouts are the game's, from the decomp (include/BMD_File.h, the samplers
// func_020457f0 / func_020456a0, the texture sequence func_02046bbc) and
// port/ntr/bmd.cpp; ndsfmt.py has the same readers in Python.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Sm64dsEnhanced
{
    static class Face
    {
        const double Turn = Math.PI * 2 / 4096;

        // The banner, or null if the dump's files aren't the ones the recipe
        // was made from (the caller then makes the flat banner). The logo is
        // the game's title-screen logo (Art.TitleLogo); without one, that
        // part of the banner stays empty.
        public static byte[] Banner(string dsmt, byte[] template, string[] recipe, Bitmap logo, Action<string> log)
        {
            var files = new Dictionary<string, byte[]>();
            var models = new Dictionary<string, Model>();
            int start = -1, end = -1, blend = -1;
            foreach (var line in recipe)
            {
                if (line.Length == 0) continue;
                var f = line.Split('\t');
                if (f[0] == "file")
                {
                    var path = Path.Combine(dsmt, f[1]);
                    if (!File.Exists(path)) { log("Banner: no " + f[1] + " in the extracted files."); return null; }
                    var b = File.ReadAllBytes(path);
                    if (b.Length != int.Parse(f[2]) || Sha(b) != f[3])
                    {
                        log("Banner: " + f[1] + " is not the one the banner was made from.");
                        return null;
                    }
                    files[f[1]] = b;
                }
                else if (f[0] == "model")
                {
                    // TAG BMD BCA BTP (- for none), from the files above
                    if (!files.ContainsKey(f[2]) || !files.ContainsKey(f[3]) || (f[4] != "-" && !files.ContainsKey(f[4])))
                        throw new BuildError("The banner's recipe is damaged.");
                    models[f[1]] = new Model(new Bmd(files[f[2]]), new Bca(files[f[3]]),
                                             f[4] == "-" ? null : new Btp(files[f[4]]));
                }
                else if (f[0] == "window")
                {
                    start = int.Parse(f[1]); end = int.Parse(f[2]); blend = int.Parse(f[3]);
                }
            }
            if (models.Count == 0 || start < 0) throw new BuildError("The banner's recipe is damaged.");
            var keys = Window(start, end, blend);

            var cgfx = (byte[])template.Clone();
            var curves = new Dictionary<string, Dictionary<string, double[]>>();
            Dictionary<string, double[]> lids = null;
            foreach (var line in recipe)
            {
                if (line.Length == 0) continue;
                var f = line.Split('\t');
                if (f[0] == "file" || f[0] == "model" || f[0] == "window" || f[0].StartsWith("sm64ds")) continue;
                var offs = new List<int>();
                foreach (var o in f[1].Split(',')) offs.Add(int.Parse(o));
                if (f[0] == "logo")
                {
                    if (logo != null)
                    {
                        var data = Swizzle(Centred(logo, int.Parse(f[3]), int.Parse(f[4])), int.Parse(f[3]),
                                           int.Parse(f[4]), int.Parse(f[2]));
                        foreach (var o in offs) Put(cgfx, o, data);
                    }
                    continue;
                }
                if (!models.ContainsKey(f[2])) throw new BuildError("The banner's recipe is damaged.");
                Model m = models[f[2]];
                switch (f[0])
                {
                    case "vtx":
                    case "idx":
                    {
                        bool root = f[f.Length - 1] == "root";   // a rigid part, in its root bone's space
                        var part = m.Part(int.Parse(f[3]), int.Parse(f[4]), root);
                        byte[] data = f[0] == "idx" ? part.Indices(f[6] == "1") : part.Stream(f[5], f[7] == "1");
                        foreach (var o in offs) Put(cgfx, o, data);
                        break;
                    }
                    case "tex":
                    {
                        int ti = int.Parse(f[3]), pi = int.Parse(f[4]);
                        var data = Swizzle(m.Bmd.Rgba(ti, pi), m.Bmd.Textures[ti].W, m.Bmd.Textures[ti].H,
                                           int.Parse(f[5]));
                        foreach (var o in offs) Put(cgfx, o, data);
                        break;
                    }
                    case "rest":
                    {
                        var b = m.Bmd.Bones[int.Parse(f[3])];
                        var v = new double[9];
                        for (int k = 0; k < 3; k++)
                        {
                            v[k] = b.Scale[k]; v[3 + k] = b.Rot[k] * Turn; v[6 + k] = b.Trans[k];
                        }
                        foreach (var o in offs) Floats(cgfx, o, 4, v);
                        break;
                    }
                    case "ibm":
                    {
                        var inv = Invert(m.Bind[int.Parse(f[3])]);
                        var v = new double[12];
                        for (int r = 0; r < 3; r++)
                            for (int c = 0; c < 4; c++) v[r * 4 + c] = (float)inv[c * 4 + r];
                        foreach (var o in offs) Floats(cgfx, o, 4, v);
                        break;
                    }
                    case "const":
                    case "keys":
                    {
                        if (!curves.ContainsKey(f[2])) curves[f[2]] = m.BoneCurves(keys);
                        var vals = curves[f[2]][f[3] + "/" + f[4] + "/" + f[5]];
                        Curve(cgfx, offs, f[0] == "keys" ? f[6] : null, vals);
                        break;
                    }
                    case "lidc":
                    case "lidk":
                    {
                        if (lids == null) lids = m.LidCurves(keys);
                        var vals = lids[f[3] + "/" + f[4] + "/" + f[5] + "/" + f[6]];
                        Curve(cgfx, offs, f[0] == "lidk" ? f[7] : null, vals);
                        break;
                    }
                    default:
                        throw new BuildError("The face banner's recipe is damaged (" + f[0] + ").");
                }
            }
            return cgfx;
        }

        // a constant (the value once) or keys ((frame, value) pairs: OFF is the
        // first key's value, key i's 8i bytes on, for window key K[i])
        static void Curve(byte[] cgfx, List<int> offs, string keyList, double[] vals)
        {
            if (keyList == null)
            {
                foreach (var o in offs) Floats(cgfx, o, 4, new[] { vals[0] });
                return;
            }
            var ks = keyList.Split(',');
            var v = new double[ks.Length];
            for (int i = 0; i < ks.Length; i++) v[i] = vals[int.Parse(ks[i])];
            foreach (var o in offs) Floats(cgfx, o, 8, v);
        }

        // the logo, edges cleaned, centred in a w x h RGBA texture, top row
        // first (face_gltf.logo_image). The game draws it over its blue sky, so
        // the pixels just outside its outline are a light blue halo anywhere
        // else: an opaque pixel touching transparency (or the picture's edge)
        // that is light (a channel at 100 or more) and bluish (blue more than
        // red + 8) goes, in one pass (face_gltf.clean_edges)
        static byte[] Centred(Bitmap pic, int w, int h)
        {
            int pw = pic.Width, ph = pic.Height;
            var c = new Color[pw, ph];
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++) c[x, y] = pic.GetPixel(x, y);
            var drop = new bool[pw, ph];
            int[] dx = { 1, -1, 0, 0 }, dy = { 0, 0, 1, -1 };
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    var p = c[x, y];
                    if (p.A == 0 || Math.Max(p.R, Math.Max(p.G, p.B)) < 100 || p.B <= p.R + 8) continue;
                    for (int k = 0; k < 4; k++)
                    {
                        int nx = x + dx[k], ny = y + dy[k];
                        if (nx < 0 || ny < 0 || nx >= pw || ny >= ph || c[nx, ny].A == 0) { drop[x, y] = true; break; }
                    }
                }
            var o = new byte[w * h * 4];
            int x0 = (w - pw) / 2, y0 = (h - ph) / 2;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    int X = x0 + x, Y = y0 + y;
                    if (X < 0 || Y < 0 || X >= w || Y >= h || drop[x, y]) continue;
                    var p = c[x, y];
                    int at = (Y * w + X) * 4;
                    o[at] = p.R; o[at + 1] = p.G; o[at + 2] = p.B; o[at + 3] = p.A;
                }
            return o;
        }

        static void Put(byte[] cgfx, int at, byte[] data)
        {
            if (at < 0 || at + data.Length > cgfx.Length) throw new BuildError("The face banner's recipe is damaged.");
            Array.Copy(data, 0, cgfx, at, data.Length);
        }

        static void Floats(byte[] cgfx, int at, int step, double[] v)
        {
            for (int i = 0; i < v.Length; i++) Put(cgfx, at + i * step, BitConverter.GetBytes((float)v[i]));
        }

        static string Sha(byte[] b)
        {
            using (var h = SHA256.Create())
                return BitConverter.ToString(h.ComputeHash(b)).Replace("-", "").ToLowerInvariant();
        }

        // The game frames the loop plays (face_gltf.window): start..end-1, then
        // start again; over the last `blend` frames the pose crossfades into the
        // frames just before start. Each is (frame a, frame b, weight of b).
        struct Key { public int A, B; public double T; }

        static List<Key> Window(int start, int end, int blend)
        {
            var w = new List<Key>();
            for (int f = start; f < end; f++)
            {
                int k = f - (end - blend);
                w.Add(k >= 0 ? new Key { A = f, B = start - blend + k, T = (k + 1) / (double)blend }
                             : new Key { A = f, B = f, T = 0 });
            }
            w.Add(new Key { A = start, B = start, T = 0 });
            return w;
        }

        // --- matrices: 4x4, row-vector convention, row-major ----------------

        // sin of a DS angle (1/4096 turns), the same to the bit wherever it runs:
        // Math.Sin and Python's differ in the last bit at 269 of the 4096 angles,
        // so this folds the angle into the first eighth of a turn and sums the
        // series in plain doubles, as sm64ds_builder.py's sin4096 does
        static double Sin4096(int a)
        {
            int q = ((a % 4096) + 4096) % 4096;
            bool neg = q >= 2048;
            if (neg) q -= 2048;
            if (q > 1024) q = 2048 - q;
            double v = q <= 512 ? SinSeries(q * (Math.PI / 2048)) : CosSeries((1024 - q) * (Math.PI / 2048));
            return neg ? -v : v;
        }

        static double SinSeries(double x)
        {
            double x2 = x * x;
            return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110 *
                       (1 - x2 / 156 * (1 - x2 / 210 * (1 - x2 / 272))))))));
        }

        static double CosSeries(double x)
        {
            double x2 = x * x;
            return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90 *
                   (1 - x2 / 132 * (1 - x2 / 182 * (1 - x2 / 240)))))));
        }

        static double[] Euler(int rx, int ry, int rz)
        {
            double a0 = Sin4096(rx), a1 = Sin4096(rx + 1024);
            double b0 = Sin4096(ry), b1 = Sin4096(ry + 1024);
            double c0 = Sin4096(rz), c1 = Sin4096(rz + 1024);
            return new[] { b1 * c1, b1 * c0, -b0,
                           b0 * a0 * c1 - a1 * c0, a1 * c1 + b0 * a0 * c0, b1 * a0,
                           a0 * c0 + b0 * a1 * c1, b0 * a1 * c0 - a0 * c1, b1 * a1 };
        }

        static double[] Local(double[] scale, int[] rot, double[] trans)
        {
            var r = Euler(rot[0], rot[1], rot[2]);
            var m = new double[16];
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) m[i * 4 + j] = scale[i] * r[i * 3 + j];
            m[12] = trans[0]; m[13] = trans[1]; m[14] = trans[2]; m[15] = 1;
            return m;
        }

        static double[] Mul(double[] a, double[] b)
        {
            var r = new double[16];
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                {
                    double s = 0;
                    for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
                    r[i * 4 + j] = s;
                }
            return r;
        }

        // v (x, y, z, w) times m; the first three
        static double[] Apply(double[] v, double[] m, int n)
        {
            var r = new double[3];
            for (int j = 0; j < 3; j++)
            {
                double s = 0;
                for (int k = 0; k < n; k++) s += v[k] * m[k * 4 + j];
                r[j] = s;
            }
            return r;
        }

        static double[] Invert(double[] m)
        {
            var a = (double[])m.Clone();
            var r = new double[16];
            for (int i = 0; i < 4; i++) r[i * 5] = 1;
            for (int c = 0; c < 4; c++)
            {
                int p = c;
                for (int i = c + 1; i < 4; i++) if (Math.Abs(a[i * 4 + c]) > Math.Abs(a[p * 4 + c])) p = i;
                for (int j = 0; j < 4; j++)
                {
                    double t = a[c * 4 + j]; a[c * 4 + j] = a[p * 4 + j]; a[p * 4 + j] = t;
                    t = r[c * 4 + j]; r[c * 4 + j] = r[p * 4 + j]; r[p * 4 + j] = t;
                }
                double d = a[c * 4 + c];
                for (int j = 0; j < 4; j++) { a[c * 4 + j] /= d; r[c * 4 + j] /= d; }
                for (int i = 0; i < 4; i++)
                {
                    if (i == c) continue;
                    double k = a[i * 4 + c];
                    for (int j = 0; j < 4; j++) { a[i * 4 + j] -= k * a[c * 4 + j]; r[i * 4 + j] -= k * r[c * 4 + j]; }
                }
            }
            return r;
        }

        // --- the game's files ---------------------------------------------

        static uint U32(byte[] d, int o) { return BitConverter.ToUInt32(d, o); }
        static int S32(byte[] d, int o) { return BitConverter.ToInt32(d, o); }
        static int U16(byte[] d, int o) { return BitConverter.ToUInt16(d, o); }
        static int S16(byte[] d, int o) { return BitConverter.ToInt16(d, o); }

        static string Name(byte[] d, int o)
        {
            if (o == 0) return "";
            int e = o;
            while (e < d.Length && d[e] != 0) e++;
            return Encoding.ASCII.GetString(d, o, e - o);
        }

        static int Sx(uint v, int bits)
        {
            int x = (int)(v & ((1u << bits) - 1));
            return (x & (1 << (bits - 1))) != 0 ? x - (1 << bits) : x;
        }

        // the game's LZ77 files: "LZ77" and a type-0x10 stream, or plain
        static byte[] Lz77(byte[] raw)
        {
            int i = raw.Length >= 4 && raw[0] == 'L' && raw[1] == 'Z' && raw[2] == '7' && raw[3] == '7' ? 4 : 0;
            if (raw[i] != 0x10) return raw;
            int n = raw[i + 1] | raw[i + 2] << 8 | raw[i + 3] << 16;
            var o = new byte[n];
            int at = 0;
            i += 4;
            while (at < n)
            {
                int flags = raw[i++];
                for (int b = 0; b < 8 && at < n; b++)
                {
                    if ((flags & (0x80 >> b)) != 0)
                    {
                        int v = raw[i] << 8 | raw[i + 1];
                        i += 2;
                        int len = (v >> 12) + 3, disp = (v & 0xfff) + 1;
                        for (int k = 0; k < len && at < n; k++, at++) o[at] = o[at - disp];
                    }
                    else o[at++] = raw[i++];
                }
            }
            return o;
        }

        static int[] Rgb555(int c)
        {
            int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            return new[] { r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2 };
        }

        class Bone { public int Parent; public double[] Scale = new double[3], Trans = new double[3]; public int[] Rot = new int[3]; public byte[] Mats, Dls; }
        class Dl { public byte[] Transforms, Data; }
        class Tex { public string Name; public byte[] Data, Extra; public uint Param; public int Fmt, W, H; }
        class Mat { public string Name; public int Tex, Pal; public uint[] Words = new uint[9]; }

        // A BMD0 model (include/BMD_File.h)
        class Bmd
        {
            public byte[] D;
            public int ScaleShift;
            public List<Bone> Bones = new List<Bone>();
            public List<Dl> Dls = new List<Dl>();
            public List<Tex> Textures = new List<Tex>();
            public List<string> PaletteNames = new List<string>();
            public List<int[][]> Palettes = new List<int[][]>();
            public List<Mat> Materials = new List<Mat>();
            public int[] Tmap = new int[32];

            public Bmd(byte[] raw)
            {
                var d = D = Lz77(raw);
                ScaleShift = (int)U32(d, 0);
                for (int i = 0; i < U32(d, 4); i++)
                {
                    int o = (int)U32(d, 8) + i * 0x40;
                    int n = (int)U32(d, o + 0x30);
                    var b = new Bone();
                    b.Parent = S16(d, o + 8) != 0 ? S16(d, o) + S16(d, o + 8) : -1;
                    for (int k = 0; k < 3; k++)
                    {
                        b.Scale[k] = S32(d, o + 0x10 + k * 4) / 4096.0;
                        b.Rot[k] = S16(d, o + 0x1c + k * 2);
                        b.Trans[k] = S32(d, o + 0x24 + k * 4) / 4096.0;
                    }
                    b.Mats = Slice(d, (int)U32(d, o + 0x34), n);
                    b.Dls = Slice(d, (int)U32(d, o + 0x38), n);
                    Bones.Add(b);
                }
                for (int g = 0; g < U32(d, 0x0c); g++)
                {
                    int rec = (int)U32(d, 0x10) + g * 8;
                    for (int k = 0; k < U32(d, rec); k++)
                    {
                        int s = (int)U32(d, rec + 4) + k * 0x10;
                        Dls.Add(new Dl { Transforms = Slice(d, (int)U32(d, s + 4), (int)U32(d, s)),
                                         Data = Slice(d, (int)U32(d, s + 0xc), (int)U32(d, s + 8)) });
                    }
                }
                for (int i = 0; i < U32(d, 0x14); i++)
                {
                    int o = (int)U32(d, 0x18) + i * 0x14;
                    uint p = U32(d, o + 0x10);
                    int at = (int)U32(d, o + 4), size = (int)U32(d, o + 8);
                    Textures.Add(new Tex { Name = Name(d, (int)U32(d, o)), Data = Slice(d, at, size),
                                           Extra = Slice(d, at + size, d.Length - at - size), Param = p,
                                           Fmt = (int)(p >> 26) & 7, W = 8 << (int)((p >> 20) & 7),
                                           H = 8 << (int)((p >> 23) & 7) });
                }
                for (int i = 0; i < U32(d, 0x1c); i++)
                {
                    int o = (int)U32(d, 0x20) + i * 0x10;
                    int at = (int)U32(d, o + 4), size = (int)U32(d, o + 8);
                    var cols = new int[size / 2][];
                    for (int k = 0; k < cols.Length; k++) cols[k] = Rgb555(U16(d, at + k * 2));
                    PaletteNames.Add(Name(d, (int)U32(d, o)));
                    Palettes.Add(cols);
                }
                for (int i = 0; i < U32(d, 0x24); i++)
                {
                    int o = (int)U32(d, 0x28) + i * 0x30;
                    var m = new Mat { Name = Name(d, (int)U32(d, o)), Tex = S32(d, o + 4), Pal = S32(d, o + 8) };
                    for (int k = 0; k < 9; k++) m.Words[k] = U32(d, o + 0x0c + k * 4);
                    Materials.Add(m);
                }
                // transform ID -> bone: a display list's MTX_RESTORE k is its
                // Transforms[k], which is this table's bone
                for (int i = 0; i < 32; i++) Tmap[i] = U16(d, (int)U32(d, 0x2c) + i * 2);
            }

            static byte[] Slice(byte[] d, int at, int n)
            {
                var r = new byte[Math.Max(0, Math.Min(n, d.Length - at))];
                Array.Copy(d, at, r, 0, r.Length);
                return r;
            }

            // a texture as RGBA bytes, top row first (port/ntr/texture.cpp)
            public byte[] Rgba(int ti, int pi)
            {
                var t = Textures[ti];
                int[][] pal = pi >= 0 ? Palettes[pi] : new int[0][];
                bool transparent0 = ((t.Param >> 29) & 1) != 0;
                var o = new byte[t.W * t.H * 4];
                Func<int, int[]> c = delegate (int i) { return i < pal.Length ? pal[i] : new[] { 0, 0, 0 }; };
                Action<int, int[], int> put = delegate (int px, int[] rgb, int a)
                {
                    o[px * 4] = (byte)rgb[0]; o[px * 4 + 1] = (byte)rgb[1]; o[px * 4 + 2] = (byte)rgb[2]; o[px * 4 + 3] = (byte)a;
                };
                var d = t.Data;
                if (t.Fmt == 5)
                {
                    int bw = t.W / 4;
                    for (int blk = 0; blk < bw * (t.H / 4); blk++)
                    {
                        uint texels = U32(d, blk * 4);
                        int info = U16(t.Extra, blk * 2);
                        int b = (info & 0x3fff) * 2, mode = info >> 14;
                        int[] c0 = c(b), c1 = c(b + 1);
                        int[][] four;
                        if (mode == 0) four = new[] { c0, c1, c(b + 2), null };
                        else if (mode == 1) four = new[] { c0, c1, Blend(c0, c1, 1, 1), null };
                        else if (mode == 2) four = new[] { c0, c1, c(b + 2), c(b + 3) };
                        else four = new[] { c0, c1, Blend(c0, c1, 5, 3), Blend(c0, c1, 3, 5) };
                        for (int k = 0; k < 16; k++)
                        {
                            var p = four[(texels >> (k * 2)) & 3];
                            int px = ((blk / bw) * 4 + k / 4) * t.W + (blk % bw) * 4 + k % 4;
                            if (p != null) put(px, p, 255);
                        }
                    }
                    return o;
                }
                for (int i = 0; i < t.W * t.H; i++)
                {
                    if (t.Fmt == 1) put(i, c(d[i] & 31), (d[i] >> 5) * 255 / 7);
                    else if (t.Fmt == 6) put(i, c(d[i] & 7), (d[i] >> 3) * 255 / 31);
                    else if (t.Fmt >= 2 && t.Fmt <= 4)
                    {
                        int bits = t.Fmt == 2 ? 2 : t.Fmt == 3 ? 4 : 8;
                        int v = (d[i * bits / 8] >> ((i * bits) % 8)) & ((1 << bits) - 1);
                        put(i, c(v), v == 0 && transparent0 ? 0 : 255);
                    }
                    else if (t.Fmt == 7)
                    {
                        int v = U16(d, i * 2);
                        put(i, Rgb555(v), (v & 0x8000) != 0 ? 255 : 0);
                    }
                }
                return o;
            }

            static int[] Blend(int[] a, int[] b, int wa, int wb)
            {
                var r = new int[3];
                for (int k = 0; k < 3; k++) r[k] = (a[k] * wa + b[k] * wb) / (wa + wb);
                return r;
            }
        }

        // A BCA joint animation, sampled as func_0204547c does
        class Bca
        {
            byte[] d;
            public int Frames;
            int pScale, pRot, pTrans, tracks;

            public Bca(byte[] raw)
            {
                d = Lz77(raw);
                Frames = U16(d, 2);
                pScale = (int)U32(d, 8); pRot = (int)U32(d, 0xc); pTrans = (int)U32(d, 0x10); tracks = (int)U32(d, 0x14);
            }

            int Fix(int pool, int shift, int flag, int start, int f)    // func_020457f0
            {
                Func<int, int> t = delegate (int k) { return S32(d, pool + (start + k) * 4); };
                if (flag == 0) return t(0);
                if (shift == 0) return t(f);
                int b = ((Frames - 1) >> shift) << shift;
                int i = f >> shift;
                if (f >= b) return t(i + (f - b));
                int rem = f - (i << shift);
                return rem == 0 ? t(i) : (t(i) * ((1 << shift) - rem) + t(i + 1) * rem) >> shift;
            }

            int Angle(int shift, int flag, int start, int f)           // func_020456a0, 12-bit angles
            {
                Func<int, int> t = delegate (int k) { return S16(d, pRot + (start + k) * 2); };
                if (flag == 0) return t(0);
                if (shift == 0) return t(f);
                int i = f >> shift;
                int b = ((Frames - 1) >> shift) << shift;
                if (f >= b) return t(i + (f - b));
                int frac = f - (i << shift);
                int lo = t(i), hi = t(i + 1);
                if (frac == 0) return lo;
                int range = (1 << shift) - frac;
                if (Math.Abs(lo - hi) >= 0x800)
                {
                    if (lo > hi) hi += 0x1000; else lo += 0x1000;
                    return ((hi * frac + lo * range) >> shift) & 0xfff;
                }
                return (lo * range + hi * frac) >> shift;
            }

            // scale, rotation (1/4096 turns) and translation at frame f
            public void Sample(int bone, int f, double[] scale, int[] rot, double[] trans)
            {
                int o = tracks + bone * 0x24;
                Func<int, int> sh = delegate (int k) { return d[o + k * 4]; };
                Func<int, int> fl = delegate (int k) { return d[o + k * 4 + 1]; };
                Func<int, int> st = delegate (int k) { return U16(d, o + k * 4 + 2); };
                bool any = false;
                for (int k = 0; k < 3; k++) if (st(k) != 0 || fl(k) != 0) any = true;
                for (int k = 0; k < 3; k++)
                {
                    scale[k] = any ? Fix(pScale, sh(k), fl(k), st(k), f) / 4096.0 : 1.0;
                    rot[k] = Angle(sh(3 + k), fl(3 + k), st(3 + k), f);
                    trans[k] = Fix(pTrans, sh(6 + k), fl(6 + k), st(6 + k), f) / 4096.0;
                }
            }
        }

        // A BTP texture sequence: per material name, its (first frame, texture,
        // palette) keys (func_02046bbc)
        class Btp
        {
            public int Frames;
            public List<string> Order = new List<string>();
            public Dictionary<string, List<Tuple<int, string, string>>> Seqs =
                new Dictionary<string, List<Tuple<int, string, string>>>();

            public Btp(byte[] raw)
            {
                var d = Lz77(raw);
                Frames = U16(d, 0);
                var texs = new List<string>();
                for (int i = 0; i < U16(d, 2); i++) texs.Add(Name(d, (int)U32(d, (int)U32(d, 4) + i * 8 + 4)));
                var pals = new List<string>();
                for (int i = 0; i < U16(d, 8); i++) pals.Add(Name(d, (int)U32(d, (int)U32(d, 0xc) + i * 8 + 4)));
                int p10 = (int)U32(d, 0x10), p14 = (int)U32(d, 0x14), p18 = (int)U32(d, 0x18);
                for (int i = 0; i < U16(d, 0x1c); i++)
                {
                    int e = (int)U32(d, 0x20) + i * 0xc;
                    int cnt = U16(d, e + 8), off = U16(d, e + 0xa);
                    var keys = new List<Tuple<int, string, string>>();
                    for (int j = 0; j < cnt; j++)
                        keys.Add(Tuple.Create(U16(d, p10 + (off + j) * 2), texs[U16(d, p14 + (off + j) * 2)],
                                              pals.Count > 0 ? pals[U16(d, p18 + (off + j) * 2)] : null));
                    var nm = Name(d, (int)U32(d, e + 4));
                    Order.Add(nm);
                    Seqs[nm] = keys;
                }
            }
        }

        // one vertex of a display list: in the bound bone's space, with the DS's
        // own values (exact), which is what tells shared vertices apart
        class Vtx { public double[] Pos = new double[3]; public int Slot; public double[] Normal; public double U, V; }

        // The geometry engine's packed commands: parameter words per command
        static readonly Dictionary<int, int> Params = new Dictionary<int, int> {
            { 0x00, 0 }, { 0x10, 1 }, { 0x11, 0 }, { 0x12, 1 }, { 0x13, 1 }, { 0x14, 1 }, { 0x15, 0 },
            { 0x16, 16 }, { 0x17, 12 }, { 0x18, 16 }, { 0x19, 12 }, { 0x1a, 9 }, { 0x1b, 3 }, { 0x1c, 3 },
            { 0x20, 1 }, { 0x21, 1 }, { 0x22, 1 }, { 0x23, 2 }, { 0x24, 1 }, { 0x25, 1 }, { 0x26, 1 },
            { 0x27, 1 }, { 0x28, 1 }, { 0x29, 1 }, { 0x2a, 1 }, { 0x2b, 1 }, { 0x30, 1 }, { 0x31, 1 },
            { 0x32, 1 }, { 0x33, 1 }, { 0x34, 32 }, { 0x40, 1 }, { 0x41, 0 }, { 0x50, 1 }, { 0x60, 1 },
            { 0x70, 3 }, { 0x71, 2 }, { 0x72, 1 } };

        // a display list's triangles (ndsfmt.display_list)
        static List<Vtx[]> DisplayList(byte[] data)
        {
            var words = new uint[data.Length / 4];
            for (int i = 0; i < words.Length; i++) words[i] = U32(data, i * 4);
            var pos = new double[3];
            int slot = 0, prim = -1;
            double[] normal = null;
            double u = 0, v = 0;
            var tris = new List<Vtx[]>();
            var strip = new List<Vtx>();
            Action emit = delegate
            {
                var s = strip;
                if (prim == 0) for (int i = 0; i + 2 < s.Count; i += 3) tris.Add(new[] { s[i], s[i + 1], s[i + 2] });
                else if (prim == 1)
                    for (int i = 0; i + 3 < s.Count; i += 4)
                    {
                        tris.Add(new[] { s[i], s[i + 1], s[i + 2] });
                        tris.Add(new[] { s[i], s[i + 2], s[i + 3] });
                    }
                else if (prim == 2)
                    for (int i = 0; i + 2 < s.Count; i++)
                        tris.Add(i % 2 == 0 ? new[] { s[i], s[i + 1], s[i + 2] } : new[] { s[i + 1], s[i], s[i + 2] });
                else if (prim == 3)
                    for (int i = 0; i + 3 < s.Count; i += 2)
                    {
                        tris.Add(new[] { s[i], s[i + 1], s[i + 3] });
                        tris.Add(new[] { s[i], s[i + 3], s[i + 2] });
                    }
            };
            Action vertex = delegate
            {
                strip.Add(new Vtx { Pos = (double[])pos.Clone(), Slot = slot, Normal = normal, U = u, V = v });
            };
            int w = 0;
            while (w < words.Length)
            {
                uint packed = words[w++];
                for (int k = 0; k < 4; k++)
                {
                    int cmd = (int)(packed >> (k * 8)) & 0xff;
                    int n;
                    if (!Params.TryGetValue(cmd, out n)) throw new BuildError("The face model has an unknown command.");
                    uint p0 = n > 0 && w < words.Length ? words[w] : 0, p1 = n > 1 && w + 1 < words.Length ? words[w + 1] : 0;
                    w += n;
                    switch (cmd)
                    {
                        case 0x14: slot = (int)(p0 & 31); break;
                        case 0x21: normal = new[] { Sx(p0, 10) / 512.0, Sx(p0 >> 10, 10) / 512.0, Sx(p0 >> 20, 10) / 512.0 }; break;
                        case 0x22: u = Sx(p0, 16) / 16.0; v = Sx(p0 >> 16, 16) / 16.0; break;
                        case 0x23: pos = new[] { Sx(p0, 16) / 4096.0, Sx(p0 >> 16, 16) / 4096.0, Sx(p1, 16) / 4096.0 }; vertex(); break;
                        case 0x24: pos = new[] { Sx(p0, 10) / 64.0, Sx(p0 >> 10, 10) / 64.0, Sx(p0 >> 20, 10) / 64.0 }; vertex(); break;
                        case 0x25: pos[0] = Sx(p0, 16) / 4096.0; pos[1] = Sx(p0 >> 16, 16) / 4096.0; vertex(); break;
                        case 0x26: pos[0] = Sx(p0, 16) / 4096.0; pos[2] = Sx(p0 >> 16, 16) / 4096.0; vertex(); break;
                        case 0x27: pos[1] = Sx(p0, 16) / 4096.0; pos[2] = Sx(p0 >> 16, 16) / 4096.0; vertex(); break;
                        case 0x28:
                            pos[0] += Sx(p0, 10) / 32768.0; pos[1] += Sx(p0 >> 10, 10) / 32768.0; pos[2] += Sx(p0 >> 20, 10) / 32768.0;
                            vertex();
                            break;
                        case 0x40:
                            if (strip.Count > 0) emit();
                            strip = new List<Vtx>();
                            prim = (int)(p0 & 3);
                            break;
                        case 0x41: emit(); strip = new List<Vtx>(); break;
                    }
                }
            }
            if (strip.Count > 0) emit();
            return tris;
        }

        // one vertex of a part, in model space at the bind pose
        class PVtx { public double[] Pos, Nrm; public double U, V; public int Bone; public string Raw; }
        class Prim { public int Material; public List<PVtx[]> Tris = new List<PVtx[]>(); }

        class Part
        {
            public List<double[]> Pos = new List<double[]>(), Nrm = new List<double[]>(), Uv = new List<double[]>();
            public List<int> Jnt = new List<int>(), Idx = new List<int>();

            // as pycgfx stores it: floats (or the joints plus one as shorts),
            // twice over for a double-sided part, the second time with the
            // normals turned round
            public byte[] Stream(string attr, bool dbl)
            {
                var o = new List<byte>();
                for (int i = 0; i < Pos.Count; i++)
                {
                    if (attr == "jnt")
                        foreach (var x in new[] { Jnt[i] + 1, 1, 1, 1 }) o.AddRange(BitConverter.GetBytes((short)x));
                    else
                        foreach (var x in attr == "pos" ? Pos[i] : attr == "nrm" ? Nrm[i] : Uv[i])
                            o.AddRange(BitConverter.GetBytes((float)x));
                }
                var once = o.ToArray();
                if (!dbl) return once;
                var r = new byte[once.Length * 2];
                Array.Copy(once, r, once.Length);
                Array.Copy(once, 0, r, once.Length, once.Length);
                if (attr == "nrm") for (int i = once.Length + 3; i < r.Length; i += 4) r[i] ^= 0x80;
                return r;
            }

            public byte[] Indices(bool dbl)
            {
                var o = new List<byte>();
                foreach (var i in Idx) o.AddRange(BitConverter.GetBytes((ushort)i));
                if (dbl) for (int k = Idx.Count - 1; k >= 0; k--) o.AddRange(BitConverter.GetBytes((ushort)(Idx[k] + Pos.Count)));
                return o.ToArray();
            }
        }

        class Blink { public List<string> Textures = new List<string>(); public List<int> Shown = new List<int>(); }

        // One BMD with its animation (face_gltf.Model)
        class Model
        {
            public Bmd Bmd;
            Bca bca;
            Btp btp;
            public double[][] Bind;
            List<Prim> prims = new List<Prim>();
            Dictionary<string, Blink> blinks = new Dictionary<string, Blink>();

            public Model(Bmd bmd, Bca bca, Btp btp)
            {
                Bmd = bmd; this.bca = bca; this.btp = btp;
                Bind = new double[bmd.Bones.Count][];
                for (int i = 0; i < Bind.Length; i++)
                {
                    var b = bmd.Bones[i];
                    var l = Local(b.Scale, b.Rot, b.Trans);
                    Bind[i] = b.Parent >= 0 ? Mul(l, Bind[b.Parent]) : l;
                }
                Geometry();
                if (btp != null) Blinks();
            }

            // One primitive per (display list, material) of the root bone, at
            // the bind pose (face_gltf.Model._geometry)
            void Geometry()
            {
                double s = 1 << Bmd.ScaleShift;
                var root = Bmd.Bones[0];
                for (int pi = 0; pi < root.Mats.Length; pi++)
                {
                    int mi = root.Mats[pi];
                    var dl = Bmd.Dls[root.Dls[pi]];
                    var mat = Bmd.Materials[mi];
                    double sw = 1, shh = 1;
                    if (mat.Tex >= 0) { sw = Bmd.Textures[mat.Tex].W; shh = Bmd.Textures[mat.Tex].H; }
                    bool env = (mat.Words[5] >> 30) == 2;
                    var prim = new Prim { Material = mi };
                    foreach (var tri in DisplayList(dl.Data))
                    {
                        var vs = new PVtx[3];
                        for (int k = 0; k < 3; k++)
                        {
                            var v = tri[k];
                            int bone = Bmd.Tmap[dl.Transforms[v.Slot]];
                            var w = Bind[bone];
                            var pos = Apply(new[] { v.Pos[0] * s, v.Pos[1] * s, v.Pos[2] * s, 1.0 }, w, 4);
                            var n = Apply(v.Normal ?? new[] { 0.0, 1.0, 0.0 }, w, 3);
                            double len = Math.Sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            if (len == 0) len = 1;
                            n = new[] { n[0] / len, n[1] / len, n[2] / len };
                            double uu = v.U / sw, vv = v.V / shh;
                            if (env) { uu = 0.5 + 0.5 * n[0]; vv = 0.5 - 0.5 * n[1]; }
                            var raw = new StringBuilder();
                            foreach (var x in v.Pos) raw.Append(BitConverter.DoubleToInt64Bits(x)).Append(',');
                            raw.Append(bone).Append(';');
                            if (v.Normal != null) foreach (var x in v.Normal) raw.Append(BitConverter.DoubleToInt64Bits(x)).Append(',');
                            raw.Append(';').Append(BitConverter.DoubleToInt64Bits(v.U)).Append(',').Append(BitConverter.DoubleToInt64Bits(v.V));
                            vs[k] = new PVtx { Pos = pos, Nrm = n, U = uu, V = vv, Bone = bone, Raw = raw.ToString() };
                        }
                        prim.Tris.Add(vs);
                    }
                    prims.Add(prim);
                }
            }

            // The eyelid pictures kept, and what each of the sequence's becomes:
            // by the picture's last digit, 1 and 2 show 0 (open), 3 shows 4
            // (closed), the rest themselves (face_gltf.LIDS)
            static string Lid(string t)
            {
                if (t.Length == 0) return t;
                char c = t[t.Length - 1];
                if (c == '1' || c == '2') return t.Substring(0, t.Length - 1) + "0";
                if (c == '3') return t.Substring(0, t.Length - 1) + "4";
                return t;
            }

            // face_gltf.blink_variants
            void Blinks()
            {
                foreach (var mat in btp.Order)
                {
                    var keys = btp.Seqs[mat];
                    var kept = new SortedSet<string>(StringComparer.Ordinal);
                    foreach (var k in keys) if (Lid(k.Item2) == k.Item2) kept.Add(k.Item2);
                    var b = new Blink();
                    b.Textures.AddRange(kept);
                    for (int f = 0; f < btp.Frames; f++)
                    {
                        Tuple<int, string, string> cur = null;
                        foreach (var k in keys) if (k.Item1 <= f) cur = k;
                        b.Shown.Add(b.Textures.IndexOf(Lid(cur.Item2)));
                    }
                    blinks[mat] = b;
                }
            }

            // A part's vertices, shared ones once (face_gltf.build): an eyelid
            // copy or a rigid part (ROOT: the star's) in the root bone's space,
            // anything else in model space with its joints numbered among the
            // bones the part uses
            public Part Part(int pi, int variant, bool root)
            {
                var prim = prims[pi];
                bool lid = root || blinks.ContainsKey(Bmd.Materials[prim.Material].Name);
                var inv0 = Invert(Bind[0]);
                var used = new SortedSet<int>();
                foreach (var t in prim.Tris) foreach (var v in t) used.Add(v.Bone);
                var usedList = new List<int>(used);
                var part = new Part();
                var seen = new Dictionary<string, int>();
                foreach (var t in prim.Tris)
                    foreach (var v in t)
                    {
                        int joint = lid ? 0 : usedList.IndexOf(v.Bone);
                        string key = v.Raw + "|" + joint;
                        int at;
                        if (!seen.TryGetValue(key, out at))
                        {
                            at = part.Pos.Count;
                            seen[key] = at;
                            if (lid)
                            {
                                part.Pos.Add(Apply(new[] { v.Pos[0], v.Pos[1], v.Pos[2], 1.0 }, inv0, 4));
                                part.Nrm.Add(Apply(v.Nrm, inv0, 3));
                            }
                            else
                            {
                                part.Pos.Add(v.Pos);
                                part.Nrm.Add(v.Nrm);
                            }
                            part.Uv.Add(new[] { v.U, v.V });
                            part.Jnt.Add(joint);
                        }
                        part.Idx.Add(at);
                    }
                return part;
            }

            // Per "bone/channel/axis", the value at each window key
            // (face_recipe.bone_curves): rotations straight from the DS angles
            // (the DS's XYZ order is CGFX's), crossfaded, then unwound so
            // neighbouring keys never differ by more than half a turn
            public Dictionary<string, double[]> BoneCurves(List<Key> keys)
            {
                var o = new Dictionary<string, double[]>();
                int n = bca.Frames;
                string[] channels = { "scale", "rot", "pos" };
                for (int bone = 0; bone < Bmd.Bones.Count; bone++)
                {
                    var vals = new double[9][];
                    for (int c = 0; c < 9; c++) vals[c] = new double[keys.Count];
                    double[] sa = new double[3], ta = new double[3], sb = new double[3], tb = new double[3];
                    int[] ra = new int[3], rb = new int[3];
                    for (int i = 0; i < keys.Count; i++)
                    {
                        var k = keys[i];
                        bca.Sample(bone, k.A % n, sa, ra, ta);
                        bca.Sample(bone, k.B % n, sb, rb, tb);
                        double t = k.T;
                        for (int ax = 0; ax < 3; ax++)
                        {
                            double y = rb[ax] + 4096 * Math.Round((ra[ax] - rb[ax]) / 4096.0);
                            vals[ax][i] = sa[ax] * (1 - t) + sb[ax] * t;
                            vals[3 + ax][i] = (ra[ax] * (1 - t) + y * t) * Turn;
                            vals[6 + ax][i] = ta[ax] * (1 - t) + tb[ax] * t;
                        }
                    }
                    for (int ax = 0; ax < 3; ax++)
                        for (int i = 1; i < keys.Count; i++)
                            vals[3 + ax][i] += 2 * Math.PI * Math.Round((vals[3 + ax][i - 1] - vals[3 + ax][i]) / (2 * Math.PI));
                    for (int c = 0; c < 3; c++)
                        for (int ax = 0; ax < 3; ax++)
                            o[bone + "/" + channels[c] + "/" + ax] = vals[c * 3 + ax];
                }
                return o;
            }

            // The eyelid copies' STEP curves, "side/variant/channel/axis": a copy
            // showing sits in place at full size, otherwise inside the head at
            // half size (face_recipe.lid_curves)
            public Dictionary<string, double[]> LidCurves(List<Key> keys)
            {
                var o = new Dictionary<string, double[]>();
                var inv0 = Invert(Bind[0]);
                Func<Func<Prim, bool>, double[]> mean = delegate (Func<Prim, bool> which)
                {
                    var sum = new double[3];
                    int count = 0;
                    foreach (var p in prims)
                    {
                        if (!which(p)) continue;
                        foreach (var t in p.Tris)
                            foreach (var v in t)
                            {
                                var l = Apply(new[] { v.Pos[0], v.Pos[1], v.Pos[2], 1.0 }, inv0, 4);
                                for (int k = 0; k < 3; k++) sum[k] += l[k];
                                count++;
                            }
                    }
                    return new[] { sum[0] / count, sum[1] / count, sum[2] / count };
                };
                var centre = mean(delegate (Prim p) { return true; });
                foreach (var pair in blinks)
                {
                    string name = pair.Key;
                    var mid = mean(delegate (Prim p) { return Bmd.Materials[p.Material].Name == name; });
                    var b = pair.Value;
                    string side = name.Substring(name.Length - 1);
                    for (int variant = 0; variant < b.Textures.Count; variant++)
                    {
                        var pos = new double[3][];
                        var scale = new double[keys.Count];
                        for (int ax = 0; ax < 3; ax++) pos[ax] = new double[keys.Count];
                        for (int i = 0; i < keys.Count; i++)
                        {
                            var k = keys[i];
                            bool on = b.Shown[(k.T < 0.5 ? k.A : k.B) % b.Shown.Count] == variant;
                            for (int ax = 0; ax < 3; ax++) pos[ax][i] = on ? 0 : centre[ax] - 0.5 * mid[ax];
                            scale[i] = on ? 1 : 0.5;
                        }
                        for (int ax = 0; ax < 3; ax++)
                        {
                            o[side + "/" + variant + "/pos/" + ax] = pos[ax];
                            o[side + "/" + variant + "/scale/" + ax] = scale;
                        }
                    }
                }
                return o;
            }
        }

        // RGBA (top row first) as a 3DS texture: bottom row first, in 8x8 tiles
        // of Morton-ordered pixels; format 3 RGB565, 2 RGBA5551 (pycgfx's
        // swizzler, at the formats face_cgfx.py picks)
        static byte[] Swizzle(byte[] rgba, int w, int h, int format)
        {
            var o = new byte[w * h * 2];
            int at = 0;
            for (int ty = 0; ty < h / 8; ty++)
                for (int tx = 0; tx < w / 8; tx++)
                {
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                        {
                            int src = ((h - 1 - (ty * 8 + y)) * w + tx * 8 + x) * 4;
                            int r = rgba[src], g = rgba[src + 1], b = rgba[src + 2], a = rgba[src + 3];
                            int px = format == 3 ? (b >> 3) | ((g & 0xfc) << 3) | ((r & 0xf8) << 8)
                                                 : (a >> 7) | ((b & 0xf8) >> 2) | ((g & 0xf8) << 3) | ((r & 0xf8) << 8);
                            int m = 0;
                            for (int bit = 0; bit < 3; bit++) m |= ((x >> bit) & 1) << (bit * 2) | ((y >> bit) & 1) << (bit * 2 + 1);
                            o[at + m * 2] = (byte)px;
                            o[at + m * 2 + 1] = (byte)(px >> 8);
                        }
                    at += 128;
                }
            return o;
        }
    }
}
