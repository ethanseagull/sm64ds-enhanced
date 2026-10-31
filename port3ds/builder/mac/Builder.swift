// Super Mario 64 DS Enhanced builder for macOS: the Windows builder
// (Builder.cs) as a Mac app. It takes a player's own cartridge dump and makes
// an installable .cia, the same way and from the same files as the Windows
// builder (its builder-files folder; package.py): the port (built
// ROM-clean, with every table from the ROM zeroed), a recipe of where those
// tables live in a dump (offsets and hashes), makerom and bannertool (built
// for the Mac), and none of Nintendo's data. Everything of the game comes
// from the dump:
//
//   1. Extract.swift (the PC port's extract_assets.ps1, ported; the Windows
//      builder's Extract.cs is the same script in C#) pulls the NitroFS
//      files, asset catalogs and romdata.bin, hash-checking each range.
//      European ROM only.
//   2. The RomFS is laid out like port3ds/cia/make_cia.py does: build/assets,
//      extracted/dsd/header.yaml and extracted/files.pak.
//   3. The HOME Menu icon is drawn from the DS game's own icon. The banner is
//      the title-screen logo in 3D, with the Power Star flying round it as
//      in the opening, made from the dump's own files into face.template
//      (Face.swift); a flat picture of the logo if that can't be made.
//      banner.wav is the HOME Menu sound.
//   4. makerom builds the .cia with game3ds.rsf.
//
// The window just takes the dump and builds (the .cia goes next to the dump),
// with music.mp3 looping (mutable).
//
// The app (package.py) is SM64DS Enhanced Builder.app: this program in
// Contents/MacOS, makerom and bannertool in Contents/Helpers, the payload in
// Contents/Resources/payload, music.mp3 and done.mp3 beside it.
import AppKit
import AVFoundation
import CryptoKit
import Foundation

@main
enum Program {
    static let title = "Super Mario 64 DS Enhanced"
    static let author = "ethanseagull"

    // No arguments: the window. --rom DUMP --out FILE.cia: the same build with
    // no window (scripts, tests), its lines in FILE.cia.log (and on standard
    // output); exit code 0 or 1. It takes the defaults, not the window's saved
    // settings; --option KEY=VALUE (options.txt's keys and console=new|old,
    // which sets that console's 60 fps and anti-aliasing defaults, so it goes
    // first) changes one. --snapshot DIR and --icon DIR draw the windows and
    // the app icon to PNGs (package.py, and checking the look) instead.
    static func main() {
        let args = Array(CommandLine.arguments.dropFirst())
        if args.contains(where: { $0.hasPrefix("--") }) {
            if let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count { exit(Snapshot.windows(args[i + 1], args)) }
            if let i = args.firstIndex(of: "--icon"), i + 1 < args.count { exit(Snapshot.icon(args[i + 1])) }
            exit(headless(args))
        }
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        withExtendedLifetime(delegate) { app.run() }
    }

    static func headless(_ args: [String]) -> Int32 {
        var rom: String?, output: String?
        var keep = false
        let options = BuildOptions()
        var i = 0
        while i < args.count {
            if args[i] == "--keep" { keep = true }   // leave the work folder (its path is logged)
            else if args[i] == "--rom" && i + 1 < args.count { i += 1; rom = args[i] }
            else if args[i] == "--out" && i + 1 < args.count { i += 1; output = args[i] }
            else if args[i] == "--option" && i + 1 < args.count { i += 1; if !options.set(args[i]) { return 2 } }
            i += 1
        }
        guard let rom = rom, let output = output else { return 2 }
        guard FileManager.default.createFile(atPath: output + ".log", contents: nil),
              let log = FileHandle(forWritingAtPath: output + ".log") else { return 1 }
        let lock = NSLock()
        let line: (String) -> Void = { s in
            lock.lock()
            log.write(Data((s + "\n").utf8))
            print(s)
            lock.unlock()
        }
        line(title + " Builder, " + Payload.label() + ".")
        let clock = Date()
        let build = Build(rom: rom, output: output, log: line) { pct, what in
            if let what = what {
                line(String(format: "[%d%%] %.1f s  ", pct, Date().timeIntervalSince(clock)) + what)
            }
        }
        build.keep = keep
        build.options = options
        do {
            try build.run()
            return 0
        } catch let e as BuildError {
            line(e.message)
            return 1
        } catch {
            line(String(describing: error))
            return 1
        }
    }
}

struct BuildError: Error, CustomStringConvertible {
    let message: String
    init(_ message: String) { self.message = message }
    var description: String { return message }
}

// The look: a night sky. A deep blue gradient with soft glows and star
// dust, a few stars that slowly twinkle, a gold star beside the title, and
// glassy cards, as on Windows; the type is the Mac's own (SF Pro). Layout
// units are points, which macOS draws sharp at any display scale. The only
// game art is the chosen dump's DS icon.
enum Look {
    static let skyTop = rgb(8, 10, 28)
    static let skyMid = rgb(14, 19, 49)
    static let skyLow = rgb(29, 36, 86)
    static let text = rgb(240, 243, 252)
    static let muted = rgb(146, 156, 198)
    static let faint = rgb(92, 100, 138)
    static let gold = rgb(255, 206, 84)
    static let goldLight = rgb(255, 228, 140)
    static let goldDeep = rgb(240, 164, 42)
    static let goldInk = rgb(44, 28, 4)   // text on gold
    static let space = CGColorSpace(name: CGColorSpace.sRGB)!

    static func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: Int = 255) -> CGColor {
        return CGColor(srgbRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: CGFloat(a) / 255)
    }

    // white at this opacity: the cards and buttons are glass over the sky
    static func white(_ a: Int) -> CGColor { return rgb(255, 255, 255, a) }

    // a colour at this opacity (C#'s Color.FromArgb(a, colour))
    static func alpha(_ a: Int, _ c: CGColor) -> CGColor { return c.copy(alpha: CGFloat(a) / 255) ?? c }

    static func display(_ size: CGFloat) -> NSFont { return .systemFont(ofSize: size, weight: .semibold) }
    static func face(_ size: CGFloat) -> NSFont { return .systemFont(ofSize: size) }
    static func strong(_ size: CGFloat) -> NSFont { return .systemFont(ofSize: size, weight: .semibold) }

    // ~/Library/Application Support/SM64DSEnhancedBuilder: the settings and the mute
    static var supportFolder: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("SM64DSEnhancedBuilder")
    }

    // a window in the sky's colours: dark, its title bar the sky's top with
    // no title in it (the sky has the title)
    static func dress(_ w: NSWindow) {
        w.appearance = NSAppearance(named: .darkAqua)
        w.titlebarAppearsTransparent = true
        w.titleVisibility = .hidden
        w.backgroundColor = NSColor(cgColor: skyTop)
        w.isReleasedWhenClosed = false
        w.tabbingMode = .disallowed
    }

    enum Align { case left, center, right }

    // One line of text in r, vertically centred as Windows' TextRenderer
    // centres it (the capitals a touch below the middle), cut off with an
    // ellipsis if it doesn't fit
    static func write(_ s: String, _ font: NSFont, _ r: CGRect, _ ink: CGColor, _ align: Align = .left) {
        let style = NSMutableParagraphStyle()
        style.alignment = align == .left ? .left : align == .center ? .center : .right
        style.lineBreakMode = .byTruncatingTail
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor(cgColor: ink) ?? .white,
                                                    .paragraphStyle: style]
        let baseline = r.midY + font.capHeight / 2 + font.pointSize * 0.06
        let box = CGRect(x: r.minX, y: baseline - font.ascender, width: r.width,
                         height: ceil(font.ascender - font.descender + font.leading))
        (s as NSString).draw(with: box, options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine], attributes: attrs)
    }

    // Display text as a shape (the title): smooth at any scale, fillable with
    // a gradient, and spaced out by tracking (points between letters). `at`
    // is the top left of the line as GDI+'s AddString takes it, so the
    // baseline is where Segoe UI's ascent puts it on Windows.
    static func lettering(_ s: String, _ font: NSFont, _ tracking: CGFloat, _ at: CGPoint) -> CGPath {
        let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: [.font: font, .kern: tracking]))
        let path = CGMutablePath()
        let baseline = at.y + font.pointSize * 1.08
        for run in CTLineGetGlyphRuns(line) as? [CTRun] ?? [] {
            let attrs = CTRunGetAttributes(run) as NSDictionary
            let runFont = attrs[kCTFontAttributeName as String].map { $0 as! CTFont } ?? (font as CTFont)
            let n = CTRunGetGlyphCount(run)
            var glyphs = [CGGlyph](repeating: 0, count: n)
            var pos = [CGPoint](repeating: .zero, count: n)
            CTRunGetGlyphs(run, CFRange(location: 0, length: 0), &glyphs)
            CTRunGetPositions(run, CFRange(location: 0, length: 0), &pos)
            for i in 0..<n {
                guard let g = CTFontCreatePathForGlyph(runFont, glyphs[i], nil) else { continue }
                path.addPath(g, transform: CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: at.x + pos[i].x, ty: baseline - pos[i].y))
            }
        }
        return path
    }

    static func gradient(_ colors: [CGColor], _ at: [CGFloat]) -> CGGradient {
        return CGGradient(colorsSpace: space, colors: colors as CFArray, locations: at)!
    }

    // a path filled top to bottom, over box, from one colour to another
    static func fill(_ g: CGContext, _ path: CGPath, _ top: CGColor, _ bottom: CGColor, _ box: CGRect) {
        g.saveGState()
        g.addPath(path)
        g.clip()
        g.drawLinearGradient(gradient([top, bottom], [0, 1]), start: CGPoint(x: 0, y: box.minY),
                             end: CGPoint(x: 0, y: box.maxY), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
        g.restoreGState()
    }

    static func rounded(_ r: CGRect, _ radius: CGFloat) -> CGPath {
        let rad = max(0, min(radius, min(r.width, r.height) / 2))
        return CGPath(roundedRect: r, cornerWidth: rad, cornerHeight: rad, transform: nil)
    }

    // filled, with a 1 pt edge (none when edge is nil)
    static func box(_ g: CGContext, _ r: CGRect, _ radius: CGFloat, _ fill: CGColor, _ edge: CGColor?) {
        let p = rounded(r, radius)
        g.addPath(p)
        g.setFillColor(fill)
        g.fillPath()
        if let edge = edge {
            g.addPath(p)
            g.setStrokeColor(edge)
            g.setLineWidth(1)
            g.strokePath()
        }
    }

    // gold, lighter at the top, with a hairline of light along its upper edge
    static func goldBox(_ g: CGContext, _ r: CGRect, _ radius: CGFloat, _ top: CGColor, _ bottom: CGColor) {
        fill(g, rounded(r, radius), top, bottom, r.insetBy(dx: 0, dy: -1))
        g.saveGState()
        g.clip(to: CGRect(x: r.minX, y: r.minY, width: r.width, height: radius))
        g.addPath(rounded(r.insetBy(dx: 0.5, dy: 0.5), radius - 0.5))
        g.setStrokeColor(rgb(255, 250, 228, 120))
        g.setLineWidth(1)
        g.strokePath()
        g.restoreGState()
    }

    // A soft round glow: color at the middle, fading out to nothing at the
    // edge, eased so it has no visible edge or cone in the middle (the
    // Windows glow's Blend, from the middle out)
    static func glow(_ g: CGContext, _ at: CGPoint, _ rx: CGFloat, _ ry: CGFloat, _ color: CGColor) {
        guard rx > 0 && ry > 0 else { return }
        let a = color.alpha
        let colors = [1, 0.76, 0.46, 0.22, 0.07, 0].map { color.copy(alpha: a * $0) ?? color }
        g.saveGState()
        g.translateBy(x: at.x, y: at.y)
        g.scaleBy(x: 1, y: ry / rx)
        g.drawRadialGradient(gradient(colors, [0, 0.2, 0.4, 0.6, 0.8, 1]), startCenter: .zero, startRadius: 0,
                             endCenter: .zero, endRadius: rx, options: [])
        g.restoreGState()
    }

    // The sky under everything: the gradient, light low on the right like a
    // horizon after dusk, a little violet in the top corner, and dust: many
    // faint stars that stay where they are (the same every time, and the
    // same as on Windows), none behind the glass and text in clear.
    static func night(_ g: CGContext, _ w: CGFloat, _ h: CGFloat, _ clear: [CGRect]) {
        g.drawLinearGradient(gradient([skyTop, skyMid, skyLow], [0, 0.55, 1]), start: CGPoint(x: 0, y: -1),
                             end: CGPoint(x: 0, y: h + 1), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
        glow(g, CGPoint(x: w * 0.8, y: h * 1.04), w * 0.7, h * 0.78, rgb(74, 96, 196, 78))
        glow(g, CGPoint(x: w * 0.02, y: -h * 0.12), w * 0.5, h * 0.62, rgb(120, 84, 200, 40))

        var dust = NetRandom(64)
        let count = Int(Float(140) * Float(w) * Float(h) / (Float(540) * Float(300)))   // as thick as the main window's
        for _ in 0..<count {
            let p = CGPoint(x: dust.nextDouble() * Double(w), y: dust.nextDouble() * Double(h))
            let size = dust.nextDouble(), shine = dust.nextDouble()
            let warm = dust.nextDouble() < 0.15
            let at = CGPoint(x: p.x.rounded(.toNearestOrEven), y: p.y.rounded(.toNearestOrEven))
            // GDI's Rectangle.Contains: the right and bottom edges are outside
            if clear.contains(where: { at.x >= $0.minX && at.x < $0.maxX && at.y >= $0.minY && at.y < $0.maxY }) { continue }
            let r = 0.35 + CGFloat(size * size * size) * 0.75
            let a = 30 + Int(shine * shine * 140)
            g.setFillColor(warm ? rgb(255, 226, 170, a) : rgb(214, 224, 255, a))
            g.fillEllipse(in: CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2))
        }
    }

    // the gold star by a title, glowing
    static func goldStar(_ g: CGContext, _ at: CGPoint, _ outer: CGFloat) {
        let k = outer / 20
        glow(g, at, 46 * k, 46 * k, alpha(70, gold))
        glow(g, at, 26 * k, 26 * k, alpha(60, goldLight))
        let box = CGRect(x: at.x - outer - 1, y: at.y - outer - 1, width: outer * 2 + 2, height: outer * 2)
        star(g, CGPoint(x: at.x, y: at.y + k), outer) { g in
            g.drawLinearGradient(gradient([rgb(255, 240, 176), goldDeep], [0, 1]), start: CGPoint(x: 0, y: box.minY),
                                 end: CGPoint(x: 0, y: box.maxY), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
        }
    }

    // A five-pointed star, its points slightly rounded; paint fills it
    // (clipped to the star)
    static func star(_ g: CGContext, _ at: CGPoint, _ outer: CGFloat, _ paint: (CGContext) -> Void) {
        let path = CGMutablePath()
        for i in 0..<10 {
            let ang = -Double.pi / 2 + Double(i) * Double.pi / 5
            let rad = i % 2 == 0 ? outer : outer * 0.48
            let p = CGPoint(x: at.x + rad * CGFloat(cos(ang)), y: at.y + rad * CGFloat(sin(ang)))
            if i == 0 { path.move(to: p) } else { path.addLine(to: p) }
        }
        path.closeSubpath()
        g.saveGState()
        g.addPath(path)
        g.clip()
        paint(g)
        g.restoreGState()
        g.saveGState()
        g.addPath(path)
        g.setLineWidth(outer * 0.16)
        g.setLineJoin(.round)
        g.replacePathWithStrokedPath()
        g.clip()
        paint(g)
        g.restoreGState()
    }

    // an image the right way up in a flipped (top-down) context
    static func image(_ g: CGContext, _ img: CGImage, _ r: CGRect) {
        g.saveGState()
        g.translateBy(x: 0, y: r.minY + r.maxY)
        g.scaleBy(x: 1, y: -1)
        g.draw(img, in: r)
        g.restoreGState()
    }

    // The app icon: the night sky on the Mac's rounded square, and the gold
    // star. Drawn top-down in a 1024-point square.
    static func appIcon(_ g: CGContext) {
        let body = CGRect(x: 100, y: 100, width: 824, height: 824)
        let shape = rounded(body, 185)
        g.saveGState()
        g.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: rgb(0, 0, 0, 90))
        g.addPath(shape)
        g.setFillColor(skyTop)
        g.fillPath()
        g.restoreGState()
        g.saveGState()
        g.addPath(shape)
        g.clip()
        g.translateBy(x: body.minX, y: body.minY)
        g.scaleBy(x: body.width / 300, y: body.height / 300)
        night(g, 300, 300, [CGRect(x: 60, y: 60, width: 180, height: 180)])
        goldStar(g, CGPoint(x: 150, y: 146), 92)
        g.restoreGState()
        g.addPath(rounded(body.insetBy(dx: 1.5, dy: 1.5), 183.5))
        g.setStrokeColor(white(30))
        g.setLineWidth(3)
        g.strokePath()
    }
}

// .NET Framework's System.Random (Knuth's subtractive generator), so a seed
// gives the star dust Windows draws
struct NetRandom {
    private var seeds = [Int](repeating: 0, count: 56)
    private var inext = 0, inextp = 21
    private static let big = Int(Int32.max), seed = 161803398

    init(_ seed: Int) {
        var mj = NetRandom.seed - abs(seed)
        seeds[55] = mj
        var mk = 1
        for i in 1..<55 {
            let ii = (21 * i) % 55
            seeds[ii] = mk
            mk = mj - mk
            if mk < 0 { mk += NetRandom.big }
            mj = seeds[ii]
        }
        for _ in 1..<5 {
            for i in 1..<56 {
                seeds[i] -= seeds[1 + (i + 30) % 55]
                if seeds[i] < 0 { seeds[i] += NetRandom.big }
            }
        }
    }

    mutating func nextDouble() -> Double {
        inext += 1
        if inext >= 56 { inext = 1 }
        inextp += 1
        if inextp >= 56 { inextp = 1 }
        var r = seeds[inext] - seeds[inextp]
        if r == NetRandom.big { r -= 1 }
        if r < 0 { r += NetRandom.big }
        seeds[inext] = r
        return Double(r) * (1.0 / Double(NetRandom.big))
    }
}

// Background music: music.mp3 looped while the window is open. Mute is
// remembered in ~/Library/Application Support/SM64DSEnhancedBuilder/muted.
enum Music {
    static var player: AVAudioPlayer?
    static var donePlayer: AVAudioPlayer?

    static var mutedFlag: URL { return Look.supportFolder.appendingPathComponent("muted") }

    static var muted: Bool {
        get { return FileManager.default.fileExists(atPath: mutedFlag.path) }
        set {
            if newValue {
                try? FileManager.default.createDirectory(at: Look.supportFolder, withIntermediateDirectories: true)
                FileManager.default.createFile(atPath: mutedFlag.path, contents: Data())
            } else {
                try? FileManager.default.removeItem(at: mutedFlag)
            }
            player?.volume = newValue ? 0 : 1
        }
    }

    static func start() {
        guard let url = Bundle.main.url(forResource: "music", withExtension: "mp3"),
              let p = try? AVAudioPlayer(contentsOf: url) else { return }
        p.numberOfLoops = -1
        p.volume = muted ? 0 : 1
        p.play()
        player = p
    }

    static func stop() {
        player?.stop()
        player = nil
    }

    // The finished build's sound (done.mp3), once, in place of the music.
    // Nothing when muted.
    static func done() {
        stop()
        if muted { return }
        guard let url = Bundle.main.url(forResource: "done", withExtension: "mp3"),
              let p = try? AVAudioPlayer(contentsOf: url) else { return }
        p.volume = 0.58
        p.play()
        donePlayer = p
    }

    static func close() {
        stop()
        donePlayer?.stop()
        donePlayer = nil
    }
}

// What the window's custom controls share: drawn top-down, lit on hover,
// pressed while the mouse is down on them, clicked on release inside. They
// take keyboard focus only with keyboard navigation on (System Settings >
// Keyboard), and show it only when the keyboard gave it, as Windows shows
// focus cues; Space clicks the focused one. VoiceOver sees each as a button.
class Control: NSView {
    var text = "" { didSet { needsDisplay = true } }
    var isEnabled = true { didSet { if !isEnabled { down = false }; needsDisplay = true } }
    var onClick: (() -> Void)?
    var hover = false, down = false
    private var keyboardFocus = false

    override var isFlipped: Bool { return true }
    override var acceptsFirstResponder: Bool { return isEnabled && NSApp.isFullKeyboardAccessEnabled }

    override func becomeFirstResponder() -> Bool {
        keyboardFocus = NSApp.currentEvent?.type == .keyDown
        needsDisplay = true
        return true
    }

    override func resignFirstResponder() -> Bool {
        keyboardFocus = false
        needsDisplay = true
        return true
    }

    var focusShown: Bool { return keyboardFocus && window?.firstResponder === self && window?.isKeyWindow == true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for t in trackingAreas { removeTrackingArea(t) }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseEntered(with event: NSEvent) { hover = true; needsDisplay = true }
    override func mouseExited(with event: NSEvent) { hover = false; down = false; needsDisplay = true }

    override func mouseDown(with event: NSEvent) {
        guard isEnabled else { return }
        down = true
        needsDisplay = true
    }

    override func mouseDragged(with event: NSEvent) {
        guard isEnabled else { return }
        let inside = isInside(event)
        if inside != down { down = inside; needsDisplay = true }
    }

    override func mouseUp(with event: NSEvent) {
        let fire = down && isEnabled && isInside(event)
        down = false
        needsDisplay = true
        if fire { click() }
    }

    func isInside(_ e: NSEvent) -> Bool { return bounds.contains(convert(e.locationInWindow, from: nil)) }

    func click() { onClick?() }

    override func keyDown(with event: NSEvent) {
        if isEnabled && event.charactersIgnoringModifiers == " " { click() } else { super.keyDown(with: event) }
    }

    override func isAccessibilityElement() -> Bool { return true }
    override func accessibilityRole() -> NSAccessibility.Role? { return .button }
    override func accessibilityLabel() -> String? { return text }
    override func isAccessibilityEnabled() -> Bool { return isEnabled }
    override func accessibilityPerformPress() -> Bool {
        guard isEnabled else { return false }
        click()
        return true
    }
}

// A button: primary (gold) or quiet (glass). Sliders: an icon of three
// sliders in place of the text (the build settings; text is its name).
final class FlatButton: Control {
    var primary = false, sliders = false

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        let ink: CGColor
        if !isEnabled {
            // still the gold button, dimmed
            if primary { Look.box(g, r, 10, Look.alpha(30, Look.gold), Look.alpha(64, Look.gold)) }
            else { Look.box(g, r, 10, Look.white(8), Look.white(20)) }
            ink = primary ? Look.rgb(150, 128, 82) : Look.faint
        } else if primary {
            if down { Look.goldBox(g, r, 10, Look.gold, Look.goldDeep) }
            else if hover { Look.goldBox(g, r, 10, Look.rgb(255, 236, 166), Look.rgb(250, 186, 66)) }
            else { Look.goldBox(g, r, 10, Look.goldLight, Look.goldDeep) }
            ink = Look.goldInk
        } else {
            Look.box(g, r, 10, Look.white(down ? 8 : hover ? 20 : 12), Look.white(hover ? 64 : 36))
            ink = Look.text
        }
        if sliders { FlatButton.drawSliders(g, CGPoint(x: r.midX, y: r.midY), hover && isEnabled ? Look.gold : ink) }
        else { Look.write(text, Look.strong(14), r, ink, .center) }
        if focusShown {
            g.addPath(Look.rounded(r.insetBy(dx: 3, dy: 3), 7))
            g.setStrokeColor(primary && isEnabled ? Look.alpha(150, Look.goldInk) : Look.white(150))
            g.setLineWidth(1.2)
            g.strokePath()
        }
    }

    // three rails, each with its knob at a different place
    static func drawSliders(_ g: CGContext, _ c: CGPoint, _ ink: CGColor) {
        let half: CGFloat = 9
        let knobs: [CGFloat] = [0.3, 0.72, 0.46]
        g.setStrokeColor(Look.alpha(170, ink))
        g.setLineWidth(1.5)
        g.setLineCap(.round)
        g.setFillColor(ink)
        for i in 0..<3 {
            let y = c.y + CGFloat(i - 1) * 6, x = c.x - half + knobs[i] * half * 2
            g.move(to: CGPoint(x: c.x - half, y: y))
            g.addLine(to: CGPoint(x: c.x + half, y: y))
            g.strokePath()
            g.fillEllipse(in: CGRect(x: x - 2.6, y: y - 2.6, width: 5.2, height: 5.2))
        }
    }
}

// The music's on/off: a speaker, struck through when muted.
final class MuteButton: Control {
    override var acceptsFirstResponder: Bool { return false }

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let w = bounds.width, h = bounds.height
        if hover { Look.box(g, CGRect(x: 0.5, y: 0.5, width: w - 1, height: h - 1), (w - 1) / 2, Look.white(16), nil) }
        let x = w / 2 - 8.5, y = h / 2
        let ink = hover ? Look.text : Look.muted
        g.setFillColor(ink)
        g.setStrokeColor(ink)
        g.setLineWidth(1.5)
        g.setLineCap(.round)
        g.setLineJoin(.round)
        let speaker = [CGPoint(x: x, y: y - 3), CGPoint(x: x + 3.5, y: y - 3), CGPoint(x: x + 8, y: y - 7),
                       CGPoint(x: x + 8, y: y + 7), CGPoint(x: x + 3.5, y: y + 3), CGPoint(x: x, y: y + 3)]
        g.addLines(between: speaker)
        g.closePath()
        g.drawPath(using: .fillStroke)
        if Music.muted {
            g.strokeLineSegments(between: [CGPoint(x: x + 12, y: y - 3.5), CGPoint(x: x + 19, y: y + 3.5),
                                           CGPoint(x: x + 19, y: y - 3.5), CGPoint(x: x + 12, y: y + 3.5)])
        } else {
            MuteButton.arc(g, CGRect(x: x + 6, y: y - 4.5, width: 8, height: 9))
            MuteButton.arc(g, CGRect(x: x + 5, y: y - 8.5, width: 14, height: 17))
        }
    }

    // GDI's DrawArc(rect, -50, 100): the right side of the ellipse in rect
    static func arc(_ g: CGContext, _ r: CGRect) {
        g.saveGState()
        g.translateBy(x: r.midX, y: r.midY)
        g.scaleBy(x: r.width / 2, y: r.height / 2)
        g.addArc(center: .zero, radius: 1, startAngle: -50 * .pi / 180, endAngle: 50 * .pi / 180, clockwise: false)
        g.restoreGState()
        g.strokePath()
    }

    override func click() {
        Music.muted = !Music.muted
        needsDisplay = true
        super.click()
    }

    override func accessibilityRole() -> NSAccessibility.Role? { return .checkBox }
    override func accessibilityLabel() -> String? { return "Music" }
    override func accessibilityValue() -> Any? { return Music.muted ? 0 : 1 }
}

// The dump tile: click it or drop a file on it. Empty, it shows a game card
// and what to choose; once chosen, the dump's DS icon, its file name and a
// line of status, with a progress bar under it while building.
final class RomTile: Control {
    var path: String?
    var icon: CGImage?
    var status: String?        // the line under the file name (nil: ready)
    var ready = "Ready. The .cia will be saved next to it."
    var saved = false          // status says where the .cia went
    var progress = -1          // 0..100 while building, else -1
    var fileDropped: ((String) -> Void)?
    private var drag = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
    }

    required init?(coder: NSCoder) { fatalError() }

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let w = bounds.width, h = bounds.height
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        let lit = hover && isEnabled
        if drag {
            Look.box(g, r, 14, Look.alpha(22, Look.gold), nil)
            g.addPath(Look.rounded(r.insetBy(dx: 0.25, dy: 0.25), 14))
            g.setStrokeColor(Look.alpha(220, Look.gold))
            g.setLineWidth(1.5)
            g.strokePath()
        } else if path == nil {
            // empty: a dashed outline over a faint glass
            Look.box(g, r, 14, Look.white(lit ? 12 : 6), nil)
            g.saveGState()
            g.addPath(Look.rounded(r, 14))
            g.setStrokeColor(focusShown ? Look.alpha(200, Look.gold) : Look.white(lit ? 96 : 54))
            g.setLineWidth(1.2)
            g.setLineDash(phase: 0, lengths: [4 * 1.2, 3.5 * 1.2])   // GDI's dashes are in pen widths
            g.strokePath()
            g.restoreGState()
        } else {
            Look.box(g, r, 14, Look.white(lit ? 14 : 9),
                     focusShown ? Look.alpha(200, Look.gold) : Look.white(lit ? 60 : 34))
        }

        // the icon's square: the DS icon at twice its size, or a game card
        let slot = CGRect(x: 16, y: (h - 64) / 2, width: 64, height: 64)
        if let icon = icon {
            g.saveGState()
            g.addPath(Look.rounded(slot, 10))
            g.clip()
            g.setFillColor(Look.white(235))
            g.fill(slot)
            g.interpolationQuality = .none
            Look.image(g, icon, slot)
            g.restoreGState()
        } else {
            Look.box(g, slot, 10, Look.white(lit || drag ? 14 : 8), Look.white(lit || drag ? 44 : 26))
            RomTile.card(g, CGPoint(x: slot.midX, y: slot.midY), lit || drag ? Look.gold : Look.muted)
        }

        let left = slot.maxX + 18, right = w - 22
        let title: String, line: String
        var lineInk = Look.muted
        if drag {
            title = "Drop to choose this dump"
            line = "Super Mario 64 DS, European cartridge (.nds)"
        } else if let path = path {
            title = (path as NSString).lastPathComponent
            line = status ?? ready
            if saved { lineInk = Look.gold }
        } else {
            title = "Choose your Super Mario 64 DS dump"
            line = "The European cartridge (.nds). Click here, or drop the file."
        }
        Look.write(title, Look.strong(15), CGRect(x: left, y: h / 2 - 24, width: right - left, height: 22), Look.text)
        var lineRight = right
        if progress >= 0 {
            lineRight -= 40
            Look.write("\(progress)%", Look.strong(13), CGRect(x: lineRight, y: h / 2 - 1, width: 40, height: 18), Look.text, .right)
        }
        Look.write(line, Look.face(13), CGRect(x: left, y: h / 2 - 1, width: lineRight - left, height: 18), lineInk)
        if progress >= 0 {
            let track = CGRect(x: left, y: h / 2 + 23, width: right - left, height: 4)
            Look.box(g, track, 2, Look.white(20), nil)
            if progress > 0 {
                let fill = CGRect(x: track.minX, y: track.minY, width: max(4, track.width * CGFloat(progress) / 100), height: track.height)
                g.saveGState()
                g.addPath(Look.rounded(fill, 2))
                g.clip()
                g.drawLinearGradient(Look.gradient([Look.goldDeep, Look.goldLight], [0, 1]),
                                     start: CGPoint(x: track.minX - 1, y: 0), end: CGPoint(x: track.maxX + 1, y: 0),
                                     options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
                g.restoreGState()
            }
        }
    }

    // a DS game card in outline: the cut corner, the ridges along the top
    // and the label
    static func card(_ g: CGContext, _ c: CGPoint, _ ink: CGColor) {
        let w: CGFloat = 24, h: CGFloat = 28, x = c.x - w / 2, y = c.y - h / 2
        g.setStrokeColor(ink)
        g.setLineWidth(1.5)
        g.setLineJoin(.round)
        g.setLineCap(.round)
        g.addLines(between: [CGPoint(x: x + 2, y: y), CGPoint(x: x + w - 6, y: y), CGPoint(x: x + w, y: y + 6),
                             CGPoint(x: x + w, y: y + h - 2), CGPoint(x: x + w - 2, y: y + h),
                             CGPoint(x: x + 2, y: y + h), CGPoint(x: x, y: y + h - 2), CGPoint(x: x, y: y + 2)])
        g.closePath()
        g.strokePath()
        for i in 0..<4 {
            let rx = x + 5 + CGFloat(i) * 3.5
            g.strokeLineSegments(between: [CGPoint(x: rx, y: y + 3.5), CGPoint(x: rx, y: y + 6.5)])
        }
        g.addPath(Look.rounded(CGRect(x: x + 4.5, y: y + 11, width: w - 9, height: h - 15.5), 2))
        g.strokePath()
    }

    override func accessibilityLabel() -> String? {
        guard let path = path else { return "Choose your Super Mario 64 DS dump" }
        return (path as NSString).lastPathComponent + ". " + (status ?? ready)
    }

    func dropped(_ info: NSDraggingInfo) -> String? {
        let urls = info.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                      options: [.urlReadingFileURLsOnly: true]) as? [URL]
        return urls?.count == 1 ? urls![0].path : nil
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        guard isEnabled, dropped(sender) != nil else { return [] }
        drag = true
        needsDisplay = true
        return .copy
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation { return drag ? .copy : [] }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        drag = false
        needsDisplay = true
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        drag = false
        needsDisplay = true
        guard isEnabled, let f = dropped(sender) else { return false }
        fileDropped?(f)
        return true
    }

    override func concludeDragOperation(_ sender: NSDraggingInfo?) {
        drag = false
        needsDisplay = true
    }

    func showDragForSnapshot(_ on: Bool) { drag = on }
}

// A view that paints a prepared sky (drawn once, for whatever display the
// window is on) under its controls
class SkyView: NSView {
    let sky: NSImage

    init(frame: NSRect, sky: NSImage) {
        self.sky = sky
        super.init(frame: frame)
    }

    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { return true }
    override var isOpaque: Bool { return true }

    override func draw(_ dirtyRect: NSRect) {
        sky.draw(in: bounds, from: .zero, operation: .copy, fraction: 1, respectFlipped: true, hints: nil)
    }
}

// The main window's sky, with its twinkling stars and the version. A star
// glows and fades over its cycle, then shows up somewhere else. The brighter
// ones have four rays.
final class StarrySky: SkyView {
    final class Star { var x: CGFloat = 0, y: CGFloat = 0, r: CGFloat = 0, phase: CGFloat = 0, speed: CGFloat = 0; var rays = false; var ink = Look.text }
    var stars: [Star] = []
    var version = ""

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        for s in stars { StarrySky.drawStar(g, s) }
        if !version.isEmpty {
            Look.write(version, Look.face(12), CGRect(x: BuilderWindow.pad, y: 240, width: 160, height: 20), Look.faint)
        }
    }

    // how far a star's glow and rays go from its middle
    static func reach(_ s: Star) -> CGFloat { return s.r * (s.rays ? 7 : 4.5) + 1 }

    static func box(_ s: Star) -> CGRect {
        let reach = StarrySky.reach(s) + 1
        return CGRect(x: floor(s.x - reach), y: floor(s.y - reach), width: ceil(reach * 2) + 1, height: ceil(reach * 2) + 1)
    }

    static func drawStar(_ g: CGContext, _ s: Star) {
        let a = (1 - cos(s.phase)) / 2
        if a < 0.02 { return }
        Look.glow(g, CGPoint(x: s.x, y: s.y), s.r * 4.5, s.r * 4.5, Look.alpha(Int(a * 110), s.ink))
        if s.rays {
            // four thin rays, tapering to nothing
            let len = s.r * 7 * (0.6 + 0.4 * a), half = s.r * 0.32
            g.setFillColor(Look.alpha(Int(a * 170), s.ink))
            g.addLines(between: [CGPoint(x: s.x - len, y: s.y), CGPoint(x: s.x, y: s.y - half),
                                 CGPoint(x: s.x + len, y: s.y), CGPoint(x: s.x, y: s.y + half)])
            g.closePath()
            g.fillPath()
            g.addLines(between: [CGPoint(x: s.x, y: s.y - len), CGPoint(x: s.x + half, y: s.y),
                                 CGPoint(x: s.x, y: s.y + len), CGPoint(x: s.x - half, y: s.y)])
            g.closePath()
            g.fillPath()
        }
        let core = s.r * (0.75 + 0.25 * a)
        g.setFillColor(Look.white(Int(a * 255)))
        g.fillEllipse(in: CGRect(x: s.x - core * 0.6, y: s.y - core * 0.6, width: core * 1.2, height: core * 1.2))
        g.setFillColor(Look.alpha(Int(a * 200), s.ink))
        g.fillEllipse(in: CGRect(x: s.x - core, y: s.y - core, width: core * 2, height: core * 2))
    }
}

// The window: the star and title, music button, dump tile, Build. The .cia
// is saved next to the dump; the build's log lines are only kept for error
// messages. When a build finishes the music stops and done.mp3 plays.
final class BuilderWindow: NSWindow {
    static let pad: CGFloat = 32, w: CGFloat = 540, h: CGFloat = 300
    static let mark = CGPoint(x: pad + 20, y: 52)   // the gold star by the title
    // the tile, the buttons' corner (the settings, and Build or Show in Finder
    // once built) and the version: no dust behind their glass or text
    static let tileBox = CGRect(x: pad, y: 108, width: w - pad * 2, height: 96)
    static let buttonBox = CGRect(x: w - pad - 206, y: 226, width: 208, height: 48)
    static let versionBox = CGRect(x: pad - 2, y: 240, width: 110, height: 20)
    // where no twinkling star goes: the star and title, the music button, the
    // tile, the button and the version
    static let taken = [CGRect(x: 14, y: 14, width: 300, height: 76), CGRect(x: 476, y: 32, width: 36, height: 36),
                        tileBox, buttonBox, versionBox]

    let tile = RomTile()
    let mute = MuteButton()
    let buildButton = FlatButton(), showButton = FlatButton(), settingsButton = FlatButton()
    let options = BuildOptions.load()
    let sky: StarrySky
    var built: String?
    var busy = false
    var lines = ""
    var clock: Timer?

    init() {
        let size = NSRect(x: 0, y: 0, width: BuilderWindow.w, height: BuilderWindow.h)
        sky = StarrySky(frame: size, sky: BuilderWindow.drawSky())
        super.init(contentRect: size, styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        title = Program.title
        Look.dress(self)
        contentView = sky
        let label = Payload.label()
        sky.version = label.hasPrefix("build ") && label != "build unknown" ? "Version " + label.dropFirst(6) : ""

        mute.frame = CGRect(x: BuilderWindow.w - BuilderWindow.pad - 28 + 4, y: 36, width: 32, height: 32)

        let t = BuilderWindow.tileBox
        tile.frame = t
        tile.onClick = { [unowned self] in self.pickRom() }
        tile.fileDropped = { [unowned self] in self.setRom($0) }

        BuilderWindow.button(buildButton, "Build", true, BuilderWindow.w - BuilderWindow.pad - 132, 228, 132)
        buildButton.isEnabled = false
        buildButton.onClick = { [unowned self] in self.startBuild() }
        BuilderWindow.button(showButton, "Show in Finder", true, BuilderWindow.w - BuilderWindow.pad - 150, 228, 150)
        showButton.isHidden = true
        showButton.onClick = { [unowned self] in self.showBuilt() }
        BuilderWindow.button(settingsButton, "Build settings", false, 0, 228, 44)
        settingsButton.sliders = true
        settingsButton.onClick = { [unowned self] in self.openSettings() }
        placeSettings()
        tile.ready = readyLine()

        for v in [mute, tile, settingsButton, buildButton, showButton] as [NSView] { sky.addSubview(v) }
        for _ in 0..<12 {
            let s = StarrySky.Star()
            place(s)
            s.phase = CGFloat.random(in: 0..<(.pi * 2))   // not all in step
            sky.stars.append(s)
        }
        center()
        setFrameAutosaveName("SM64DSEnhancedBuilder")
    }

    // Everything that doesn't move: the sky's gradient and glows, the star
    // dust, the gold star and the title.
    static func drawSky() -> NSImage {
        return NSImage(size: NSSize(width: w, height: h), flipped: true) { _ in
            guard let g = NSGraphicsContext.current?.cgContext else { return false }
            Look.night(g, w, h, [tileBox, buttonBox, versionBox])
            Look.goldStar(g, mark, 20)
            let t = Look.lettering("Super Mario 64 DS", Look.display(28), 0, CGPoint(x: pad + 52, y: 26))
            Look.fill(g, t, Look.rgb(255, 255, 255), Look.rgb(200, 210, 240), t.boundingBoxOfPath.insetBy(dx: 0, dy: -1))
            g.addPath(Look.lettering("ENHANCED", Look.display(11.5), 3.2, CGPoint(x: pad + 53.5, y: 62)))
            g.setFillColor(Look.gold)
            g.fillPath()
            return true
        }
    }

    static func button(_ b: FlatButton, _ text: String, _ primary: Bool, _ x: CGFloat, _ y: CGFloat, _ w: CGFloat) {
        b.text = text
        b.primary = primary
        b.frame = CGRect(x: x, y: y, width: w, height: 44)
    }

    // the music starts with the window, and the stars with the clock
    func start() {
        makeKeyAndOrderFront(nil)
        Music.start()
        mute.needsDisplay = true
        let timer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in self?.twinkle() }
        RunLoop.main.add(timer, forMode: .common)
        clock = timer
    }

    func place(_ s: StarrySky.Star) {
        s.r = 0.9 + CGFloat.random(in: 0..<0.9)
        s.rays = Double.random(in: 0..<1) < 0.35
        if s.rays { s.r += 0.3 }
        s.ink = Double.random(in: 0..<1) < 0.3 ? Look.rgb(255, 222, 140) : Look.rgb(226, 234, 255)
        s.speed = 0.05 + CGFloat.random(in: 0..<0.025)   // a cycle of 4 to 6 s
        let reach = StarrySky.reach(s)
        for _ in 0..<200 {
            let p = CGPoint(x: CGFloat(Int.random(in: 10..<Int(BuilderWindow.w) - 10)),
                            y: CGFloat(Int.random(in: 10..<Int(BuilderWindow.h) - 10)))
            let box = CGRect(x: p.x - reach, y: p.y - reach, width: reach * 2, height: reach * 2)
            if BuilderWindow.taken.contains(where: { box.intersects($0) }) { continue }
            if sky.stars.contains(where: { $0 !== s && abs($0.x - p.x) + abs($0.y - p.y) < 56 }) { continue }
            s.x = p.x
            s.y = p.y
            break
        }
    }

    func twinkle() {
        guard occlusionState.contains(.visible) else { return }
        for s in sky.stars {
            sky.setNeedsDisplay(StarrySky.box(s))
            s.phase += s.speed
            if s.phase < .pi * 2 { continue }
            s.phase -= .pi * 2   // dark here: move it
            place(s)
            sky.setNeedsDisplay(StarrySky.box(s))
        }
    }

    func setRom(_ path: String) {
        if busy { return }
        let exists = FileManager.default.fileExists(atPath: path)
        tile.path = exists ? path : nil
        tile.icon = exists ? (try? Art.dsIcon(path))?.cgImage() : nil
        tile.progress = -1
        tile.status = nil
        tile.saved = false
        tile.needsDisplay = true
        built = nil
        showButton.isHidden = true
        buildButton.isHidden = false
        buildButton.text = "Build"
        buildButton.isEnabled = tile.path != nil
        placeSettings()
    }

    // the settings button, just left of Build or Show in Finder
    func placeSettings() {
        let beside = showButton.isHidden ? buildButton : showButton
        settingsButton.frame.origin.x = beside.frame.minX - 54
    }

    func readyLine() -> String {
        return "Ready for " + (options.old ? "an Old 3DS" : "a New 3DS") + ". The .cia will be saved next to it."
    }

    // The settings window; a change after a build brings Build back, so the
    // new choices can be built.
    func openSettings() {
        if busy { return }
        let d = SettingsWindow(options)
        d.setFrameOrigin(NSPoint(x: frame.midX - d.frame.width / 2, y: frame.maxY - d.frame.height - 24))
        if let screen = screen?.visibleFrame {
            var f = d.frame
            f.origin.y = min(max(f.origin.y, screen.minY), screen.maxY - f.height)
            d.setFrame(f, display: false)
        }
        let response = NSApp.runModal(for: d)
        d.orderOut(nil)
        if response != .OK || d.chosen.same(options) { return }
        options.take(d.chosen)
        options.save()
        tile.ready = readyLine()
        if built != nil, let p = tile.path { setRom(p) }
        tile.needsDisplay = true
    }

    func pickRom() {
        if busy { return }
        let d = NSOpenPanel()
        d.title = "Your Super Mario 64 DS dump"
        d.message = "Choose your dump of the European Super Mario 64 DS cartridge (.nds)."
        d.canChooseDirectories = false
        d.allowsMultipleSelection = false
        d.beginSheetModal(for: self) { [weak self] r in
            if r == .OK, let url = d.url { self?.setRom(url.path) }
        }
    }

    func showBuilt() {
        if let built = built { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: built)]) }
    }

    func startBuild() {
        guard !busy, let rom = tile.path, FileManager.default.fileExists(atPath: rom) else { return }
        let full = URL(fileURLWithPath: rom).standardizedFileURL
        let output = full.deletingLastPathComponent().appendingPathComponent(Program.title + ".cia").path
        setBusy(true)
        lines = ""
        let chosen = options.copy()
        Thread.detachNewThread { [weak self] in self?.runBuild(full.path, output, chosen) }
    }

    func runBuild(_ rom: String, _ output: String, _ chosen: BuildOptions) {
        var error: String? = nil
        do {
            let build = Build(rom: rom, output: output, log: { [weak self] in self?.log($0) }) { [weak self] pct, what in
                DispatchQueue.main.async { self?.progress(pct, what) }
            }
            build.options = chosen
            try build.run()
        } catch let e as BuildError {
            error = e.message
        } catch let e {
            error = "Something unexpected went wrong.\n\n" + e.localizedDescription
        }
        DispatchQueue.main.async { [weak self] in self?.finished(output, error) }
    }

    func finished(_ output: String, _ error: String?) {
        setBusy(false)
        tile.progress = -1
        if let error = error {
            tile.status = "The build didn't finish. Press Build to try again."
            tile.needsDisplay = true
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "The build didn't finish"
            alert.informativeText = error
            alert.beginSheetModal(for: self)
            return
        }
        showSaved(output)
        Music.done()
    }

    func showSaved(_ output: String) {
        built = output
        tile.status = "Saved " + (output as NSString).lastPathComponent + " next to it."
        tile.saved = true
        // Show in Finder takes Build's place until another dump (or another
        // setting) is chosen
        buildButton.isHidden = true
        showButton.isHidden = false
        placeSettings()
        tile.needsDisplay = true
    }

    func setBusy(_ busy: Bool) {
        self.busy = busy
        tile.isEnabled = !busy
        buildButton.isEnabled = !busy
        settingsButton.isEnabled = !busy
        if busy {
            showButton.isHidden = true
            buildButton.isHidden = false
            buildButton.text = "Building…"
            tile.progress = 0
            tile.status = "Starting…"
            tile.saved = false
            tile.needsDisplay = true
        } else {
            buildButton.text = "Build"
        }
        placeSettings()
    }

    func log(_ line: String) {
        DispatchQueue.main.async { [weak self] in self?.lines += line + "\n" }
    }

    func progress(_ percent: Int, _ what: String?) {
        guard busy else { return }
        tile.progress = max(0, min(100, percent))
        if let what = what {
            var s = what
            while s.hasSuffix(".") { s.removeLast() }
            tile.status = s
        }
        tile.needsDisplay = true
    }

    // Return builds, as the default button would
    override func keyDown(with event: NSEvent) {
        if (event.keyCode == 36 || event.keyCode == 76) && !buildButton.isHidden && buildButton.isEnabled { startBuild() }
        else { super.keyDown(with: event) }
    }

    // the menu bar's commands
    @objc func chooseDump(_ sender: Any?) { pickRom() }
    @objc func build(_ sender: Any?) { startBuild() }
    @objc func showInFinder(_ sender: Any?) { showBuilt() }
    @objc func buildSettings(_ sender: Any?) { openSettings() }

    override func validateMenuItem(_ item: NSMenuItem) -> Bool {
        switch item.action {
        case #selector(chooseDump(_:)), #selector(buildSettings(_:)): return !busy
        case #selector(build(_:)): return !busy && tile.path != nil
        case #selector(showInFinder(_:)): return built != nil
        default: return super.validateMenuItem(item)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: BuilderWindow?
    var pending: String?   // a dump opened (from the Finder) before the window was there

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.mainMenu = AppDelegate.menu()
        let w = BuilderWindow()
        window = w
        w.start()
        if let p = pending { w.setRom(p) }
        NSApp.activate(ignoringOtherApps: true)
    }

    // a dump opened with the app, or dropped on its Dock icon
    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first(where: { $0.isFileURL }) else { return }
        if let w = window { w.setRom(url.path) } else { pending = url.path }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { return true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let w = window, w.busy else { return .terminateNow }
        let alert = NSAlert()
        alert.messageText = "A build is still running"
        alert.informativeText = "If you quit now, the .cia won't be made."
        alert.addButton(withTitle: "Keep Building")
        alert.addButton(withTitle: "Quit")
        return alert.runModal() == .alertSecondButtonReturn ? .terminateNow : .terminateCancel
    }

    func applicationWillTerminate(_ notification: Notification) { Music.close() }

    @objc func about(_ sender: Any?) {
        let credits = NSAttributedString(string:
            "Built on tangosdev's Super Mario 64 DS decompilation and PC port. It also uses makerom, " +
            "bannertool, libctru, citro3d and SIMDe; their licences are in THIRD-PARTY.txt in the app.\n\n" +
            "Super Mario 64 DS is Nintendo's. This is a fan project and isn't affiliated with Nintendo.",
            attributes: [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.labelColor])
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
        NSApp.activate(ignoringOtherApps: true)
    }

    static func menu() -> NSMenu {
        let bar = NSMenu()
        let name = "SM64DS Enhanced Builder"
        func sub(_ title: String, _ items: [NSMenuItem]) -> NSMenu {
            let m = NSMenu(title: title)
            for i in items { m.addItem(i) }
            let holder = NSMenuItem()
            holder.submenu = m
            bar.addItem(holder)
            return m
        }
        func item(_ title: String, _ action: Selector?, _ key: String = "", _ mods: NSEvent.ModifierFlags = .command) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
            i.keyEquivalentModifierMask = mods
            return i
        }
        _ = sub(name, [
            item("About " + name, #selector(AppDelegate.about(_:))),
            .separator(),
            item("Build Settings…", #selector(BuilderWindow.buildSettings(_:)), ","),
            .separator(),
            item("Hide " + name, #selector(NSApplication.hide(_:)), "h"),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option]),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:))),
            .separator(),
            item("Quit " + name, #selector(NSApplication.terminate(_:)), "q")])
        _ = sub("File", [
            item("Choose Dump…", #selector(BuilderWindow.chooseDump(_:)), "o"),
            item("Build", #selector(BuilderWindow.build(_:)), "b"),
            item("Show in Finder", #selector(BuilderWindow.showInFinder(_:)), "r"),
            .separator(),
            item("Close", #selector(NSWindow.performClose(_:)), "w")])
        let windows = sub("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
            .separator(),
            item("Bring All to Front", #selector(NSApplication.arrangeInFront(_:)))])
        NSApp.windowsMenu = windows
        return bar
    }
}

// The payload (package.py puts it in the app): the port, the recipe and
// manifest, the RSF, the banner's template and recipe, banner.wav and
// LABEL.txt, in Contents/Resources/payload; makerom and bannertool in
// Contents/Helpers.
enum Payload {
    static var dir: String { return (Bundle.main.resourcePath ?? ".") + "/payload" }

    static func open() throws -> String {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: dir, isDirectory: &isDir), isDir.boolValue else {
            throw BuildError("This builder is damaged (its payload is missing). Download it again.")
        }
        return dir
    }

    static func helper(_ name: String) throws -> String {
        let path = Bundle.main.bundlePath + "/Contents/Helpers/" + name
        guard FileManager.default.isExecutableFile(atPath: path) else {
            throw BuildError("This builder is damaged (its " + name + " is missing). Download it again.")
        }
        return path
    }

    static func label() -> String {
        guard let s = try? String(contentsOfFile: dir + "/LABEL.txt", encoding: .utf8) else { return "build unknown" }
        return "build " + s.trimmingCharacters(in: .whitespacesAndNewlines)
    }
}

final class Build {
    let rom: String, output: String, tmp: String
    let log: (String) -> Void
    let progress: (Int, String?) -> Void
    var keep = false
    var options = BuildOptions()

    init(rom: String, output: String, log: @escaping (String) -> Void, progress: @escaping (Int, String?) -> Void) {
        self.rom = URL(fileURLWithPath: rom).standardizedFileURL.path
        self.output = URL(fileURLWithPath: output).standardizedFileURL.path
        self.log = log
        self.progress = progress
        tmp = (NSTemporaryDirectory() as NSString).appendingPathComponent(
            "SM64DSEnhancedBuilder-" + UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased())
    }

    func run() throws {
        let header = try readHeader()
        defer {
            if keep { log("Kept the work folder: " + tmp) }
            else { try? FileManager.default.removeItem(atPath: tmp) }
        }
        do {
            try steps(header)
        } catch let e as BuildError {
            throw e
        } catch let e as NSError where Build.refused(e) {
            throw BuildError("macOS refused access to a file: " + e.localizedDescription + " If macOS asked whether " +
                             "the builder may use a folder and you said no, allow it in System Settings > Privacy & " +
                             "Security > Files and Folders, then build again.")
        } catch let e as NSError where e.domain == NSCocoaErrorDomain || e.domain == NSPOSIXErrorDomain {
            throw BuildError("A file could not be read or written: " + e.localizedDescription)
        }
    }

    static func refused(_ e: NSError) -> Bool {
        if e.domain == NSCocoaErrorDomain { return e.code == NSFileReadNoPermissionError || e.code == NSFileWriteNoPermissionError }
        if e.domain == NSPOSIXErrorDomain { return e.code == Int(EACCES) || e.code == Int(EPERM) }
        return false
    }

    func steps(_ header: [UInt8]) throws {
        let fm = FileManager.default
        func mkdir(_ p: String) throws { try fm.createDirectory(atPath: p, withIntermediateDirectories: true) }
        try mkdir(tmp)
        progress(1, "Reading your dump...")
        let kit = try Payload.open()
        func k(_ name: String) -> String { return kit + "/" + name }

        // 1. the game data, out of the dump (the PC port's extractor, Extract.swift)
        let work = tmp + "/work"
        try mkdir(work)
        log("Extracting the game data from your dump...")
        try Extract(rom: rom, destination: work, recipe: k("romdata.recipe.tsv"), manifest: k("romdata.manifest"),
                    step: { [log] s in if !s.trimmingCharacters(in: .whitespaces).isEmpty { log("  " + s) } },
                    progress: { [progress] pct, what in progress(2 + pct * 68 / 100, what) }).run()

        // 2. the RomFS
        progress(72, "Packing the game files...")
        let romfs = tmp + "/romfs"
        let assets = romfs + "/build/assets"
        try mkdir(assets)
        for name in ["files.tsv", "handles.tsv", "nitrofs.tsv", "nitrofs_fat.bin", "nitrofs_fnt.bin",
                     "nitrofs_ovt9.bin", "romdata.bin", "romdata.manifest"] {
            let from = work + "/build/assets/" + name
            if !fm.fileExists(atPath: from) {
                throw BuildError("The extraction did not produce " + name + ". Try again; if it happens again, " +
                                 "the dump may be damaged.")
            }
            try fm.copyItem(atPath: from, toPath: assets + "/" + name)
        }
        let dsd = romfs + "/extracted/dsd"
        try mkdir(dsd)
        // hal/nitrofs_boot.cpp reads normal_cmd_setting (header +0x60) out of this file
        try Bytes.write(Array(("title: " + Bytes.ascii(header, 0, 12).trimmingTrailingNuls() + "\n" +
                               "gamecode: " + Bytes.ascii(header, 12, 4) + "\n" +
                               "normal_cmd_setting: " + String(Bytes.u32(header, 0x60)) + "\n").utf8),
                        to: dsd + "/header.yaml")
        let packed = try Pack.write(work + "/extracted/dsd/files", romfs + "/extracted/files.pak")
        log("Packed \(packed) game files.")
        // the settings: the game's options.txt, read at boot until the
        // player saves their own on the SD card (options3ds.cpp)
        try Bytes.write(Array(options.fileText().utf8), to: romfs + "/options.txt")
        log("Settings: " + options.summary() + ".")

        // 3. the icon from the DS game's own icon; the banner the title
        // logo with the opening's star round it (Face.swift), or, failing
        // that, a flat picture of the logo
        progress(80, "Drawing the HOME Menu icon and banner...")
        let art = tmp + "/art"
        try mkdir(art)
        let dsIcon = try Art.dsIcon(rom)
        try Art.saveIcon(dsIcon, art + "/icon.png")
        var bannerArt: [String]? = nil
        let logo = try Art.titleLogo(rom)
        if logo == nil { log("Couldn't read the title logo.") }
        let template = k("face.template")
        if fm.fileExists(atPath: template) {
            let dsmt = work + "/extracted/dsd/files/data/DSMT"
            if let model = try Face.banner(dsmt: dsmt, template: try Bytes.read(template),
                                           recipe: try Bytes.lines(k("face.recipe")), logo: logo, log: log) {
                try Bytes.write(model, to: art + "/banner.cgfx")
                bannerArt = ["-ci", art + "/banner.cgfx"]
                log("Made the 3D banner (\(model.count / 1024) KB).")
            }
        }
        if bannerArt == nil {
            try Art.saveBanner(logo, dsIcon, art + "/banner.png")
            bannerArt = ["-i", art + "/banner.png"]
        }
        // the HOME Menu sound (banner.wav in the payload)
        var sound = k("banner.wav")
        if !fm.fileExists(atPath: sound) {
            sound = art + "/banner.wav"
            try Art.saveSilence(sound)
        }
        let bannertool = try Payload.helper("bannertool")
        try tool(bannertool, ["makesmdh", "-s", Program.title, "-l", Program.title, "-p", Program.author,
                              "-i", art + "/icon.png", "-o", art + "/icon.smdh"], "making the icon")
        try tool(bannertool, ["makebanner", bannerArt![0], bannerArt![1], "-a", sound, "-o", art + "/banner.bnr"],
                 "making the banner")

        // 4. the .cia (make_cia.py's makerom line)
        progress(86, "Building the .cia...")
        let made = tmp + "/out.cia"
        try tool(try Payload.helper("makerom"), [
            "-f", "cia", "-o", made, "-elf", k("game3ds.elf"), "-rsf", k("game3ds.rsf"),
            "-icon", art + "/icon.smdh", "-banner", art + "/banner.bnr",
            "-DROMFS_DIR=" + romfs, "-target", "t", "-exefslogo"], "building the .cia")
        let size = (try? fm.attributesOfItem(atPath: made)[.size] as? Int) ?? nil
        if (size ?? 0) < 1024 * 1024 { throw BuildError("makerom did not write the .cia.") }
        try mkdir((output as NSString).deletingLastPathComponent)
        if fm.fileExists(atPath: output) { try fm.removeItem(atPath: output) }
        try fm.moveItem(atPath: made, toPath: output)
        log("Wrote " + output + " (\(size! / (1024 * 1024)) MB).")
        progress(100, "Done.")
    }

    // The cartridge header, with the checks a player can act on before the
    // extractor's own (it checks them again, and everything after them).
    func readHeader() throws -> [UInt8] {
        let h: [UInt8]
        do {
            let f = try FileHandle(forReadingFrom: URL(fileURLWithPath: rom))
            defer { try? f.close() }
            let data = try f.read(upToCount: 0x200) ?? Data()
            if data.count < 0x200 { throw BuildError("This file is too small to be a DS dump.") }
            h = [UInt8](data)
        } catch let e as BuildError {
            throw e
        } catch {
            throw BuildError("The dump could not be read: " + error.localizedDescription)
        }
        let code = Bytes.ascii(h, 12, 4)
        if !code.hasPrefix("ASM") {
            throw BuildError("This is not a Super Mario 64 DS dump (its game code is " +
                             Bytes.printable(code.replacingOccurrences(of: "\0", with: " ")).trimmingCharacters(in: .whitespaces) + ").")
        }
        if code != "ASMP" {
            throw BuildError("This is Super Mario 64 DS, but not the European version (game code " + code + "). " +
                             Program.title + " is built from the European cartridge (ASMP); its data does not " +
                             "come out of another region's.")
        }
        return h
    }

    func tool(_ exe: String, _ args: [String], _ doing: String) throws {
        var lines: [String] = []
        let rc = try Proc.run(exe, args) { lines.append($0) }
        if rc != 0 {
            for l in lines { log("  " + l) }
            throw BuildError("Stopped while " + doing + " (" + (exe as NSString).lastPathComponent + " exit code \(rc)).")
        }
    }
}

enum Proc {
    // exe's output and errors, a line at a time, then its exit code
    static func run(_ exe: String, _ args: [String], _ onLine: (String) -> Void) throws -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        do {
            try p.run()
        } catch {
            throw BuildError("Could not start " + (exe as NSString).lastPathComponent + ": " + error.localizedDescription)
        }
        let out = pipe.fileHandleForReading
        var pending = Data()
        func emit(_ d: Data) { onLine(String(decoding: d, as: UTF8.self).trimmingCharacters(in: CharacterSet(charactersIn: "\r"))) }
        while true {
            let chunk = out.availableData
            if chunk.isEmpty { break }
            pending.append(chunk)
            while let nl = pending.firstIndex(of: 10) {
                emit(pending[pending.startIndex..<nl])
                pending.removeSubrange(pending.startIndex...nl)
            }
        }
        if !pending.isEmpty { emit(pending) }
        p.waitUntilExit()
        return p.terminationStatus
    }
}

// Same S3PK pack as port3ds/tools/pack_files.py, byte for byte: same order
// (Python's sorted() over Windows paths, compared per component, lowercased),
// and dsd's files.txt left out.
enum Pack {
    static func before(_ a: [String], _ b: [String]) -> Bool {
        for i in 0..<min(a.count, b.count) {
            let x = Array(a[i].lowercased().utf16), y = Array(b[i].lowercased().utf16)
            if x != y { return x.lexicographicallyPrecedes(y) }
        }
        return a.count < b.count
    }

    static func write(_ src: String, _ outPath: String) throws -> Int {
        let root = URL(fileURLWithPath: src).standardizedFileURL.path
        var rels: [[String]] = []
        guard let walk = FileManager.default.enumerator(atPath: root) else {
            throw BuildError("The extracted game files are missing.")
        }
        while let rel = walk.nextObject() as? String {
            if walk.fileAttributes?[.type] as? FileAttributeType != .typeRegular { continue }
            if rel == "files.txt" { continue }
            rels.append(rel.components(separatedBy: "/"))
        }
        rels.sort(by: before)
        let names = rels.map { Array($0.joined(separator: "/").utf8) }
        let indexLen = names.reduce(0) { $0 + 10 + $1.count }
        let dataOff = (16 + indexLen + 3) & ~3
        var sizes: [Int] = []
        for r in rels {
            let attrs = try FileManager.default.attributesOfItem(atPath: root + "/" + r.joined(separator: "/"))
            sizes.append((attrs[.size] as? Int) ?? 0)
        }
        var o: [UInt8] = []
        o.reserveCapacity(dataOff + sizes.reduce(0) { $0 + $1 + 3 })
        func u32(_ v: Int) { let x = UInt32(truncatingIfNeeded: v); o += [UInt8(x & 0xff), UInt8(x >> 8 & 0xff), UInt8(x >> 16 & 0xff), UInt8(x >> 24)] }
        o += Array("S3PK".utf8)
        u32(1)
        u32(rels.count)
        u32(dataOff)
        var at = dataOff
        for i in 0..<rels.count {
            u32(at)
            u32(sizes[i])
            o += [UInt8(names[i].count & 0xff), UInt8(names[i].count >> 8 & 0xff)]
            o += names[i]
            at += sizes[i] + ((-sizes[i]) & 3)
        }
        while o.count < dataOff { o.append(0) }
        for r in rels {
            let b = try Bytes.read(root + "/" + r.joined(separator: "/"))
            o += b
            o += [UInt8](repeating: 0, count: (-b.count) & 3)
        }
        try FileManager.default.createDirectory(atPath: (outPath as NSString).deletingLastPathComponent,
                                                withIntermediateDirectories: true)
        try Bytes.write(o, to: outPath)
        return rels.count
    }
}

// Bytes, files and text the way the Windows builder has them
enum Bytes {
    static func read(_ path: String) throws -> [UInt8] { return [UInt8](try Data(contentsOf: URL(fileURLWithPath: path))) }

    static func write<C: Collection>(_ b: C, to path: String) throws where C.Element == UInt8 {
        try Data(b).write(to: URL(fileURLWithPath: path))
    }

    static func u32(_ d: [UInt8], _ o: Int) -> Int {
        return Int(d[o]) | Int(d[o + 1]) << 8 | Int(d[o + 2]) << 16 | Int(d[o + 3]) << 24
    }

    static func u16(_ d: [UInt8], _ o: Int) -> Int { return Int(d[o]) | Int(d[o + 1]) << 8 }

    // .NET's Encoding.ASCII: bytes over 0x7f come out as '?'
    static func ascii(_ d: [UInt8], _ o: Int, _ n: Int) -> String {
        guard n > 0 else { return "" }
        return String(String.UnicodeScalarView(d[o..<o + n].map { Unicode.Scalar($0 < 0x80 ? $0 : 0x3f) }))
    }

    // control characters as '.', for quoting a file's bytes in a message
    static func printable(_ s: String) -> String {
        return String(String.UnicodeScalarView(s.unicodeScalars.map { $0.value >= 0x20 && $0.value < 0x7f ? $0 : "." }))
    }

    // File.ReadAllLines: split at line ends, without them
    static func lines(_ path: String) throws -> [String] {
        let text = String(decoding: try read(path), as: UTF8.self)
        var lines = text.components(separatedBy: "\n").map { $0.hasSuffix("\r") ? String($0.dropLast()) : $0 }
        if lines.last == "" { lines.removeLast() }
        return lines
    }

    // [math]::Round, for the extractor's narration: to even, and no ".0"
    static func round(_ x: Double, _ digits: Int = 0) -> String {
        let scale = pow(10.0, Double(digits))
        let r = (x * scale).rounded(.toNearestOrEven) / scale
        return r == r.rounded() ? String(Int(r)) : String(format: "%.\(digits)f", r)
    }
}

extension String {
    func trimmingTrailingNuls() -> String {
        var s = self
        while s.hasSuffix("\0") { s.removeLast() }
        return s
    }
}

// A picture as straight (not premultiplied) RGBA bytes, top row first: what
// a GDI+ Bitmap's pixels are to the Windows builder
struct Pixels {
    let width: Int, height: Int
    var rgba: [UInt8]

    init(_ width: Int, _ height: Int) {
        self.width = width
        self.height = height
        rgba = [UInt8](repeating: 0, count: width * height * 4)
    }

    mutating func set(_ x: Int, _ y: Int, _ r: Int, _ g: Int, _ b: Int, _ a: Int) {
        let at = (y * width + x) * 4
        rgba[at] = UInt8(r); rgba[at + 1] = UInt8(g); rgba[at + 2] = UInt8(b); rgba[at + 3] = UInt8(a)
    }

    // nearest neighbour, factor times the size
    func scaled(_ factor: Int) -> Pixels {
        var big = Pixels(width * factor, height * factor)
        for y in 0..<big.height {
            for x in 0..<big.width {
                let from = ((y / factor) * width + x / factor) * 4, to = (y * big.width + x) * 4
                big.rgba.replaceSubrange(to..<to + 4, with: rgba[from..<from + 4])
            }
        }
        return big
    }

    func cgImage() -> CGImage? {
        guard let provider = CGDataProvider(data: Data(rgba) as CFData) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: Look.space, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    // a w x h canvas, bottom-up as Core Graphics draws
    static func canvas(_ w: Int, _ h: Int) -> CGContext? {
        return CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: Look.space,
                         bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    }

    static func png(_ image: CGImage, _ path: String) throws {
        guard let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil) else {
            throw BuildError("Could not write " + path + ".")
        }
        CGImageDestinationAddImage(dest, image, nil)
        if !CGImageDestinationFinalize(dest) { throw BuildError("Could not write " + path + ".") }
    }
}

// HOME Menu art from the DS game's own icon: the 32x32 16-colour picture in
// the dump's banner (header +0x68: tiles at +0x20, palette at +0x220, colour 0
// transparent).
enum Art {
    static func dsIcon(_ rom: String) throws -> Pixels {
        let f = try FileHandle(forReadingFrom: URL(fileURLWithPath: rom))
        defer { try? f.close() }
        let length = try f.seekToEnd()
        try f.seek(toOffset: 0x68)
        let head = [UInt8](try f.read(upToCount: 4) ?? Data())
        guard head.count == 4 else { throw BuildError("The dump has no DS icon to make the HOME Menu icon from.") }
        let banner = UInt64(Bytes.u32(head, 0))
        if banner == 0 || banner + 0x240 > length {
            throw BuildError("The dump has no DS icon to make the HOME Menu icon from.")
        }
        try f.seek(toOffset: banner + 0x20)
        let both = [UInt8](try f.read(upToCount: 512 + 32) ?? Data())
        guard both.count == 544 else { throw BuildError("The dump has no DS icon to make the HOME Menu icon from.") }
        let tiles = Array(both[0..<512]), pal = Array(both[512..<544])
        var colours = [[Int]](repeating: [255, 255, 255, 0], count: 16)   // 0: Color.Transparent
        for i in 1..<16 {
            let c = Int(pal[i * 2]) | Int(pal[i * 2 + 1]) << 8
            let rr = c & 31, gg = (c >> 5) & 31, bb = (c >> 10) & 31
            colours[i] = [rr << 3 | rr >> 2, gg << 3 | gg >> 2, bb << 3 | bb >> 2, 255]
        }
        var bmp = Pixels(32, 32)
        for ty in 0..<4 {
            for tx in 0..<4 {
                for y in 0..<8 {
                    for x in 0..<8 {
                        let b = Int(tiles[(ty * 4 + tx) * 32 + y * 4 + x / 2])
                        let c = colours[(x & 1) == 0 ? b & 15 : b >> 4]
                        bmp.set(tx * 8 + x, ty * 8 + y, c[0], c[1], c[2], c[3])
                    }
                }
            }
        }
        return bmp
    }

    // 48x48 (the SMDH's large icon), on white: an SMDH icon has no alpha
    static func saveIcon(_ ds: Pixels, _ path: String) throws {
        guard let big = ds.scaled(3).cgImage(), let g = Pixels.canvas(48, 48) else {
            throw BuildError("Could not draw the HOME Menu icon.")
        }
        g.setFillColor(Look.rgb(255, 255, 255))
        g.fill(CGRect(x: 0, y: 0, width: 48, height: 48))
        g.interpolationQuality = .high
        g.draw(big, in: CGRect(x: 2, y: 2, width: 44, height: 44))
        guard let icon = g.makeImage() else { throw BuildError("Could not draw the HOME Menu icon.") }
        try Pixels.png(icon, path)
    }

    // The title screen's logo, from overlay 7 of the dump: title_demo_main.ncg
    // laid out by its screen, in the title palette. These are NITRO-System's
    // source formats (NCCL/NCSC/NCCG) embedded in the overlay. Each block is
    // checked against the European ROM's hash; nil if any doesn't match.
    static func titleLogo(_ rom: String) throws -> Pixels? {
        guard let ov = overlay(try Bytes.read(rom), 7) else { return nil }
        guard let pal = block(ov, 0x2bf08, 556, "8d3a76050a99c1b2007249164cb4047450ea607d5a70a01f90017a2cedf4cc2a"),
              let scr = block(ov, 0x30068, 6420, "eca1045a4f516efcfa532f0ed85bf97a2d9bef295db19f51d97825b35780b4fd"),
              let chr = block(ov, 0x3d08c, 33392, "16eac000c3f8de4135ccd1b78d465aa52bb350fba1323162f4ddf20f2f1bbc83")
        else { return nil }

        // PALT colours start at +0x20, the SCRN's 32x32 entries at +0x28, the
        // CHAR's 8bpp tiles at +0x24
        var colours = [[Int]](repeating: [0, 0, 0], count: 256)
        for i in 1..<256 {
            let c = Int(pal[0x20 + i * 2]) | Int(pal[0x21 + i * 2]) << 8
            let r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31
            colours[i] = [r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2]
        }
        var px = Pixels(256, 256)
        var minX = 256, minY = 256, maxX = -1, maxY = -1
        for k in 0..<32 * 32 {
            let e = Int(scr[0x28 + k * 2]) | Int(scr[0x29 + k * 2]) << 8
            let tile = e & 0x3ff
            let hf = (e & 0x400) != 0, vf = (e & 0x800) != 0
            if 0x24 + tile * 64 + 64 > chr.count { continue }
            for y in 0..<8 {
                for x in 0..<8 {
                    let idx = Int(chr[0x24 + tile * 64 + (vf ? 7 - y : y) * 8 + (hf ? 7 - x : x)])
                    if idx == 0 { continue }
                    let X = (k % 32) * 8 + x, Y = (k / 32) * 8 + y
                    px.set(X, Y, colours[idx][0], colours[idx][1], colours[idx][2], 255)
                    minX = min(minX, X); maxX = max(maxX, X)
                    minY = min(minY, Y); maxY = max(maxY, Y)
                }
            }
        }
        if maxX < 0 { return nil }
        var logo = Pixels(maxX - minX + 1, maxY - minY + 1)
        for y in 0..<logo.height {
            let from = ((minY + y) * 256 + minX) * 4
            logo.rgba.replaceSubrange(y * logo.width * 4..<(y + 1) * logo.width * 4, with: px.rgba[from..<from + logo.width * 4])
        }
        return logo
    }

    static func block(_ ov: [UInt8], _ at: Int, _ size: Int, _ sha: String) -> [UInt8]? {
        if at + size > ov.count { return nil }
        let b = Array(ov[at..<at + size])
        return Face.sha(b) == sha ? b : nil
    }

    // An ARM9 overlay out of the dump, decompressed
    static func overlay(_ rom: [UInt8], _ id: Int) -> [UInt8]? {
        guard rom.count >= 0x58 else { return nil }
        let fat = Bytes.u32(rom, 0x48), table = Bytes.u32(rom, 0x50), tableSize = Bytes.u32(rom, 0x54)
        for i in 0..<tableSize / 32 {
            let e = table + i * 32
            guard e + 32 <= rom.count else { return nil }
            if Bytes.u32(rom, e) != id { continue }
            let file = Bytes.u32(rom, e + 24), flags = Bytes.u32(rom, e + 28)
            guard fat + file * 8 + 8 <= rom.count else { return nil }
            let start = Bytes.u32(rom, fat + file * 8), end = Bytes.u32(rom, fat + file * 8 + 4)
            if end <= start || end > rom.count { return nil }
            let raw = Array(rom[start..<end])
            return (flags & 0x1000000) != 0 ? blz(raw) : raw
        }
        return nil
    }

    // The DS's backward LZ (overlays and arm9): decoded from the end down
    static func blz(_ c: [UInt8]) -> [UInt8]? {
        if c.count < 8 { return nil }
        let hdr = Bytes.u32(c, c.count - 8)
        let extra = Int(Int32(truncatingIfNeeded: Bytes.u32(c, c.count - 4)))
        let enc = hdr & 0xffffff, headLen = hdr >> 24
        if extra < 0 { return nil }
        var o = c + [UInt8](repeating: 0, count: extra)
        var src = c.count - headLen, dst = o.count
        let stop = c.count - enc
        if stop < 0 || src > c.count { return nil }
        while src > stop {
            src -= 1
            let flags = Int(c[src])
            var bit = 0
            while bit < 8 && src > stop {
                if (flags & (0x80 >> bit)) != 0 {
                    if src < 2 { return nil }
                    src -= 2
                    let v = Int(c[src + 1]) << 8 | Int(c[src])
                    var len = (v >> 12) + 3
                    let disp = (v & 0xfff) + 3
                    if dst - len < 0 || dst - 1 + disp >= o.count { return nil }
                    while len > 0 { dst -= 1; o[dst] = o[dst + disp]; len -= 1 }
                } else {
                    if dst == 0 { return nil }
                    dst -= 1
                    src -= 1
                    o[dst] = c[src]
                }
                bit += 1
            }
        }
        return o
    }

    // 256x128: the title logo on a sky, or the DS icon if the logo couldn't be read
    static func saveBanner(_ logo: Pixels?, _ ds: Pixels, _ path: String) throws {
        guard let g = Pixels.canvas(256, 128) else { throw BuildError("Could not draw the HOME Menu banner.") }
        g.drawLinearGradient(Look.gradient([Look.rgb(52, 132, 228), Look.rgb(150, 208, 255)], [0, 1]),
                             start: CGPoint(x: 0, y: 128), end: CGPoint(x: 0, y: 0), options: [])
        let art = logo ?? ds
        let s = logo != nil ? min(1, min(244 / Double(art.width), 120 / Double(art.height))) : 3
        let w = Int((Double(art.width) * s).rounded(.toNearestOrEven)), h = Int((Double(art.height) * s).rounded(.toNearestOrEven))
        g.interpolationQuality = s == 1 || logo == nil ? .none : .high
        if let img = art.cgImage() {
            g.draw(img, in: CGRect(x: (256 - w) / 2, y: 128 - (128 - h) / 2 - h, width: w, height: h))
        }
        guard let banner = g.makeImage() else { throw BuildError("Could not draw the HOME Menu banner.") }
        try Pixels.png(banner, path)
    }

    // a short silent banner sound, for a payload without banner.wav
    // (bannertool wants one; the HOME Menu plays it)
    static func saveSilence(_ path: String) throws {
        let rate = 22050, channels = 2, samples = rate / 2
        let bytes = samples * channels * 2
        var w: [UInt8] = []
        func u32(_ v: Int) { w += [UInt8(v & 0xff), UInt8(v >> 8 & 0xff), UInt8(v >> 16 & 0xff), UInt8(v >> 24 & 0xff)] }
        func u16(_ v: Int) { w += [UInt8(v & 0xff), UInt8(v >> 8 & 0xff)] }
        w += Array("RIFF".utf8); u32(36 + bytes)
        w += Array("WAVEfmt ".utf8); u32(16)
        u16(1); u16(channels); u32(rate); u32(rate * channels * 2); u16(channels * 2); u16(16)
        w += Array("data".utf8); u32(bytes)
        w += [UInt8](repeating: 0, count: bytes)
        try Bytes.write(w, to: path)
    }
}

// --snapshot DIR [--rom DUMP]: the windows in their states as PNGs (at 2x),
// for checking the look without showing anything on screen. --icon DIR: the
// app icon's iconset, for package.py.
enum Snapshot {
    static func windows(_ dir: String, _ args: [String]) -> Int32 {
        NSApplication.shared.setActivationPolicy(.prohibited)   // no Dock icon, nothing on screen
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        var rom: String? = nil
        if let i = args.firstIndex(of: "--rom"), i + 1 < args.count { rom = args[i + 1] }
        let w = BuilderWindow()
        for s in w.sky.stars { s.phase = .pi }   // every star at its brightest
        func shot(_ name: String) {
            w.sky.display()
            draw(w.sky, dir + "/" + name + ".png")
        }
        shot("empty")
        w.tile.showDragForSnapshot(true)
        shot("drop")
        w.tile.showDragForSnapshot(false)
        if let rom = rom {
            w.setRom(rom)
            shot("chosen")
            w.setBusy(true)
            w.progress(42, "Unpacking file 1000 of 2072")
            shot("building")
            w.setBusy(false)
            w.tile.progress = -1
            w.showSaved((rom as NSString).deletingLastPathComponent + "/" + Program.title + ".cia")
            shot("saved")
        }
        let d = SettingsWindow(w.options)
        draw(d.contentView!, dir + "/settings.png")
        d.chosen.old = true
        d.chosen.heavy()
        d.showChosen()
        draw(d.contentView!, dir + "/settings-old.png")
        return 0
    }

    static func draw(_ view: NSView, _ path: String) {
        let size = view.bounds.size
        guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * 2), pixelsHigh: Int(size.height * 2),
                                         bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                                         colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0) else { return }
        rep.size = size
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    }

    static func icon(_ dir: String) -> Int32 {
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        for (points, scale) in [(16, 1), (16, 2), (32, 1), (32, 2), (128, 1), (128, 2), (256, 1), (256, 2), (512, 1), (512, 2)] {
            let px = points * scale
            guard let g = Pixels.canvas(px, px) else { return 1 }
            g.translateBy(x: 0, y: CGFloat(px))
            g.scaleBy(x: CGFloat(px) / 1024, y: -CGFloat(px) / 1024)
            Look.appIcon(g)
            guard let img = g.makeImage() else { return 1 }
            let name = "icon_\(points)x\(points)" + (scale == 2 ? "@2x" : "") + ".png"
            do { try Pixels.png(img, dir + "/" + name) } catch { return 1 }
        }
        return 0
    }
}
