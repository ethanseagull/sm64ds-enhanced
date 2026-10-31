// The build settings: the console a build is for and the game's four options
// (the switches of its SELECT menu), chosen in the builder's settings dialog
// and written into the .cia as the game's options.txt (Build.Run). The game
// reads that file at boot until the player saves their own from the menu
// (port3ds/game/options3ds.cpp).
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace Sm64dsEnhanced
{
    // What a build starts the game with. The window's choices are remembered in
    // %LOCALAPPDATA%\SM64DSEnhancedBuilder\settings.txt, in options.txt's lines.
    class BuildOptions
    {
        public bool Old;   // for an Old 3DS (a New 3DS otherwise)
        public bool Fps60 = true, Aa = true, Counter, Analog = true;

        // The console's defaults: on a New 3DS everything but the FPS counter,
        // on an Old 3DS only analog controls.
        public void Reset()
        {
            Heavy();
            Counter = false;
            Analog = true;
        }

        // 60 fps and anti-aliasing, the two that depend on the console: on for a
        // New 3DS, off for an Old 3DS
        public void Heavy()
        {
            Fps60 = Aa = !Old;
        }

        public BuildOptions Copy() { return (BuildOptions)MemberwiseClone(); }

        public void Take(BuildOptions o)
        {
            Old = o.Old;
            Fps60 = o.Fps60;
            Aa = o.Aa;
            Counter = o.Counter;
            Analog = o.Analog;
        }

        public bool Same(BuildOptions o)
        {
            return Old == o.Old && Fps60 == o.Fps60 && Aa == o.Aa && Counter == o.Counter && Analog == o.Analog;
        }

        // options.txt's lines. The game puts console= only in its log: it sizes
        // itself to the console it runs on either way.
        public string FileText()
        {
            return "# The builder's settings, read at boot until the options menu (SELECT)\n" +
                   "# saves sdmc:/sm64ds3ds/options.txt.\n" +
                   "console=" + (Old ? "old" : "new") + "\n" +
                   "60fps=" + Bit(Fps60) + "\n" +
                   "anti_aliasing=" + Bit(Aa) + "\n" +
                   "fps_counter=" + Bit(Counter) + "\n" +
                   "analog_controls=" + Bit(Analog) + "\n";
        }

        static string Bit(bool on) { return on ? "1" : "0"; }

        // One KEY=VALUE line; false if it isn't one of options.txt's. A console
        // line brings that console's 60 fps and anti-aliasing defaults, so lines
        // after it (as in FileText) still have the last word.
        public bool Set(string line)
        {
            int eq = line.IndexOf('=');
            if (eq < 0) return false;
            string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim().ToLowerInvariant();
            bool on = value == "1" || value == "on" || value == "true";
            switch (key)
            {
                case "console":
                    if (value != "old" && value != "new") return false;
                    Old = value == "old";
                    Heavy();
                    return true;
                case "60fps": Fps60 = on; return true;
                case "anti_aliasing": Aa = on; return true;
                case "fps_counter": Counter = on; return true;
                case "analog_controls": Analog = on; return true;
            }
            return false;
        }

        public string Summary()
        {
            return (Old ? "Old 3DS" : "New 3DS") + "; 60 fps " + OnOff(Fps60) + ", anti-aliasing " + OnOff(Aa) +
                   ", FPS counter " + OnOff(Counter) + ", analog controls " + OnOff(Analog);
        }

        static string OnOff(bool on) { return on ? "on" : "off"; }

        static string SavedFile
        {
            get
            {
                return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                    "SM64DSEnhancedBuilder", "settings.txt");
            }
        }

        public static BuildOptions Load()
        {
            var o = new BuildOptions();
            try
            {
                if (File.Exists(SavedFile))
                    foreach (var line in File.ReadAllLines(SavedFile))
                        if (!line.StartsWith("#")) o.Set(line);
            }
            catch (Exception) { }
            return o;
        }

        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(SavedFile));
                File.WriteAllText(SavedFile, FileText(), new UTF8Encoding(false));
            }
            catch (Exception) { }
        }
    }

    // The settings dialog, under the same night sky: the console as two cards
    // (the chosen one gold), the game's options as switches on a glass panel,
    // and Done. Esc or the close box leaves the settings as they were.
    class SettingsForm : Form
    {
        public readonly BuildOptions Chosen;
        readonly Bitmap sky;
        readonly ConsoleCard newCard, oldCard;
        readonly Switch fps, aa, counter, analog;

        const int Pad = 28, W = 440, H = 436, RowH = 42;
        static readonly PointF Mark = new PointF(Pad + 13, 41);
        static readonly Rectangle CardsBox = new Rectangle(Pad, 92, W - Pad * 2, 46);
        static readonly Rectangle PanelBox = new Rectangle(Pad, 178, W - Pad * 2, RowH * 4 + 8);
        static readonly Rectangle FooterBox = new Rectangle(Pad - 2, 370, W - Pad * 2 + 4, 46);

        public SettingsForm(BuildOptions current)
        {
            Chosen = current.Copy();
            Text = "Build settings";
            Font = new Font(Look.Face, 13f, GraphicsUnit.Pixel);
            ForeColor = Look.Text;
            BackColor = Look.SkyMid;
            ClientSize = new Size(Look.Px(W), Look.Px(H));
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            ShowIcon = ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            DoubleBuffered = true;
            sky = Sky();

            float half = (CardsBox.Width - 10) / 2f;
            newCard = new ConsoleCard { Text = "New 3DS" };
            newCard.SetBounds(Look.Px(CardsBox.X), Look.Px(CardsBox.Y), Look.Px(half), Look.Px(CardsBox.Height));
            oldCard = new ConsoleCard { Text = "Old 3DS" };
            oldCard.SetBounds(Look.Px(CardsBox.X + half + 10), Look.Px(CardsBox.Y), Look.Px(half), Look.Px(CardsBox.Height));
            newCard.Click += delegate { PickConsole(false); };
            oldCard.Click += delegate { PickConsole(true); };

            fps = Row(0, "60 FPS");
            aa = Row(1, "Anti-aliasing");
            counter = Row(2, "FPS counter");
            analog = Row(3, "Analog controls");
            fps.Toggled += delegate { Chosen.Fps60 = fps.On; };
            aa.Toggled += delegate { Chosen.Aa = aa.On; };
            counter.Toggled += delegate { Chosen.Counter = counter.On; };
            analog.Toggled += delegate { Chosen.Analog = analog.On; };

            var restore = new FlatButton { Text = "Restore defaults" };
            restore.SetBounds(Look.Px(Pad), Look.Px(FooterBox.Y + 2), Look.Px(150), Look.Px(42));
            restore.Click += delegate { Chosen.Reset(); ShowChosen(); };
            var done = new FlatButton { Text = "Done", Primary = true };
            done.SetBounds(Look.Px(W - Pad - 120), Look.Px(FooterBox.Y + 2), Look.Px(120), Look.Px(42));
            done.Click += delegate { DialogResult = DialogResult.OK; };

            Controls.AddRange(new Control[] { newCard, oldCard, fps, aa, counter, analog, restore, done });
            HandleCreated += delegate { Look.DarkTitleBar(Handle); };
            FormClosed += delegate { sky.Dispose(); };
            Shown += delegate { done.Focus(); };
            ShowChosen();
        }

        Switch Row(int i, string name)
        {
            var s = new Switch { Text = name };
            s.SetBounds(Look.Px(PanelBox.X + 4), Look.Px(PanelBox.Y + 4 + i * RowH), Look.Px(PanelBox.Width - 8), Look.Px(RowH));
            return s;
        }

        // The console's own 60 fps and anti-aliasing defaults come with it; the
        // other switches stay as they are.
        void PickConsole(bool old)
        {
            if (Chosen.Old == old) return;
            Chosen.Old = old;
            Chosen.Heavy();
            ShowChosen();
        }

        void ShowChosen()
        {
            newCard.On = !Chosen.Old;
            oldCard.On = Chosen.Old;
            fps.On = Chosen.Fps60;
            aa.On = Chosen.Aa;
            counter.On = Chosen.Counter;
            analog.On = Chosen.Analog;
            foreach (Control c in Controls) c.Invalidate();
        }

        // Everything that doesn't change: the sky, the star and title, the
        // headings, and the options' glass panel with its dividers.
        Bitmap Sky()
        {
            var bmp = new Bitmap(Look.Px(W), Look.Px(H), PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(bmp))
            {
                Look.Layout(g);
                Look.Night(g, W, H, new[] { CardsBox, PanelBox, FooterBox });
                Look.GoldStar(g, Mark, 13);
                using (var t = Look.Lettering(g, "Build settings", Look.Display, 22f, 0, new PointF(Pad + 36, 26)))
                {
                    var box = t.GetBounds();
                    using (var b = new LinearGradientBrush(new RectangleF(box.X, box.Y - 1, box.Width, box.Height + 2),
                                                           Color.White, Color.FromArgb(200, 210, 240), 90f))
                        g.FillPath(b, t);
                }
                Heading(g, "CONSOLE", CardsBox.Y - 18);
                Heading(g, "GAME OPTIONS", PanelBox.Y - 18);
                Look.Box(g, new RectangleF(PanelBox.X + 0.5f, PanelBox.Y + 0.5f, PanelBox.Width - 1, PanelBox.Height - 1), 14,
                         Look.White(9), Look.White(34));
                using (var p = new Pen(Look.White(18), 1f))
                    for (int i = 1; i < 4; i++)
                    {
                        float y = PanelBox.Y + 4 + i * RowH + 0.5f;
                        g.DrawLine(p, PanelBox.X + 16, y, PanelBox.Right - 16, y);
                    }
            }
            return bmp;
        }

        static void Heading(Graphics g, string text, float y)
        {
            using (var t = Look.Lettering(g, text, Look.Display, 10.5f, 2.4f, new PointF(Pad + 2, y)))
            using (var b = new SolidBrush(Look.Gold))
                g.FillPath(b, t);
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            var g = e.Graphics;
            var c = e.ClipRectangle;
            g.CompositingMode = CompositingMode.SourceCopy;
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.DrawImage(sky, c, c, GraphicsUnit.Pixel);
            g.CompositingMode = CompositingMode.SourceOver;
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if (keyData == Keys.Escape)
            {
                DialogResult = DialogResult.Cancel;
                return true;
            }
            return base.ProcessCmdKey(ref msg, keyData);
        }
    }

    // A console to build for: gold when chosen, glass otherwise, with a small
    // open 3DS drawn beside its name.
    class ConsoleCard : Control
    {
        public bool On;
        bool hover;

        public ConsoleCard()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.SupportsTransparentBackColor | ControlStyles.Selectable, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
            TabStop = true;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            Look.Layout(g);
            float w = Width / Look.S, h = Height / Look.S;
            var r = new RectangleF(0.5f, 0.5f, w - 1, h - 1);
            Color ink;
            if (On)
            {
                if (hover) Look.GoldBox(g, r, 12, Color.FromArgb(255, 236, 166), Color.FromArgb(250, 186, 66));
                else Look.GoldBox(g, r, 12, Look.GoldLight, Look.GoldDeep);
                ink = Look.GoldInk;
            }
            else
            {
                Look.Box(g, r, 12, Look.White(hover ? 16 : 9), Look.White(hover ? 64 : 34));
                ink = hover ? Look.Text : Color.FromArgb(214, 220, 240);
            }
            Clamshell(g, new PointF(30, h / 2), ink);
            Look.Write(g, Text, Look.Strong, 14.5f, new RectangleF(52, 0, w - 58, h), ink,
                       TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            if (Focused && ShowFocusCues)
                using (var p = new Pen(On ? Color.FromArgb(150, Look.GoldInk) : Look.White(150), 1.2f))
                using (var inner = Look.Rounded(RectangleF.Inflate(r, -3, -3), 9))
                    g.DrawPath(p, inner);
        }

        // an open 3DS in outline: the wide top screen over the bottom half
        static void Clamshell(Graphics g, PointF c, Color ink)
        {
            using (var p = new Pen(ink, 1.4f))
            {
                p.LineJoin = LineJoin.Round;
                using (var top = Look.Rounded(new RectangleF(c.X - 11, c.Y - 13, 22, 12), 2.5f)) g.DrawPath(p, top);
                using (var low = Look.Rounded(new RectangleF(c.X - 10, c.Y + 1, 20, 12), 2.5f)) g.DrawPath(p, low);
                using (var b = new SolidBrush(Color.FromArgb(ink.A / 2, ink)))
                {
                    g.FillRectangle(b, c.X - 7.5f, c.Y - 10.5f, 15, 7);   // the top screen
                    g.FillRectangle(b, c.X - 5, c.Y + 3.5f, 10, 7);       // the touch screen
                }
            }
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyUp(e);
        }
    }

    // One of the game's options: its name and a switch that's gold when on.
    // The whole row toggles it.
    class Switch : Control
    {
        public bool On;
        public event EventHandler Toggled;
        bool hover;

        public Switch()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw |
                     ControlStyles.SupportsTransparentBackColor | ControlStyles.Selectable, true);
            BackColor = Color.Transparent;
            Cursor = Cursors.Hand;
            TabStop = true;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            Look.Layout(g);
            float w = Width / Look.S, h = Height / Look.S;
            var r = new RectangleF(0.5f, 0.5f, w - 1, h - 1);
            if (hover) Look.Box(g, RectangleF.Inflate(r, -1, -1), 10, Look.White(10), Color.Empty);
            if (Focused && ShowFocusCues)
                using (var p = new Pen(Color.FromArgb(170, Look.Gold), 1.2f))
                using (var path = Look.Rounded(RectangleF.Inflate(r, -1.5f, -1.5f), 10))
                    g.DrawPath(p, path);
            Look.Write(g, Text, Look.Strong, 14f, new RectangleF(14, 0, w - 84, h), Look.Text,
                       TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);

            var track = new RectangleF(w - 14 - 42, h / 2 - 11.5f, 42, 23);
            float knob = 8.5f, kx = On ? track.Right - 11.5f : track.X + 11.5f, ky = track.Y + track.Height / 2;
            if (On)
            {
                Look.GoldBox(g, track, 11.5f, hover ? Color.FromArgb(255, 236, 166) : Look.GoldLight, Look.GoldDeep);
                Look.Glow(g, new PointF(kx, ky + 1), knob + 3, knob + 3, Color.FromArgb(70, Look.GoldInk));
                using (var b = new SolidBrush(Color.FromArgb(255, 252, 240))) g.FillEllipse(b, kx - knob, ky - knob, knob * 2, knob * 2);
            }
            else
            {
                Look.Box(g, track, 11.5f, Look.White(hover ? 20 : 12), Look.White(hover ? 72 : 52));
                using (var b = new SolidBrush(hover ? Look.Text : Look.Muted)) g.FillEllipse(b, kx - knob + 1, ky - knob + 1, knob * 2 - 2, knob * 2 - 2);
            }
        }

        protected override void OnClick(EventArgs e)
        {
            On = !On;
            Invalidate();
            if (Toggled != null) Toggled(this, EventArgs.Empty);
            base.OnClick(e);
        }

        protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnGotFocus(EventArgs e) { Invalidate(); base.OnGotFocus(e); }
        protected override void OnLostFocus(EventArgs e) { Invalidate(); base.OnLostFocus(e); }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter) OnClick(EventArgs.Empty);
            base.OnKeyUp(e);
        }
    }
}
