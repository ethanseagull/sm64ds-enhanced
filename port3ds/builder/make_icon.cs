// Draws builder.ico, the builder's file icon: the window's gold star (Look.Star
// in Builder.cs) on a tile of its night sky, at the sizes Explorer asks for.
//
//   csc /nologo /out:make_icon.exe /r:System.Drawing.dll make_icon.cs
//   make_icon builder.ico
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

static class MakeIcon
{
    static GraphicsPath Rounded(RectangleF r, float rad)
    {
        var p = new GraphicsPath();
        float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static byte[] Draw(int n)
    {
        using (var bmp = new Bitmap(n, n, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);
                var tile = new RectangleF(0, 0, n, n);
                using (var path = Rounded(tile, n * 0.22f))
                using (var sky = new LinearGradientBrush(tile, Color.FromArgb(14, 19, 49), Color.FromArgb(29, 36, 86), 90f))
                    g.FillPath(sky, path);
                float outer = n * 0.36f;
                var at = new PointF(n / 2f, n / 2f + n * 0.03f);
                var pts = new PointF[10];
                for (int i = 0; i < 10; i++)
                {
                    double ang = -Math.PI / 2 + i * Math.PI / 5;
                    float rad = i % 2 == 0 ? outer : outer * 0.48f;
                    pts[i] = new PointF(at.X + rad * (float)Math.Cos(ang), at.Y + rad * (float)Math.Sin(ang));
                }
                using (var gold = new LinearGradientBrush(new RectangleF(at.X - outer - 1, at.Y - outer - 1, outer * 2 + 2, outer * 2),
                                                          Color.FromArgb(255, 240, 176), Color.FromArgb(240, 164, 42), 90f))
                using (var pen = new Pen(gold, outer * 0.16f))
                {
                    pen.LineJoin = LineJoin.Round;
                    g.FillPolygon(gold, pts);
                    g.DrawPolygon(pen, pts);
                }
            }
            using (var ms = new MemoryStream())
            {
                // 256 as PNG; the rest as the classic bitmap, which everything reads
                if (n >= 256) { bmp.Save(ms, ImageFormat.Png); return ms.ToArray(); }
                var w = new BinaryWriter(ms);
                int maskRow = (n + 31) / 32 * 4;
                w.Write(40); w.Write(n); w.Write(n * 2); w.Write((ushort)1); w.Write((ushort)32);
                w.Write(0); w.Write(n * n * 4 + maskRow * n); w.Write(0); w.Write(0); w.Write(0); w.Write(0);
                for (int y = n - 1; y >= 0; y--)
                    for (int x = 0; x < n; x++)
                    {
                        var c = bmp.GetPixel(x, y);
                        w.Write(c.B); w.Write(c.G); w.Write(c.R); w.Write(c.A);
                    }
                w.Write(new byte[maskRow * n]);   // the alpha says it all
                return ms.ToArray();
            }
        }
    }

    static void Main(string[] args)
    {
        int[] sizes = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };
        var images = new byte[sizes.Length][];
        for (int i = 0; i < sizes.Length; i++) images[i] = Draw(sizes[i]);
        using (var w = new BinaryWriter(File.Create(args[0])))
        {
            w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++)
            {
                w.Write((byte)(sizes[i] % 256)); w.Write((byte)(sizes[i] % 256));
                w.Write((byte)0); w.Write((byte)0);
                w.Write((ushort)1); w.Write((ushort)32);
                w.Write(images[i].Length); w.Write(offset);
                offset += images[i].Length;
            }
            foreach (var b in images) w.Write(b);
        }
    }
}
