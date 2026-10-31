// Super Mario 64 DS Enhanced builder: takes a player's own cartridge dump and
// makes an installable .cia. It carries the port (built ROM-clean, with every
// table from the ROM zeroed), a recipe of where those tables live in a dump
// (offsets and hashes), makerom and bannertool, and
// none of Nintendo's data. Everything of the game comes from the dump:
//
//   1. Extract.cs (upstream's extract_assets.ps1, in C#) pulls the NitroFS
//      files, asset catalogs and romdata.bin, hash-checking each range.
//      European ROM only.
//   2. The RomFS is laid out like port3ds/cia/make_cia.py does: build/assets,
//      extracted/dsd/header.yaml and extracted/files.pak.
//   3. The HOME Menu icon is drawn from the DS game's own icon. The banner is
//      the title-screen logo in 3D, with the Power Star flying round it as
//      in the opening, made from the dump's own files into face.template
//      (Face.cs); a flat picture of the logo if that can't be made.
//      banner.wav is the HOME Menu sound.
//   4. makerom builds the .cia with game3ds.rsf.
//
// The window just takes the dump and builds (the .cia goes next to the dump),
// with music.mp3 looping (mutable).
//
// Built by package.py with the C# 5 compiler that ships with Windows
// (Framework64\v4.0.30319\csc.exe). Everything it carries is in the
// builder-files folder beside it (Kit), in plain sight: an .exe that unpacked
// programs out of itself into %TEMP% and ran them looked like malware to
// antivirus scanners.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Super Mario 64 DS Enhanced Builder")]
[assembly: AssemblyProduct("Super Mario 64 DS Enhanced")]
[assembly: AssemblyCompany("ethanseagull")]

namespace Sm64dsEnhanced
{
    static class Program
    {
        public const string Title = "Super Mario 64 DS Enhanced";
        public const string Author = "ethanseagull";

        // No arguments: the window. --rom DUMP --out FILE.cia: the same build with
        // no window (scripts, tests), its lines in FILE.cia.log; exit code 0 or 1.
        // It takes the defaults, not the window's saved settings; --option
        // KEY=VALUE (options.txt's keys and console=new|old, which sets that
        // console's 60 fps and anti-aliasing defaults, so it goes first) changes one.
        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length > 0) return Headless(args);
            if (!Kit.Present)   // e.g. run from inside its zip
            {
                MessageBox.Show(Kit.Missing, Title + " Builder", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return 1;
            }
            Look.DpiAware();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new BuilderForm());
            return 0;
        }

        static int Headless(string[] args)
        {
            string rom = null, output = null;
            bool keep = false;
            var options = new BuildOptions();
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--keep") keep = true;   // leave the work folder (its path is logged)
                else if (args[i] == "--rom" && i + 1 < args.Length) rom = args[++i];
                else if (args[i] == "--out" && i + 1 < args.Length) output = args[++i];
                else if (args[i] == "--option" && i + 1 < args.Length && !options.Set(args[++i])) return 2;
            }
            if (rom == null || output == null) return 2;
            using (var log = new StreamWriter(output + ".log", false, new UTF8Encoding(false)))
            {
                log.AutoFlush = true;
                Action<string> line = delegate (string s) { lock (log) log.WriteLine(s); };
                line(Title + " Builder, " + Kit.Label() + ".");
                try
                {
                    var clock = Stopwatch.StartNew();
                    var build = new Build(rom, output, line, delegate (int pct, string what)
                    {
                        if (what != null)
                            line(string.Format("[{0}%] {1:0.0} s  {2}", pct, clock.Elapsed.TotalSeconds, what));
                    });
                    build.Keep = keep;
                    build.Options = options;
                    build.Run();
                    return 0;
                }
                catch (Exception e)
                {
                    line(e is BuildError ? e.Message : e.ToString());
                    return 1;
                }
            }
        }
    }

    class BuildError : Exception
    {
        public BuildError(string message) : base(message) { }
    }

    // The look: a night sky. A deep blue gradient with soft glows and star
    // dust, a few stars that slowly twinkle, a gold star beside the title, and
    // glassy cards. Segoe UI Variable (Windows 11; Segoe UI on 10). The only game
    // art is the chosen dump's DS icon.
    static class Look
    {
        public static readonly Color SkyTop = Color.FromArgb(8, 10, 28);
        public static readonly Color SkyMid = Color.FromArgb(14, 19, 49);
        public static readonly Color SkyLow = Color.FromArgb(29, 36, 86);
        public static readonly Color Text = Color.FromArgb(240, 243, 252);
        public static readonly Color Muted = Color.FromArgb(146, 156, 198);
        public static readonly Color Faint = Color.FromArgb(92, 100, 138);
        public static readonly Color Gold = Color.FromArgb(255, 206, 84);
        public static readonly Color GoldLight = Color.FromArgb(255, 228, 140);
        public static readonly Color GoldDeep = Color.FromArgb(240, 164, 42);
        public static readonly Color GoldInk = Color.FromArgb(44, 28, 4);   // text on gold
        public static readonly string Display = Installed("Segoe UI Variable Display Semib", "Segoe UI Semibold");
        public static readonly string Face = Installed("Segoe UI Variable Text", "Segoe UI");
        public static readonly string Strong = Installed("Segoe UI Variable Text Semibold", "Segoe UI Semibold");

        // white at this opacity: the cards and buttons are glass over the sky
        public static Color White(int alpha) { return Color.FromArgb(alpha, 255, 255, 255); }

        // The display's scale (1 at 100%, 3 at 300%). The window is laid out in
        // 96 dpi units and drawn at this scale, so it's sharp at any setting
        // instead of stretched by Windows.
        public static float S = 1f;
        // what a pixel-sized Font has to ask for to come out at S times its size
        // through TextRenderer (see DpiAware)
        static float fontScale = 1f;

        [DllImport("user32.dll")]
        static extern bool SetProcessDPIAware();
        [DllImport("user32.dll")]
        static extern IntPtr GetDC(IntPtr hwnd);
        [DllImport("user32.dll")]
        static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
        [DllImport("gdi32.dll")]
        static extern int GetDeviceCaps(IntPtr dc, int index);

        // The scale comes from GDI, not System.Drawing: GDI+ settles on its dpi
        // the first time it's used (the font lookup above), which can be before
        // the process is DPI aware, so it may still think 96. TextRenderer turns a
        // pixel-sized Font into points at GDI+'s dpi and back at GDI's, so the
        // font asks for its size times GDI+'s scale to come out at S times.
        public static void DpiAware()
        {
            try
            {
                SetProcessDPIAware();
                IntPtr dc = GetDC(IntPtr.Zero);
                int dpi = GetDeviceCaps(dc, 88);   // LOGPIXELSX
                ReleaseDC(IntPtr.Zero, dc);
                S = Math.Max(1f, dpi / 96f);
                using (var g = Graphics.FromHwnd(IntPtr.Zero)) fontScale = g.DpiX / 96f;
            }
            catch (Exception)
            {
                S = 1f;
                fontScale = 1f;
            }
        }

        public static int Px(float v) { return (int)Math.Round(v * S); }

        // smooth, and in layout units from here on
        public static void Layout(Graphics g)
        {
            Smooth(g);
            g.ScaleTransform(S, S);
        }

        // TextRenderer draws in device pixels whatever the transform, so the text
        // is sized and placed here (through the whole transform: a transparent
        // control's background is the form's, painted with an offset)
        public static void Write(Graphics g, string text, string face, float size, RectangleF r, Color ink,
                                 TextFormatFlags flags)
        {
            var t = g.Transform;
            var corners = new[] { r.Location, new PointF(r.Right, r.Bottom) };
            t.TransformPoints(corners);
            g.ResetTransform();
            using (var font = new Font(face, size * fontScale, GraphicsUnit.Pixel))
                TextRenderer.DrawText(g, text, font,
                                      Rectangle.FromLTRB((int)Math.Round(corners[0].X), (int)Math.Round(corners[0].Y),
                                                         (int)Math.Round(corners[1].X), (int)Math.Round(corners[1].Y)),
                                      ink, flags | TextFormatFlags.NoPadding);
            g.Transform = t;
        }

        // Display text as a shape (the title): smooth at any scale, fillable with
        // a gradient, and spaced out by tracking (layout units between letters)
        public static GraphicsPath Lettering(Graphics g, string text, string face, float size, float tracking, PointF at)
        {
            var path = new GraphicsPath();
            var fmt = StringFormat.GenericTypographic;
            using (var family = new FontFamily(face))
            using (var font = new Font(family, size, FontStyle.Regular, GraphicsUnit.Pixel))
            {
                if (tracking == 0)
                {
                    path.AddString(text, family, (int)FontStyle.Regular, size, at, fmt);
                    return path;
                }
                float x = at.X;
                foreach (char c in text)
                {
                    path.AddString(c.ToString(), family, (int)FontStyle.Regular, size, new PointF(x, at.Y), fmt);
                    x += g.MeasureString(c.ToString(), font, PointF.Empty, fmt).Width + tracking;
                }
            }
            return path;
        }

        // the first of these fonts Windows has
        static string Installed(params string[] names)
        {
            using (var fonts = new InstalledFontCollection())
                foreach (var n in names)
                    foreach (var f in fonts.Families)
                        if (f.Name == n) return n;
            return names[names.Length - 1];
        }

        public static GraphicsPath Rounded(RectangleF r, float radius)
        {
            var p = new GraphicsPath();
            float d = Math.Min(radius * 2, Math.Min(r.Width, r.Height));
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        public static void Smooth(Graphics g)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        }

        // filled, with a 1 px edge (none when edge is empty)
        public static void Box(Graphics g, RectangleF r, float radius, Color fill, Color edge)
        {
            using (var path = Rounded(r, radius))
            using (var b = new SolidBrush(fill))
            {
                g.FillPath(b, path);
                if (edge != Color.Empty)
                    using (var p = new Pen(edge, 1f)) g.DrawPath(p, path);
            }
        }

        // gold, lighter at the top, with a hairline of light along its upper edge
        public static void GoldBox(Graphics g, RectangleF r, float radius, Color top, Color bottom)
        {
            using (var path = Rounded(r, radius))
            using (var b = new LinearGradientBrush(new RectangleF(r.X, r.Y - 1, r.Width, r.Height + 2), top, bottom, 90f))
                g.FillPath(b, path);
            var state = g.Save();
            g.IntersectClip(new RectangleF(r.X, r.Y, r.Width, radius));
            using (var path = Rounded(RectangleF.Inflate(r, -0.5f, -0.5f), radius - 0.5f))
            using (var p = new Pen(Color.FromArgb(120, 255, 250, 228), 1f))
                g.DrawPath(p, path);
            g.Restore(state);
        }

        // A soft round glow: color at the middle, fading out to nothing at the edge
        public static void Glow(Graphics g, PointF at, float rx, float ry, Color color)
        {
            using (var path = new GraphicsPath())
            {
                path.AddEllipse(at.X - rx, at.Y - ry, rx * 2, ry * 2);
                using (var b = new PathGradientBrush(path))
                {
                    b.CenterPoint = at;
                    b.CenterColor = color;
                    b.SurroundColors = new[] { Color.FromArgb(0, color) };
                    b.Blend = Falloff;
                    g.FillPath(b, path);
                }
            }
        }

        // eased, so a glow has no visible edge or cone in the middle
        static readonly Blend Falloff = new Blend
        {
            Positions = new[] { 0f, 0.2f, 0.4f, 0.6f, 0.8f, 1f },
            Factors = new[] { 0f, 0.07f, 0.22f, 0.46f, 0.76f, 1f },
        };

        // The sky under everything: the gradient, light low on the right like a
        // horizon after dusk, a little violet in the top corner, and dust: many
        // faint stars that stay where they are (the same every time), none
        // behind the glass and text in clear.
        public static void Night(Graphics g, float w, float h, Rectangle[] clear)
        {
            using (var b = new LinearGradientBrush(new RectangleF(0, -1, w, h + 2), SkyTop, SkyLow, 90f))
            {
                b.InterpolationColors = new ColorBlend
                {
                    Colors = new[] { SkyTop, SkyMid, SkyLow },
                    Positions = new[] { 0f, 0.55f, 1f },
                };
                g.FillRectangle(b, 0, 0, w, h);
            }
            Glow(g, new PointF(w * 0.8f, h * 1.04f), w * 0.7f, h * 0.78f, Color.FromArgb(78, 74, 96, 196));
            Glow(g, new PointF(w * 0.02f, -h * 0.12f), w * 0.5f, h * 0.62f, Color.FromArgb(40, 120, 84, 200));

            var dust = new Random(64);
            int count = (int)(140 * w * h / (540f * 300f));   // as thick as the main window's
            for (int i = 0; i < count; i++)
            {
                var p = new PointF((float)dust.NextDouble() * w, (float)dust.NextDouble() * h);
                double size = dust.NextDouble(), shine = dust.NextDouble();
                bool warm = dust.NextDouble() < 0.15;
                var at = Point.Round(p);
                bool hidden = false;
                foreach (var c in clear) if (c.Contains(at)) hidden = true;
                if (hidden) continue;
                float r = 0.35f + (float)(size * size * size) * 0.75f;
                int a = 30 + (int)(shine * shine * 140);
                using (var b = new SolidBrush(warm ? Color.FromArgb(a, 255, 226, 170) : Color.FromArgb(a, 214, 224, 255)))
                    g.FillEllipse(b, p.X - r, p.Y - r, r * 2, r * 2);
            }
        }

        // the gold star by a title, glowing
        public static void GoldStar(Graphics g, PointF at, float outer)
        {
            float k = outer / 20f;
            Glow(g, at, 46 * k, 46 * k, Color.FromArgb(70, Gold));
            Glow(g, at, 26 * k, 26 * k, Color.FromArgb(60, GoldLight));
            using (var b = new LinearGradientBrush(new RectangleF(at.X - outer - 1, at.Y - outer - 1, outer * 2 + 2, outer * 2),
                                                   Color.FromArgb(255, 240, 176), GoldDeep, 90f))
                Star(g, new PointF(at.X, at.Y + k), outer, b);
        }

        // A five-pointed star, its points slightly rounded
        public static void Star(Graphics g, PointF at, float outer, Brush fill)
        {
            var pts = new PointF[10];
            for (int i = 0; i < 10; i++)
            {
                double ang = -Math.PI / 2 + i * Math.PI / 5;
                float rad = i % 2 == 0 ? outer : outer * 0.48f;
                pts[i] = new PointF(at.X + rad * (float)Math.Cos(ang), at.Y + rad * (float)Math.Sin(ang));
            }
            using (var p = new Pen(fill, outer * 0.16f))
            {
                p.LineJoin = LineJoin.Round;
                g.FillPolygon(fill, pts);
                g.DrawPolygon(p, pts);
            }
        }

        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

        // Windows 10/11's dark title bar for this window (DWMWA_USE_IMMERSIVE_DARK_MODE),
        // and on 11 the title bar in the sky's colour (DWMWA_CAPTION_COLOR, DWMWA_BORDER_COLOR)
        public static void DarkTitleBar(IntPtr hwnd)
        {
            try
            {
                int on = 1;
                if (DwmSetWindowAttribute(hwnd, 20, ref on, 4) != 0) DwmSetWindowAttribute(hwnd, 19, ref on, 4);
                int sky = SkyTop.B << 16 | SkyTop.G << 8 | SkyTop.R;
                DwmSetWindowAttribute(hwnd, 35, ref sky, 4);
                DwmSetWindowAttribute(hwnd, 34, ref sky, 4);
            }
            catch (Exception) { }
        }
    }

    // Background music: music.mp3 looped through Windows' MCI (winmm) while the
    // window is open. Mute is remembered in %LOCALAPPDATA%\SM64DSEnhancedBuilder\muted.
    static class Music
    {
        [DllImport("winmm.dll", CharSet = CharSet.Unicode)]
        static extern int mciSendString(string command, StringBuilder ret, int retLength, IntPtr callback);

        static bool playing;

        static string MutedFlag
        {
            get
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                    "SM64DSEnhancedBuilder", "muted");
            }
        }

        public static bool Muted
        {
            get { try { return File.Exists(MutedFlag); } catch (Exception) { return false; } }
            set
            {
                try
                {
                    if (value)
                    {
                        Directory.CreateDirectory(Path.GetDirectoryName(MutedFlag));
                        File.WriteAllText(MutedFlag, "");
                    }
                    else if (File.Exists(MutedFlag)) File.Delete(MutedFlag);
                }
                catch (Exception) { }
                if (playing) mciSendString("setaudio bgm " + (value ? "off" : "on"), null, 0, IntPtr.Zero);
            }
        }

        public static void Start()
        {
            try
            {
                var file = Kit.Path("music.mp3");
                if (!File.Exists(file)) return;
                if (mciSendString("open \"" + file + "\" type mpegvideo alias bgm", null, 0, IntPtr.Zero) != 0) return;
                playing = true;
                mciSendString("setaudio bgm volume to 1000", null, 0, IntPtr.Zero);   // MCI's loudest
                if (Muted) mciSendString("setaudio bgm off", null, 0, IntPtr.Zero);
                mciSendString("play bgm repeat", null, 0, IntPtr.Zero);
            }
            catch (Exception) { }
        }

        public static void Stop()
        {
            if (playing) mciSendString("close bgm", null, 0, IntPtr.Zero);
            playing = false;
        }

        // The finished build's sound (done.mp3), once, in place of the music.
        // Nothing when muted.
        public static void Done()
        {
            Stop();
            if (Muted) return;
            try
            {
                var doneFile = Kit.Path("done.mp3");
                if (!File.Exists(doneFile)) return;
                if (mciSendString("open \"" + doneFile + "\" type mpegvideo alias done", null, 0, IntPtr.Zero) != 0) return;
                mciSendString("setaudio done volume to 580", null, 0, IntPtr.Zero);
                mciSendString("play done", null, 0, IntPtr.Zero);
            }
            catch (Exception) { }
        }

        public static void Close()
        {
            Stop();
            mciSendString("close done", null, 0, IntPtr.Zero);
        }
    }

    // A button: Primary (gold) or quiet (glass). Sliders: an icon of three
    // sliders in place of the text (the build settings; Text is its name).
    class FlatButton : Control
    {
        public bool Primary, Sliders;
        bool hover, down;

        public FlatButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.SupportsTransparentBackColor | ControlStyles.Selectable, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
            TabStop = true;
            Font = new Font(Look.Face, 13f, GraphicsUnit.Pixel);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            Look.Layout(g);
            var r = new RectangleF(0.5f, 0.5f, Width / Look.S - 1, Height / Look.S - 1);
            Color ink;
            if (!Enabled)
            {
                // still the gold button, dimmed
                if (Primary) Look.Box(g, r, 10, Color.FromArgb(30, Look.Gold), Color.FromArgb(64, Look.Gold));
                else Look.Box(g, r, 10, Look.White(8), Look.White(20));
                ink = Primary ? Color.FromArgb(150, 128, 82) : Look.Faint;
            }
            else if (Primary)
            {
                if (down) Look.GoldBox(g, r, 10, Look.Gold, Look.GoldDeep);
                else if (hover) Look.GoldBox(g, r, 10, Color.FromArgb(255, 236, 166), Color.FromArgb(250, 186, 66));
                else Look.GoldBox(g, r, 10, Look.GoldLight, Look.GoldDeep);
                ink = Look.GoldInk;
            }
            else
            {
                Look.Box(g, r, 10, Look.White(down ? 8 : hover ? 20 : 12), Look.White(hover ? 64 : 36));
                ink = Look.Text;
            }
            if (Sliders) DrawSliders(g, new PointF(r.X + r.Width / 2, r.Y + r.Height / 2), hover && Enabled ? Look.Gold : ink);
            else Look.Write(g, Text, Look.Strong, 14f, r, ink, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (Focused && ShowFocusCues)
                using (var p = new Pen(Primary && Enabled ? Color.FromArgb(150, Look.GoldInk) : Look.White(150), 1.2f))
                using (var inner = Look.Rounded(RectangleF.Inflate(r, -3, -3), 7))
                    g.DrawPath(p, inner);
        }

        // three rails, each with its knob at a different place
        static void DrawSliders(Graphics g, PointF c, Color ink)
        {
            float half = 9f;
            var knobs = new[] { 0.3f, 0.72f, 0.46f };
            using (var rail = new Pen(Color.FromArgb(170, ink), 1.5f))
            using (var b = new SolidBrush(ink))
            {
                rail.StartCap = rail.EndCap = LineCap.Round;
                for (int i = 0; i < 3; i++)
                {
                    float y = c.Y + (i - 1) * 6f, x = c.X - half + knobs[i] * half * 2;
                    g.DrawLine(rail, c.X - half, y, c.X + half, y);
                    g.FillEllipse(b, x - 2.6f, y - 2.6f, 5.2f, 5.2f);
                }
            }
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { down = true; Focus(); Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyUp(e);
        }
    }

    // The music's on/off: a speaker, struck through when muted.
    class MuteButton : Control
    {
        bool hover;

        public MuteButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            Look.Layout(g);
            float w = Width / Look.S, h = Height / Look.S;
            if (hover) Look.Box(g, new RectangleF(0.5f, 0.5f, w - 1, h - 1), (w - 1) / 2, Look.White(16), Color.Empty);
            float x = w / 2f - 8.5f, y = h / 2f;
            var ink = hover ? Look.Text : Look.Muted;
            using (var b = new SolidBrush(ink))
            using (var p = new Pen(ink, 1.5f))
            {
                p.StartCap = p.EndCap = LineCap.Round;
                p.LineJoin = LineJoin.Round;
                var speaker = new[] { new PointF(x, y - 3), new PointF(x + 3.5f, y - 3), new PointF(x + 8, y - 7),
                                      new PointF(x + 8, y + 7), new PointF(x + 3.5f, y + 3), new PointF(x, y + 3) };
                g.FillPolygon(b, speaker);
                g.DrawPolygon(p, speaker);
                if (Music.Muted)
                {
                    g.DrawLine(p, x + 12, y - 3.5f, x + 19, y + 3.5f);
                    g.DrawLine(p, x + 19, y - 3.5f, x + 12, y + 3.5f);
                }
                else
                {
                    g.DrawArc(p, x + 6, y - 4.5f, 8, 9, -50, 100);
                    g.DrawArc(p, x + 5, y - 8.5f, 14, 17, -50, 100);
                }
            }
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }

        protected override void OnClick(EventArgs e)
        {
            Music.Muted = !Music.Muted;
            Invalidate();
            base.OnClick(e);
        }
    }

    // The dump tile: click it or drop a file on it. Empty, it shows a game card
    // and what to choose; once chosen, the dump's DS icon, its file name and a
    // line of status, with a progress bar under it while building.
    class RomTile : Control
    {
        public string Path;
        public Bitmap Icon;
        public string Status;       // the line under the file name (null: Ready)
        public string Ready = "Ready. The .cia will be saved next to it.";
        public bool Saved;          // Status says where the .cia went
        public int Progress = -1;   // 0..100 while building, else -1
        bool hover, drag;

        public RomTile()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.SupportsTransparentBackColor | ControlStyles.Selectable, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
            TabStop = true;
            AllowDrop = true;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            Look.Layout(g);
            float w = Width / Look.S, h = Height / Look.S;
            var r = new RectangleF(0.5f, 0.5f, w - 1, h - 1);
            bool lit = hover && Enabled;
            if (drag)
            {
                Look.Box(g, r, 14, Color.FromArgb(22, Look.Gold), Color.Empty);
                using (var path = Look.Rounded(RectangleF.Inflate(r, -0.25f, -0.25f), 14))
                using (var p = new Pen(Color.FromArgb(220, Look.Gold), 1.5f)) g.DrawPath(p, path);
            }
            else if (Path == null)
            {
                // empty: a dashed outline over a faint glass
                Look.Box(g, r, 14, Look.White(lit ? 12 : 6), Color.Empty);
                using (var path = Look.Rounded(r, 14))
                using (var p = new Pen(Focused && ShowFocusCues ? Color.FromArgb(200, Look.Gold) : Look.White(lit ? 96 : 54), 1.2f))
                {
                    p.DashPattern = new[] { 4f, 3.5f };
                    g.DrawPath(p, path);
                }
            }
            else
            {
                Look.Box(g, r, 14, Look.White(lit ? 14 : 9),
                         Focused && ShowFocusCues ? Color.FromArgb(200, Look.Gold) : Look.White(lit ? 60 : 34));
            }

            // the icon's square: the DS icon at twice its size, or a game card
            var slot = new RectangleF(16, (h - 64) / 2f, 64, 64);
            if (Icon != null)
            {
                var mode = g.InterpolationMode;
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                using (var clip = Look.Rounded(slot, 10))
                {
                    var state = g.Save();
                    g.SetClip(clip, CombineMode.Intersect);
                    using (var b = new SolidBrush(Look.White(235))) g.FillRectangle(b, slot);
                    g.DrawImage(Icon, slot.X, slot.Y, 64, 64);
                    g.Restore(state);
                }
                g.InterpolationMode = mode;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            }
            else
            {
                Look.Box(g, slot, 10, Look.White(lit || drag ? 14 : 8), Look.White(lit || drag ? 44 : 26));
                Card(g, new PointF(slot.X + slot.Width / 2, slot.Y + slot.Height / 2),
                     lit || drag ? Look.Gold : Look.Muted);
            }

            float left = slot.Right + 18, right = w - 22;
            string title, line;
            Color lineInk = Look.Muted;
            if (drag)
            {
                title = "Drop to choose this dump";
                line = "Super Mario 64 DS, European cartridge (.nds)";
            }
            else if (Path == null)
            {
                title = "Choose your Super Mario 64 DS dump";
                line = "The European cartridge (.nds). Click here, or drop the file.";
            }
            else
            {
                title = System.IO.Path.GetFileName(Path);
                line = Status ?? Ready;
                if (Saved) lineInk = Look.Gold;
            }
            Look.Write(g, title, Look.Strong, 15f, new RectangleF(left, h / 2 - 24, right - left, 22), Look.Text,
                       TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.SingleLine);
            float lineRight = right;
            if (Progress >= 0)
            {
                lineRight -= 40;
                Look.Write(g, Progress + "%", Look.Strong, 13f, new RectangleF(lineRight, h / 2 - 1, 40, 18), Look.Text,
                           TextFormatFlags.VerticalCenter | TextFormatFlags.Right | TextFormatFlags.SingleLine);
            }
            Look.Write(g, line, Look.Face, 13f, new RectangleF(left, h / 2 - 1, lineRight - left, 18), lineInk,
                       TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis | TextFormatFlags.SingleLine);
            if (Progress >= 0)
            {
                var track = new RectangleF(left, h / 2 + 23, right - left, 4);
                Look.Box(g, track, 2, Look.White(20), Color.Empty);
                if (Progress > 0)
                {
                    var fill = new RectangleF(track.X, track.Y, Math.Max(4, track.Width * Progress / 100f), track.Height);
                    using (var path = Look.Rounded(fill, 2))
                    using (var b = new LinearGradientBrush(new RectangleF(track.X - 1, track.Y, track.Width + 2, 4),
                                                           Look.GoldDeep, Look.GoldLight, 0f))
                        g.FillPath(b, path);
                }
            }
        }

        // a DS game card in outline: the cut corner, the ridges along the top
        // and the label
        static void Card(Graphics g, PointF c, Color ink)
        {
            float w = 24, h = 28, x = c.X - w / 2, y = c.Y - h / 2;
            using (var p = new Pen(ink, 1.5f))
            {
                p.LineJoin = LineJoin.Round;
                p.StartCap = p.EndCap = LineCap.Round;
                g.DrawPolygon(p, new[] { new PointF(x + 2, y), new PointF(x + w - 6, y), new PointF(x + w, y + 6),
                                         new PointF(x + w, y + h - 2), new PointF(x + w - 2, y + h),
                                         new PointF(x + 2, y + h), new PointF(x, y + h - 2), new PointF(x, y + 2) });
                for (int i = 0; i < 4; i++) g.DrawLine(p, x + 5 + i * 3.5f, y + 3.5f, x + 5 + i * 3.5f, y + 6.5f);
                using (var label = Look.Rounded(new RectangleF(x + 4.5f, y + 11, w - 9, h - 15.5f), 2))
                    g.DrawPath(p, label);
            }
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }
        protected override void OnEnabledChanged(EventArgs e) { Cursor = Enabled ? Cursors.Hand : Cursors.Default; Invalidate(); base.OnEnabledChanged(e); }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyUp(e);
        }

        static string Dropped(DragEventArgs e)
        {
            var files = e.Data.GetDataPresent(DataFormats.FileDrop) ? e.Data.GetData(DataFormats.FileDrop) as string[] : null;
            return files != null && files.Length == 1 ? files[0] : null;
        }

        public Action<string> FileDropped;

        protected override void OnDragEnter(DragEventArgs e)
        {
            if (Enabled && Dropped(e) != null) { e.Effect = DragDropEffects.Copy; drag = true; Invalidate(); }
            base.OnDragEnter(e);
        }

        protected override void OnDragLeave(EventArgs e) { drag = false; Invalidate(); base.OnDragLeave(e); }

        protected override void OnDragDrop(DragEventArgs e)
        {
            drag = false;
            Invalidate();
            var f = Dropped(e);
            if (f != null && FileDropped != null) FileDropped(f);
            base.OnDragDrop(e);
        }
    }

    // The window: the star and title, music button, dump tile, Build. The .cia
    // is saved next to the dump; the build's log lines are only kept for error
    // messages. When a build finishes the music stops and done.mp3 plays.
    class BuilderForm : Form
    {
        RomTile tile;
        FlatButton buildButton, showButton, settingsButton;
        string built;
        readonly BuildOptions options = BuildOptions.Load();
        readonly string version;
        readonly Bitmap sky;
        readonly StringBuilder lines = new StringBuilder();
        readonly System.Windows.Forms.Timer clock = new System.Windows.Forms.Timer();
        readonly Random rng = new Random();

        const int Pad = 32, W = 540, H = 300;
        static readonly PointF Mark = new PointF(Pad + 20, 52);   // the gold star by the title

        // A star glows and fades over its cycle, then shows up somewhere else.
        // The brighter ones have four rays.
        class Star { public float X, Y, R, Phase, Speed; public bool Rays; public Color Ink; }
        readonly Star[] stars = new Star[12];
        // the tile, the buttons' corner (the settings, and Build or Show in folder
        // once built) and the version: no dust behind their glass or text
        static readonly Rectangle TileBox = new Rectangle(Pad, 108, W - Pad * 2, 96);
        static readonly Rectangle ButtonBox = new Rectangle(W - Pad - 206, 226, 208, 48);
        static readonly Rectangle VersionBox = new Rectangle(Pad - 2, 240, 110, 20);
        // where no twinkling star goes: the star and title, the music button, the
        // tile, the button and the version
        static readonly Rectangle[] Taken = {
            new Rectangle(14, 14, 300, 76), new Rectangle(476, 32, 36, 36), TileBox, ButtonBox, VersionBox };

        public BuilderForm()
        {
            Text = Program.Title;
            Font = new Font(Look.Face, 13f, GraphicsUnit.Pixel);
            ForeColor = Look.Text;
            BackColor = Look.SkyMid;
            ClientSize = new Size(Look.Px(W), Look.Px(H));
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            DoubleBuffered = true;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
            var label = Kit.Label();
            version = label.StartsWith("build ") && label != "build unknown" ? "Version " + label.Substring(6) : "";
            sky = Sky();

            var mute = new MuteButton();
            mute.SetBounds(Look.Px(W - Pad - 28 + 4), Look.Px(36), Look.Px(32), Look.Px(32));

            tile = new RomTile();
            tile.SetBounds(Look.Px(TileBox.X), Look.Px(TileBox.Y), Look.Px(TileBox.Width), Look.Px(TileBox.Height));
            tile.Click += delegate { PickRom(); };
            tile.FileDropped = SetRom;

            buildButton = Button("Build", true, W - Pad - 132, 228, 132);
            buildButton.Enabled = false;
            buildButton.Click += delegate { StartBuild(); };
            showButton = Button("Show in folder", true, W - Pad - 150, 228, 150);
            showButton.Visible = false;
            showButton.Click += delegate
            {
                if (built != null) Process.Start("explorer.exe", "/select,\"" + built + "\"");
            };
            settingsButton = Button("Build settings", false, 0, 228, 44);
            settingsButton.Sliders = true;
            settingsButton.AccessibleName = "Build settings";
            settingsButton.Click += delegate { OpenSettings(); };
            PlaceSettings();
            tile.Ready = ReadyLine();

            Controls.AddRange(new Control[] { mute, tile, settingsButton, buildButton, showButton });
            HandleCreated += delegate { Look.DarkTitleBar(Handle); };
            Shown += delegate { Music.Start(); mute.Invalidate(); };
            FormClosed += delegate { Music.Close(); sky.Dispose(); };

            for (int i = 0; i < stars.Length; i++)
            {
                stars[i] = new Star();
                Place(stars[i]);
                stars[i].Phase = (float)(rng.NextDouble() * Math.PI * 2);   // not all in step
            }
            clock.Interval = 50;
            clock.Tick += delegate { Twinkle(); };
            clock.Start();
        }

        // Everything that doesn't move, drawn once at the window's real size: the
        // sky's gradient and glows, the star dust, the gold star and the title.
        Bitmap Sky()
        {
            var bmp = new Bitmap(Look.Px(W), Look.Px(H), PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                Look.Layout(g);
                Look.Night(g, W, H, new[] { TileBox, ButtonBox, VersionBox });
                Look.GoldStar(g, Mark, 20);

                // the title
                using (var t = Look.Lettering(g, "Super Mario 64 DS", Look.Display, 28f, 0, new PointF(Pad + 52, 26)))
                {
                    var box = t.GetBounds();
                    using (var b = new LinearGradientBrush(new RectangleF(box.X, box.Y - 1, box.Width, box.Height + 2),
                                                           Color.White, Color.FromArgb(200, 210, 240), 90f))
                        g.FillPath(b, t);
                }
                using (var t = Look.Lettering(g, "ENHANCED", Look.Display, 11.5f, 3.2f, new PointF(Pad + 53.5f, 62)))
                using (var b = new SolidBrush(Look.Gold))
                    g.FillPath(b, t);
            }
            return bmp;
        }

        void Place(Star s)
        {
            s.R = 0.9f + (float)rng.NextDouble() * 0.9f;
            s.Rays = rng.NextDouble() < 0.35;
            if (s.Rays) s.R += 0.3f;
            s.Ink = rng.NextDouble() < 0.3 ? Color.FromArgb(255, 222, 140) : Color.FromArgb(226, 234, 255);
            s.Speed = 0.05f + (float)rng.NextDouble() * 0.025f;   // a cycle of 4 to 6 s
            float reach = Reach(s);
            for (int tries = 0; tries < 200; tries++)
            {
                var p = new PointF(rng.Next(10, W - 10), rng.Next(10, H - 10));
                var box = RectangleF.FromLTRB(p.X - reach, p.Y - reach, p.X + reach, p.Y + reach);
                bool ok = true;
                foreach (var r in Taken) if (box.IntersectsWith(r)) ok = false;
                foreach (var o in stars)
                    if (o != null && o != s && Math.Abs(o.X - p.X) + Math.Abs(o.Y - p.Y) < 56) ok = false;
                if (!ok) continue;
                s.X = p.X;
                s.Y = p.Y;
                break;
            }
        }

        // how far a star's glow and rays go from its middle
        static float Reach(Star s) { return s.R * (s.Rays ? 7f : 4.5f) + 1; }

        void Twinkle()
        {
            foreach (var s in stars)
            {
                Invalidate(StarBox(s));
                s.Phase += s.Speed;
                if (s.Phase < Math.PI * 2) continue;
                s.Phase -= (float)(Math.PI * 2);   // dark here: move it
                Place(s);
                Invalidate(StarBox(s));
            }
        }

        static Rectangle StarBox(Star s)
        {
            float reach = Reach(s) + 1;
            return Rectangle.FromLTRB((int)Math.Floor((s.X - reach) * Look.S), (int)Math.Floor((s.Y - reach) * Look.S),
                                      (int)Math.Ceiling((s.X + reach) * Look.S), (int)Math.Ceiling((s.Y + reach) * Look.S));
        }

        static void DrawStar(Graphics g, Star s)
        {
            float a = (float)(1 - Math.Cos(s.Phase)) / 2;
            if (a < 0.02f) return;
            Look.Glow(g, new PointF(s.X, s.Y), s.R * 4.5f, s.R * 4.5f, Color.FromArgb((int)(a * 110), s.Ink));
            if (s.Rays)
            {
                // four thin rays, tapering to nothing
                float len = s.R * 7f * (0.6f + 0.4f * a), half = s.R * 0.32f;
                using (var b = new SolidBrush(Color.FromArgb((int)(a * 170), s.Ink)))
                {
                    g.FillPolygon(b, new[] { new PointF(s.X - len, s.Y), new PointF(s.X, s.Y - half),
                                             new PointF(s.X + len, s.Y), new PointF(s.X, s.Y + half) });
                    g.FillPolygon(b, new[] { new PointF(s.X, s.Y - len), new PointF(s.X + half, s.Y),
                                             new PointF(s.X, s.Y + len), new PointF(s.X - half, s.Y) });
                }
            }
            float core = s.R * (0.75f + 0.25f * a);
            using (var b = new SolidBrush(Color.FromArgb((int)(a * 255), Color.White)))
                g.FillEllipse(b, s.X - core * 0.6f, s.Y - core * 0.6f, core * 1.2f, core * 1.2f);
            using (var b = new SolidBrush(Color.FromArgb((int)(a * 200), s.Ink)))
                g.FillEllipse(b, s.X - core, s.Y - core, core * 2, core * 2);
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            var g = e.Graphics;
            var c = e.ClipRectangle;
            g.CompositingMode = CompositingMode.SourceCopy;
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.DrawImage(sky, c, c, GraphicsUnit.Pixel);
            g.CompositingMode = CompositingMode.SourceOver;
            Look.Layout(g);
            foreach (var s in stars) DrawStar(g, s);
            if (version.Length > 0)
                Look.Write(g, version, Look.Face, 12f, new RectangleF(Pad, 240, 160, 20), Look.Faint,
                           TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
        }

        static FlatButton Button(string text, bool primary, int x, int y, int w)
        {
            var b = new FlatButton();
            b.Text = text;
            b.Primary = primary;
            b.SetBounds(Look.Px(x), Look.Px(y), Look.Px(w), Look.Px(44));
            return b;
        }

        void SetRom(string path)
        {
            Bitmap icon = null;
            try { if (File.Exists(path)) icon = Art.DsIcon(path); } catch (Exception) { }
            tile.Path = File.Exists(path) ? path : null;
            tile.Icon = icon;
            tile.Progress = -1;
            tile.Status = null;
            tile.Saved = false;
            tile.Invalidate();
            showButton.Visible = false;
            buildButton.Visible = true;
            buildButton.Text = "Build";
            buildButton.Enabled = tile.Path != null;
            PlaceSettings();
        }

        // the settings button, just left of Build or Show in folder
        void PlaceSettings()
        {
            var beside = showButton.Visible ? showButton : buildButton;
            settingsButton.Left = beside.Left - Look.Px(54);
        }

        string ReadyLine()
        {
            return "Ready for " + (options.Old ? "an Old 3DS" : "a New 3DS") + ". The .cia will be saved next to it.";
        }

        // The settings dialog; a change after a build brings Build back, so the
        // new choices can be built.
        void OpenSettings()
        {
            using (var d = new SettingsForm(options))
            {
                if (d.ShowDialog(this) != DialogResult.OK || d.Chosen.Same(options)) return;
                options.Take(d.Chosen);
                options.Save();
            }
            tile.Ready = ReadyLine();
            if (built != null && tile.Path != null) SetRom(tile.Path);
            tile.Invalidate();
        }

        void PickRom()
        {
            using (var d = new OpenFileDialog())
            {
                d.Title = "Your Super Mario 64 DS dump";
                d.Filter = "Nintendo DS ROM (*.nds)|*.nds|All files (*.*)|*.*";
                if (d.ShowDialog(this) == DialogResult.OK) SetRom(d.FileName);
            }
        }

        void StartBuild()
        {
            var rom = tile.Path;
            if (rom == null || !File.Exists(rom)) return;
            var output = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(rom)),
                                                Program.Title + ".cia");
            SetBusy(true);
            lines.Length = 0;
            var chosen = options.Copy();
            var worker = new Thread(delegate () { RunBuild(rom, output, chosen); });
            worker.IsBackground = true;
            worker.Start();
        }

        void RunBuild(string rom, string output, BuildOptions chosen)
        {
            string error = null;
            try
            {
                var build = new Build(rom, output, Log, Progress);
                build.Options = chosen;
                build.Run();
            }
            catch (BuildError e)
            {
                error = e.Message;
            }
            catch (Exception e)
            {
                error = "Something unexpected went wrong.\r\n\r\n" + e.Message;
            }
            BeginInvoke((MethodInvoker)delegate
            {
                SetBusy(false);
                tile.Progress = -1;
                if (error == null)
                {
                    built = output;
                    tile.Status = "Saved " + System.IO.Path.GetFileName(output) + " next to it.";
                    tile.Saved = true;
                    // Show in folder takes Build's place until another dump (or
                    // another setting) is chosen
                    buildButton.Visible = false;
                    showButton.Visible = true;
                    PlaceSettings();
                    Music.Done();
                }
                else
                {
                    tile.Status = "The build didn't finish. Press Build to try again.";
                }
                tile.Invalidate();
                if (error != null) MessageBox.Show(this, error, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            });
        }

        void SetBusy(bool busy)
        {
            tile.Enabled = buildButton.Enabled = settingsButton.Enabled = !busy;
            UseWaitCursor = busy;
            if (busy)
            {
                showButton.Visible = false;
                buildButton.Text = "Building…";
                tile.Progress = 0;
                tile.Status = "Starting…";
                tile.Saved = false;
                tile.Invalidate();
            }
            else
            {
                buildButton.Text = "Build";
            }
        }

        void Log(string line)
        {
            lock (lines) lines.AppendLine(line);
        }

        void Progress(int percent, string what)
        {
            if (InvokeRequired)
            {
                BeginInvoke((MethodInvoker)delegate { Progress(percent, what); });
                return;
            }
            tile.Progress = Math.Max(0, Math.Min(100, percent));
            if (what != null) tile.Status = what.TrimEnd('.');
            tile.Invalidate();
        }
    }

    // The builder's files (package.py): the builder-files folder beside the .exe,
    // as the zip it comes in has them.
    static class Kit
    {
        public const string Folder = "builder-files";
        public static readonly string Dir =
            System.IO.Path.Combine(System.IO.Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location), Folder);

        public const string Missing = "The builder's files aren't next to it. Extract the whole zip first " +
                                      "(right-click it, Extract All), then run SM64DS-Enhanced-Builder.exe from " +
                                      "the folder that makes, with its " + Folder + " folder beside it.";

        public static bool Present { get { return File.Exists(Path("LABEL.txt")); } }

        public static string Path(string name) { return System.IO.Path.Combine(Dir, name); }

        // a file the build can't do without
        public static string Need(string name)
        {
            var p = Path(name);
            if (!File.Exists(p))
                throw new BuildError(Present ? "The builder's " + name + " is missing from its " + Folder +
                                               " folder. Download the builder again and extract the whole zip."
                                             : Missing);
            return p;
        }

        public static string Label()
        {
            try { return "build " + File.ReadAllText(Path("LABEL.txt")).Trim(); }
            catch (Exception) { return "build unknown"; }
        }
    }

    class Build
    {
        readonly string rom, output, tmp;
        readonly Action<string> log;
        readonly Action<int, string> progress;
        public bool Keep;
        public BuildOptions Options = new BuildOptions();

        public Build(string rom, string output, Action<string> log, Action<int, string> progress)
        {
            this.rom = Path.GetFullPath(rom);
            this.output = Path.GetFullPath(output);
            this.log = log;
            this.progress = progress;
            tmp = Path.Combine(Path.GetTempPath(), "SM64DSEnhancedBuilder-" + Guid.NewGuid().ToString("N"));
        }

        public void Run()
        {
            if (!Kit.Present) throw new BuildError(Kit.Missing);
            byte[] header = ReadHeader();
            try
            {
                Directory.CreateDirectory(tmp);
                progress(1, "Starting...");

                // 1. the game data, out of the dump (upstream's extractor)
                var work = Path.Combine(tmp, "work");
                Directory.CreateDirectory(work);
                log("Extracting the game data from your dump...");
                Sm64dsEnhanced.Extract.Run(rom, Kit.Need("romdata.recipe.tsv"), Kit.Need("romdata.manifest"), work,
                                           delegate (string line) { log("  " + line); },
                                           delegate (int pct, string what) { progress(2 + pct * 68 / 100, what); });

                // 2. the RomFS
                progress(72, "Packing the game files...");
                var romfs = Path.Combine(tmp, "romfs");
                var assets = Path.Combine(romfs, "build", "assets");
                Directory.CreateDirectory(assets);
                foreach (var name in new[] { "files.tsv", "handles.tsv", "nitrofs.tsv", "nitrofs_fat.bin",
                                             "nitrofs_fnt.bin", "nitrofs_ovt9.bin", "romdata.bin",
                                             "romdata.manifest" })
                {
                    var from = Path.Combine(work, "build", "assets", name);
                    if (!File.Exists(from))
                        throw new BuildError("The extraction did not produce " + name + ". Try again; if it " +
                                             "happens again, the dump may be damaged.");
                    File.Copy(from, Path.Combine(assets, name));
                }
                var dsd = Path.Combine(romfs, "extracted", "dsd");
                Directory.CreateDirectory(dsd);
                // hal/nitrofs_boot.cpp reads normal_cmd_setting (header +0x60) out of this file
                File.WriteAllText(Path.Combine(dsd, "header.yaml"),
                    "title: " + Encoding.ASCII.GetString(header, 0, 12).TrimEnd('\0') + "\n" +
                    "gamecode: " + Encoding.ASCII.GetString(header, 12, 4) + "\n" +
                    "normal_cmd_setting: " + BitConverter.ToUInt32(header, 0x60) + "\n",
                    new UTF8Encoding(false));
                int packed = Pack.Write(Path.Combine(work, "extracted", "dsd", "files"),
                                        Path.Combine(romfs, "extracted", "files.pak"));
                log("Packed " + packed + " game files.");
                // the settings: the game's options.txt, read at boot until the
                // player saves their own on the SD card (options3ds.cpp)
                File.WriteAllText(Path.Combine(romfs, "options.txt"), Options.FileText(), new UTF8Encoding(false));
                log("Settings: " + Options.Summary() + ".");

                // 3. the icon from the DS game's own icon; the banner the title
                // logo with the opening's star round it (Face.cs), or, failing
                // that, a flat picture of the logo
                progress(80, "Drawing the HOME Menu icon and banner...");
                var art = Path.Combine(tmp, "art");
                Directory.CreateDirectory(art);
                Bitmap dsIcon = Art.DsIcon(rom);
                Art.SaveIcon(dsIcon, Path.Combine(art, "icon.png"));
                string[] bannerArt = null;
                Bitmap logo = Art.TitleLogo(rom);
                if (logo == null) log("Couldn't read the title logo.");
                var template = Kit.Path("face.template");
                if (File.Exists(template))
                {
                    var dsmt = Path.Combine(work, "extracted", "dsd", "files", "data", "DSMT");
                    var model = Face.Banner(dsmt, File.ReadAllBytes(template),
                                            File.ReadAllLines(Kit.Need("face.recipe")), logo, log);
                    if (model != null)
                    {
                        File.WriteAllBytes(Path.Combine(art, "banner.cgfx"), model);
                        bannerArt = new[] { "-ci", Path.Combine(art, "banner.cgfx") };
                        log("Made the 3D banner (" + model.Length / 1024 + " KB).");
                    }
                }
                if (bannerArt == null)
                {
                    Art.SaveBanner(logo, dsIcon, Path.Combine(art, "banner.png"));
                    bannerArt = new[] { "-i", Path.Combine(art, "banner.png") };
                }
                // the HOME Menu sound (builder-files/banner.wav)
                var sound = Kit.Path("banner.wav");
                if (!File.Exists(sound))
                {
                    sound = Path.Combine(art, "banner.wav");
                    Art.SaveSilence(sound);
                }
                var bannertool = Kit.Need("bannertool.exe");
                Tool(bannertool, new[] { "makesmdh", "-s", Program.Title, "-l", Program.Title,
                                         "-p", Program.Author, "-i", Path.Combine(art, "icon.png"),
                                         "-o", Path.Combine(art, "icon.smdh") }, "making the icon");
                Tool(bannertool, new[] { "makebanner", bannerArt[0], bannerArt[1], "-a", sound,
                                         "-o", Path.Combine(art, "banner.bnr") }, "making the banner");

                // 4. the .cia (make_cia.py's makerom line)
                progress(86, "Building the .cia...");
                var made = Path.Combine(tmp, "out.cia");
                Tool(Kit.Need("makerom.exe"), new[] {
                        "-f", "cia", "-o", made, "-elf", Kit.Need("game3ds.elf"),
                        "-rsf", Kit.Need("game3ds.rsf"),
                        "-icon", Path.Combine(art, "icon.smdh"), "-banner", Path.Combine(art, "banner.bnr"),
                        "-DROMFS_DIR=" + romfs, "-target", "t", "-exefslogo" }, "building the .cia");
                if (!File.Exists(made) || new FileInfo(made).Length < 1024 * 1024)
                    throw new BuildError("makerom did not write the .cia.");
                Directory.CreateDirectory(Path.GetDirectoryName(output));
                if (File.Exists(output)) File.Delete(output);
                File.Move(made, output);
                log("Wrote " + output + " (" + (new FileInfo(output).Length / (1024 * 1024)) + " MB).");
                progress(100, "Done.");
            }
            catch (IOException e)
            {
                throw new BuildError("A file could not be read or written: " + e.Message);
            }
            catch (UnauthorizedAccessException e)
            {
                throw new BuildError("Windows refused access to a file: " + e.Message);
            }
            finally
            {
                if (Keep) log("Kept the work folder: " + tmp);
                else try { if (Directory.Exists(tmp)) Directory.Delete(tmp, true); } catch (Exception) { }
            }
        }

        // The cartridge header, with the checks a player can act on before the
        // extractor's own (it checks them again, and everything after them).
        byte[] ReadHeader()
        {
            var h = new byte[0x200];
            try
            {
                using (var f = File.OpenRead(rom))
                {
                    if (f.Length < 0x200 || f.Read(h, 0, h.Length) != h.Length)
                        throw new BuildError("This file is too small to be a DS dump.");
                }
            }
            catch (IOException e)
            {
                throw new BuildError("The dump could not be read: " + e.Message);
            }
            var code = Encoding.ASCII.GetString(h, 12, 4);
            if (!code.StartsWith("ASM"))
                throw new BuildError("This is not a Super Mario 64 DS dump (its game code is " +
                                     code.Replace('\0', ' ').Trim() + ").");
            if (code != "ASMP")
                throw new BuildError("This is Super Mario 64 DS, but not the European version (game code " +
                                     code + "). " + Program.Title + " is built from the European " +
                                     "cartridge (ASMP); its data does not come out of another region's.");
            return h;
        }

        void Tool(string exe, string[] args, string doing)
        {
            var lines = new List<string>();
            int rc = Proc.Run(exe, args, delegate (string line) { lock (lines) lines.Add(line); });
            if (rc != 0)
            {
                foreach (var l in lines) log("  " + l);
                throw new BuildError("Stopped while " + doing + " (" + Path.GetFileName(exe) +
                                     " exit code " + rc + ").");
            }
        }
    }

    static class Proc
    {
        static string Quote(string a)
        {
            if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return a;
            var sb = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in a)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') sb.Append('\\', slashes * 2 + 1);
                else sb.Append('\\', slashes);
                slashes = 0;
                sb.Append(c);
            }
            sb.Append('\\', slashes * 2);
            return sb.Append('"').ToString();
        }

        public static int Run(string exe, string[] args, Action<string> onLine)
        {
            var parts = new List<string>();
            foreach (var a in args) parts.Add(Quote(a));
            var si = new ProcessStartInfo(exe, string.Join(" ", parts.ToArray()));
            si.UseShellExecute = false;
            si.CreateNoWindow = true;
            si.RedirectStandardOutput = true;
            si.RedirectStandardError = true;
            si.StandardOutputEncoding = Encoding.UTF8;
            using (var p = new Process())
            {
                p.StartInfo = si;
                p.OutputDataReceived += delegate (object s, DataReceivedEventArgs e) { if (e.Data != null) onLine(e.Data); };
                p.ErrorDataReceived += delegate (object s, DataReceivedEventArgs e) { if (e.Data != null) onLine(e.Data); };
                try
                {
                    p.Start();
                }
                catch (System.ComponentModel.Win32Exception e)
                {
                    throw new BuildError("Could not start " + Path.GetFileName(exe) + ": " + e.Message);
                }
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                return p.ExitCode;
            }
        }
    }

    // Same S3PK pack as port3ds/tools/pack_files.py, byte for byte: same order
    // (Python's sorted() over Windows paths, compared per component, lowercased),
    // and dsd's files.txt left out.
    static class Pack
    {
        class PathOrder : IComparer<string[]>
        {
            public int Compare(string[] a, string[] b)
            {
                for (int i = 0; i < Math.Min(a.Length, b.Length); i++)
                {
                    int c = string.CompareOrdinal(a[i].ToLowerInvariant(), b[i].ToLowerInvariant());
                    if (c != 0) return c;
                }
                return a.Length.CompareTo(b.Length);
            }
        }

        public static int Write(string src, string outPath)
        {
            src = Path.GetFullPath(src).TrimEnd(Path.DirectorySeparatorChar);
            var rels = new List<string[]>();
            foreach (var f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                var rel = f.Substring(src.Length + 1).Replace(Path.DirectorySeparatorChar, '/');
                if (rel == "files.txt") continue;
                rels.Add(rel.Split('/'));
            }
            rels.Sort(new PathOrder());
            var names = new List<byte[]>();
            long indexLen = 0;
            foreach (var r in rels)
            {
                var n = Encoding.UTF8.GetBytes(string.Join("/", r));
                names.Add(n);
                indexLen += 10 + n.Length;
            }
            long dataOff = (16 + indexLen + 3) & ~3L;
            Directory.CreateDirectory(Path.GetDirectoryName(outPath));
            using (var o = new BinaryWriter(File.Create(outPath)))
            {
                o.Write(Encoding.ASCII.GetBytes("S3PK"));
                o.Write((uint)1);
                o.Write((uint)rels.Count);
                o.Write((uint)dataOff);
                long at = dataOff;
                for (int i = 0; i < rels.Count; i++)
                {
                    long len = new FileInfo(Path.Combine(src, string.Join("\\", rels[i]))).Length;
                    o.Write((uint)at);
                    o.Write((uint)len);
                    o.Write((ushort)names[i].Length);
                    o.Write(names[i]);
                    at += len + ((-len) & 3);
                }
                while (o.BaseStream.Position < dataOff) o.Write((byte)0);
                for (int i = 0; i < rels.Count; i++)
                {
                    var b = File.ReadAllBytes(Path.Combine(src, string.Join("\\", rels[i])));
                    o.Write(b);
                    for (long pad = (-(long)b.Length) & 3; pad > 0; pad--) o.Write((byte)0);
                }
            }
            return rels.Count;
        }
    }

    // HOME Menu art from the DS game's own icon: the 32x32 16-colour picture in
    // the dump's banner (header +0x68: tiles at +0x20, palette at +0x220, colour 0
    // transparent).
    static class Art
    {
        public static Bitmap DsIcon(string rom)
        {
            byte[] tiles = new byte[512], pal = new byte[32];
            using (var f = File.OpenRead(rom))
            {
                var r = new BinaryReader(f);
                f.Position = 0x68;
                uint banner = r.ReadUInt32();
                if (banner == 0 || banner + 0x240 > f.Length)
                    throw new BuildError("The dump has no DS icon to make the HOME Menu icon from.");
                f.Position = banner + 0x20;
                tiles = r.ReadBytes(512);
                pal = r.ReadBytes(32);
            }
            var colours = new Color[16];
            for (int i = 0; i < 16; i++)
            {
                int c = pal[i * 2] | pal[i * 2 + 1] << 8;
                int rr = c & 31, gg = (c >> 5) & 31, bb = (c >> 10) & 31;
                colours[i] = i == 0 ? Color.Transparent
                                    : Color.FromArgb(rr << 3 | rr >> 2, gg << 3 | gg >> 2, bb << 3 | bb >> 2);
            }
            var bmp = new Bitmap(32, 32, PixelFormat.Format32bppArgb);
            for (int ty = 0; ty < 4; ty++)
                for (int tx = 0; tx < 4; tx++)
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x++)
                        {
                            byte b = tiles[(ty * 4 + tx) * 32 + y * 4 + x / 2];
                            int idx = (x & 1) == 0 ? b & 15 : b >> 4;
                            bmp.SetPixel(tx * 8 + x, ty * 8 + y, colours[idx]);
                        }
            return bmp;
        }

        static Bitmap Scaled(Bitmap src, int factor)
        {
            var big = new Bitmap(src.Width * factor, src.Height * factor, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(big))
            {
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.PixelOffsetMode = PixelOffsetMode.Half;
                g.DrawImage(src, 0, 0, big.Width, big.Height);
            }
            return big;
        }

        // 48x48 (the SMDH's large icon), on white: an SMDH icon has no alpha
        public static void SaveIcon(Bitmap ds, string path)
        {
            using (var big = Scaled(ds, 3))
            using (var icon = new Bitmap(48, 48, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(icon))
                {
                    g.Clear(Color.White);
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    g.DrawImage(big, 2, 2, 44, 44);
                }
                icon.Save(path, ImageFormat.Png);
            }
        }

        // The title screen's logo, from overlay 7 of the dump: title_demo_main.ncg
        // laid out by its screen, in the title palette. These are NITRO-System's
        // source formats (NCCL/NCSC/NCCG) embedded in the overlay. Each block is
        // checked against the European ROM's hash; null if any doesn't match.
        public static Bitmap TitleLogo(string rom)
        {
            byte[] ov = Overlay(rom, 7);
            if (ov == null) return null;
            byte[] pal = Block(ov, 0x2bf08, 556, "8d3a76050a99c1b2007249164cb4047450ea607d5a70a01f90017a2cedf4cc2a");
            byte[] scr = Block(ov, 0x30068, 6420, "eca1045a4f516efcfa532f0ed85bf97a2d9bef295db19f51d97825b35780b4fd");
            byte[] chr = Block(ov, 0x3d08c, 33392, "16eac000c3f8de4135ccd1b78d465aa52bb350fba1323162f4ddf20f2f1bbc83");
            if (pal == null || scr == null || chr == null) return null;

            // PALT colours start at +0x20, the SCRN's 32x32 entries at +0x28, the
            // CHAR's 8bpp tiles at +0x24
            var colours = new int[256];
            for (int i = 1; i < 256; i++)
            {
                int c = pal[0x20 + i * 2] | pal[0x21 + i * 2] << 8;
                int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                colours[i] = unchecked((int)0xff000000) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            }
            var px = new int[256 * 256];
            int minX = 256, minY = 256, maxX = -1, maxY = -1;
            for (int k = 0; k < 32 * 32; k++)
            {
                int e = scr[0x28 + k * 2] | scr[0x29 + k * 2] << 8;
                int tile = e & 0x3ff;
                bool hf = (e & 0x400) != 0, vf = (e & 0x800) != 0;
                if (0x24 + tile * 64 + 64 > chr.Length) continue;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                    {
                        int idx = chr[0x24 + tile * 64 + (vf ? 7 - y : y) * 8 + (hf ? 7 - x : x)];
                        if (idx == 0) continue;
                        int X = (k % 32) * 8 + x, Y = (k / 32) * 8 + y;
                        px[Y * 256 + X] = colours[idx];
                        minX = Math.Min(minX, X); maxX = Math.Max(maxX, X);
                        minY = Math.Min(minY, Y); maxY = Math.Max(maxY, Y);
                    }
            }
            if (maxX < 0) return null;
            var logo = new Bitmap(maxX - minX + 1, maxY - minY + 1, PixelFormat.Format32bppArgb);
            var data = logo.LockBits(new Rectangle(0, 0, logo.Width, logo.Height), ImageLockMode.WriteOnly,
                                     PixelFormat.Format32bppArgb);
            for (int y = 0; y < logo.Height; y++)
                Marshal.Copy(px, (minY + y) * 256 + minX, data.Scan0 + y * data.Stride, logo.Width);
            logo.UnlockBits(data);
            return logo;
        }

        static byte[] Block(byte[] ov, int at, int size, string sha)
        {
            if (at + size > ov.Length) return null;
            var b = new byte[size];
            Array.Copy(ov, at, b, 0, size);
            using (var h = SHA256.Create())
                return BitConverter.ToString(h.ComputeHash(b)).Replace("-", "").ToLowerInvariant() == sha ? b : null;
        }

        // An ARM9 overlay out of the dump, decompressed
        static byte[] Overlay(string rom, int id)
        {
            using (var f = File.OpenRead(rom))
            {
                var r = new BinaryReader(f);
                f.Position = 0x48;
                uint fat = r.ReadUInt32();
                f.Position = 0x50;
                uint table = r.ReadUInt32(), tableSize = r.ReadUInt32();
                for (uint i = 0; i < tableSize / 32; i++)
                {
                    f.Position = table + i * 32;
                    if (r.ReadUInt32() != id) continue;
                    f.Position = table + i * 32 + 24;
                    uint file = r.ReadUInt32(), flags = r.ReadUInt32();
                    f.Position = fat + file * 8;
                    uint start = r.ReadUInt32(), end = r.ReadUInt32();
                    if (end <= start || end > f.Length) return null;
                    f.Position = start;
                    byte[] raw = r.ReadBytes((int)(end - start));
                    return (flags & 0x1000000) != 0 ? Blz(raw) : raw;
                }
            }
            return null;
        }

        // The DS's backward LZ (overlays and arm9): decoded from the end down
        static byte[] Blz(byte[] c)
        {
            if (c.Length < 8) return null;
            uint hdr = BitConverter.ToUInt32(c, c.Length - 8);
            int extra = BitConverter.ToInt32(c, c.Length - 4);
            int enc = (int)(hdr & 0xffffff), headLen = (int)(hdr >> 24);
            var o = new byte[c.Length + extra];
            Array.Copy(c, o, c.Length);
            int src = c.Length - headLen, dst = o.Length, stop = c.Length - enc;
            if (stop < 0 || src > c.Length) return null;
            while (src > stop)
            {
                int flags = c[--src];
                for (int bit = 0; bit < 8 && src > stop; bit++)
                {
                    if ((flags & (0x80 >> bit)) != 0)
                    {
                        src -= 2;
                        int v = c[src + 1] << 8 | c[src];
                        int len = (v >> 12) + 3, disp = (v & 0xfff) + 3;
                        if (dst - len < 0 || dst - 1 + disp >= o.Length) return null;
                        while (len-- > 0) { dst--; o[dst] = o[dst + disp]; }
                    }
                    else
                    {
                        if (dst == 0) return null;
                        o[--dst] = c[--src];
                    }
                }
            }
            return o;
        }

        // 256x128: the title logo on a sky, or the DS icon if the logo couldn't be read
        public static void SaveBanner(Bitmap logo, Bitmap ds, string path)
        {
            using (var banner = new Bitmap(256, 128, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(banner))
                {
                    var all = new Rectangle(0, 0, 256, 128);
                    using (var sky = new LinearGradientBrush(all, Color.FromArgb(52, 132, 228),
                                                             Color.FromArgb(150, 208, 255), 90f))
                        g.FillRectangle(sky, all);
                    Bitmap art = logo ?? ds;
                    float s = logo != null ? Math.Min(1f, Math.Min(244f / art.Width, 120f / art.Height)) : 3f;
                    int w = (int)Math.Round(art.Width * s), h = (int)Math.Round(art.Height * s);
                    g.InterpolationMode = s == 1f || logo == null ? InterpolationMode.NearestNeighbor
                                                                   : InterpolationMode.HighQualityBicubic;
                    g.PixelOffsetMode = PixelOffsetMode.Half;
                    g.DrawImage(art, (256 - w) / 2, (128 - h) / 2, w, h);
                }
                banner.Save(path, ImageFormat.Png);
            }
        }

        // a short silent banner sound, for a builder-files without banner.wav
        // (bannertool wants one; the HOME Menu plays it)
        public static void SaveSilence(string path)
        {
            const int rate = 22050, channels = 2, samples = rate / 2;
            int bytes = samples * channels * 2;
            using (var w = new BinaryWriter(File.Create(path)))
            {
                w.Write(Encoding.ASCII.GetBytes("RIFF"));
                w.Write(36 + bytes);
                w.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
                w.Write(16);
                w.Write((short)1);
                w.Write((short)channels);
                w.Write(rate);
                w.Write(rate * channels * 2);
                w.Write((short)(channels * 2));
                w.Write((short)16);
                w.Write(Encoding.ASCII.GetBytes("data"));
                w.Write(bytes);
                w.Write(new byte[bytes]);
            }
        }
    }
}
