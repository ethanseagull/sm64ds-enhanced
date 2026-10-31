// The build settings: the console a build is for and the game's four options
// (the switches of its SELECT menu), chosen in the builder's settings window
// and written into the .cia as the game's options.txt (Build.run). The game
// reads that file at boot until the player saves their own from the menu
// (port3ds/game/options3ds.cpp). A port of ../Settings.cs.
import AppKit

// What a build starts the game with. The window's choices are remembered in
// ~/Library/Application Support/SM64DSEnhancedBuilder/settings.txt, in
// options.txt's lines.
final class BuildOptions {
    var old = false   // for an Old 3DS (a New 3DS otherwise)
    var fps60 = true, aa = true, counter = false, analog = true

    // The console's defaults: on a New 3DS everything but the FPS counter,
    // on an Old 3DS only analog controls.
    func reset() {
        heavy()
        counter = false
        analog = true
    }

    // 60 fps and anti-aliasing, the two that depend on the console: on for a
    // New 3DS, off for an Old 3DS
    func heavy() {
        fps60 = !old
        aa = !old
    }

    func copy() -> BuildOptions {
        let o = BuildOptions()
        o.take(self)
        return o
    }

    func take(_ o: BuildOptions) {
        old = o.old
        fps60 = o.fps60
        aa = o.aa
        counter = o.counter
        analog = o.analog
    }

    func same(_ o: BuildOptions) -> Bool {
        return old == o.old && fps60 == o.fps60 && aa == o.aa && counter == o.counter && analog == o.analog
    }

    // options.txt's lines. The game puts console= only in its log: it sizes
    // itself to the console it runs on either way.
    func fileText() -> String {
        return "# The builder's settings, read at boot until the options menu (SELECT)\n" +
               "# saves sdmc:/sm64ds3ds/options.txt.\n" +
               "console=" + (old ? "old" : "new") + "\n" +
               "60fps=" + BuildOptions.bit(fps60) + "\n" +
               "anti_aliasing=" + BuildOptions.bit(aa) + "\n" +
               "fps_counter=" + BuildOptions.bit(counter) + "\n" +
               "analog_controls=" + BuildOptions.bit(analog) + "\n"
    }

    static func bit(_ on: Bool) -> String { return on ? "1" : "0" }

    // One KEY=VALUE line; false if it isn't one of options.txt's. A console
    // line brings that console's 60 fps and anti-aliasing defaults, so lines
    // after it (as in fileText) still have the last word.
    func set(_ line: String) -> Bool {
        guard let eq = line.firstIndex(of: "=") else { return false }
        let key = line[..<eq].trimmingCharacters(in: .whitespaces)
        let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces).lowercased()
        let on = value == "1" || value == "on" || value == "true"
        switch key {
        case "console":
            if value != "old" && value != "new" { return false }
            old = value == "old"
            heavy()
            return true
        case "60fps": fps60 = on; return true
        case "anti_aliasing": aa = on; return true
        case "fps_counter": counter = on; return true
        case "analog_controls": analog = on; return true
        default: return false
        }
    }

    func summary() -> String {
        return (old ? "Old 3DS" : "New 3DS") + "; 60 fps " + BuildOptions.onOff(fps60) + ", anti-aliasing " +
               BuildOptions.onOff(aa) + ", FPS counter " + BuildOptions.onOff(counter) + ", analog controls " +
               BuildOptions.onOff(analog)
    }

    static func onOff(_ on: Bool) -> String { return on ? "on" : "off" }

    static var savedFile: URL { return Look.supportFolder.appendingPathComponent("settings.txt") }

    static func load() -> BuildOptions {
        let o = BuildOptions()
        if let text = try? String(contentsOf: savedFile, encoding: .utf8) {
            for line in text.components(separatedBy: "\n") where !line.hasPrefix("#") {
                _ = o.set(line.trimmingCharacters(in: CharacterSet(charactersIn: "\r")))
            }
        }
        return o
    }

    func save() {
        try? FileManager.default.createDirectory(at: Look.supportFolder, withIntermediateDirectories: true)
        try? fileText().write(to: BuildOptions.savedFile, atomically: true, encoding: .utf8)
    }
}

// The settings window, under the same night sky: the console as two cards
// (the chosen one gold), the game's options as switches on a glass panel,
// and Done. Esc or the close button leaves the settings as they were.
// NSApp.runModal(for:) it; it stops the modal session with .OK for Done.
final class SettingsWindow: NSWindow {
    let chosen: BuildOptions
    let newCard = ConsoleCard(), oldCard = ConsoleCard()
    let fps = Switch(), aa = Switch(), counter = Switch(), analog = Switch()
    private var finished = false

    static let pad: CGFloat = 28, w: CGFloat = 440, h: CGFloat = 436, rowH: CGFloat = 42
    static let mark = CGPoint(x: pad + 13, y: 41)
    static let cardsBox = CGRect(x: pad, y: 92, width: w - pad * 2, height: 46)
    static let panelBox = CGRect(x: pad, y: 178, width: w - pad * 2, height: rowH * 4 + 8)
    static let footerBox = CGRect(x: pad - 2, y: 370, width: w - pad * 2 + 4, height: 46)

    init(_ current: BuildOptions) {
        chosen = current.copy()
        super.init(contentRect: NSRect(x: 0, y: 0, width: SettingsWindow.w, height: SettingsWindow.h),
                   styleMask: [.titled, .closable], backing: .buffered, defer: false)
        title = "Build settings"
        Look.dress(self)
        let view = SkyView(frame: NSRect(x: 0, y: 0, width: SettingsWindow.w, height: SettingsWindow.h),
                           sky: SettingsWindow.sky())
        contentView = view

        let cards = SettingsWindow.cardsBox
        let half = (cards.width - 10) / 2
        newCard.text = "New 3DS"
        newCard.frame = CGRect(x: cards.minX, y: cards.minY, width: half, height: cards.height)
        oldCard.text = "Old 3DS"
        oldCard.frame = CGRect(x: cards.minX + half + 10, y: cards.minY, width: half, height: cards.height)
        newCard.onClick = { [unowned self] in self.pickConsole(false) }
        oldCard.onClick = { [unowned self] in self.pickConsole(true) }

        row(fps, 0, "60 FPS") { [unowned self] in self.chosen.fps60 = self.fps.on }
        row(aa, 1, "Anti-aliasing") { [unowned self] in self.chosen.aa = self.aa.on }
        row(counter, 2, "FPS counter") { [unowned self] in self.chosen.counter = self.counter.on }
        row(analog, 3, "Analog controls") { [unowned self] in self.chosen.analog = self.analog.on }

        let footer = SettingsWindow.footerBox
        let restore = FlatButton()
        restore.text = "Restore defaults"
        restore.frame = CGRect(x: SettingsWindow.pad, y: footer.minY + 2, width: 150, height: 42)
        restore.onClick = { [unowned self] in self.chosen.reset(); self.showChosen() }
        let done = FlatButton()
        done.text = "Done"
        done.primary = true
        done.frame = CGRect(x: SettingsWindow.w - SettingsWindow.pad - 120, y: footer.minY + 2, width: 120, height: 42)
        done.onClick = { [unowned self] in self.finish(.OK) }

        for v in [newCard, oldCard, fps, aa, counter, analog, restore, done] as [NSView] { view.addSubview(v) }
        showChosen()
    }

    func row(_ s: Switch, _ i: Int, _ name: String, _ toggled: @escaping () -> Void) {
        let panel = SettingsWindow.panelBox
        s.text = name
        s.frame = CGRect(x: panel.minX + 4, y: panel.minY + 4 + CGFloat(i) * SettingsWindow.rowH,
                         width: panel.width - 8, height: SettingsWindow.rowH)
        s.toggled = toggled
    }

    // The console's own 60 fps and anti-aliasing defaults come with it; the
    // other switches stay as they are.
    func pickConsole(_ old: Bool) {
        if chosen.old == old { return }
        chosen.old = old
        chosen.heavy()
        showChosen()
    }

    func showChosen() {
        newCard.on = !chosen.old
        oldCard.on = chosen.old
        fps.on = chosen.fps60
        aa.on = chosen.aa
        counter.on = chosen.counter
        analog.on = chosen.analog
        for v in contentView?.subviews ?? [] { v.needsDisplay = true }
    }

    func finish(_ response: NSApplication.ModalResponse) {
        if finished { return }
        finished = true
        if NSApp.modalWindow === self { NSApp.stopModal(withCode: response) }
        orderOut(nil)
    }

    // Return is Done; Esc and the close button leave things as they were
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 36 || event.keyCode == 76 { finish(.OK) } else { super.keyDown(with: event) }
    }

    override func cancelOperation(_ sender: Any?) { finish(.cancel) }

    override func performClose(_ sender: Any?) { finish(.cancel) }

    override func close() {
        finish(.cancel)
        super.close()
    }

    // Everything that doesn't change: the sky, the star and title, the
    // headings, and the options' glass panel with its dividers.
    static func sky() -> NSImage {
        return NSImage(size: NSSize(width: w, height: h), flipped: true) { _ in
            guard let g = NSGraphicsContext.current?.cgContext else { return false }
            Look.night(g, w, h, [cardsBox, panelBox, footerBox])
            Look.goldStar(g, mark, 13)
            let t = Look.lettering("Build settings", Look.display(22), 0, CGPoint(x: pad + 36, y: 26))
            Look.fill(g, t, Look.rgb(255, 255, 255), Look.rgb(200, 210, 240), t.boundingBoxOfPath.insetBy(dx: 0, dy: -1))
            heading(g, "CONSOLE", cardsBox.minY - 18)
            heading(g, "GAME OPTIONS", panelBox.minY - 18)
            Look.box(g, panelBox.insetBy(dx: 0.5, dy: 0.5), 14, Look.white(9), Look.white(34))
            g.setStrokeColor(Look.white(18))
            g.setLineWidth(1)
            for i in 1..<4 {
                let y = panelBox.minY + 4 + CGFloat(i) * rowH + 0.5
                g.move(to: CGPoint(x: panelBox.minX + 16, y: y))
                g.addLine(to: CGPoint(x: panelBox.maxX - 16, y: y))
                g.strokePath()
            }
            return true
        }
    }

    static func heading(_ g: CGContext, _ text: String, _ y: CGFloat) {
        g.addPath(Look.lettering(text, Look.display(10.5), 2.4, CGPoint(x: pad + 2, y: y)))
        g.setFillColor(Look.gold)
        g.fillPath()
    }
}

// A console to build for: gold when chosen, glass otherwise, with a small
// open 3DS drawn beside its name.
final class ConsoleCard: Control {
    var on = false { didSet { needsDisplay = true } }

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let w = bounds.width, h = bounds.height
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        let ink: CGColor
        if on {
            if hover { Look.goldBox(g, r, 12, Look.rgb(255, 236, 166), Look.rgb(250, 186, 66)) }
            else { Look.goldBox(g, r, 12, Look.goldLight, Look.goldDeep) }
            ink = Look.goldInk
        } else {
            Look.box(g, r, 12, Look.white(hover ? 16 : 9), Look.white(hover ? 64 : 34))
            ink = hover ? Look.text : Look.rgb(214, 220, 240)
        }
        ConsoleCard.clamshell(g, CGPoint(x: 30, y: h / 2), ink)
        Look.write(text, Look.strong(14.5), CGRect(x: 52, y: 0, width: w - 58, height: h), ink)
        if focusShown {
            g.addPath(Look.rounded(r.insetBy(dx: 3, dy: 3), 9))
            g.setStrokeColor(on ? Look.alpha(150, Look.goldInk) : Look.white(150))
            g.setLineWidth(1.2)
            g.strokePath()
        }
    }

    // an open 3DS in outline: the wide top screen over the bottom half
    static func clamshell(_ g: CGContext, _ c: CGPoint, _ ink: CGColor) {
        g.setStrokeColor(ink)
        g.setLineWidth(1.4)
        g.setLineJoin(.round)
        g.addPath(Look.rounded(CGRect(x: c.x - 11, y: c.y - 13, width: 22, height: 12), 2.5))
        g.strokePath()
        g.addPath(Look.rounded(CGRect(x: c.x - 10, y: c.y + 1, width: 20, height: 12), 2.5))
        g.strokePath()
        g.setFillColor(ink.copy(alpha: ink.alpha / 2) ?? ink)
        g.fill(CGRect(x: c.x - 7.5, y: c.y - 10.5, width: 15, height: 7))   // the top screen
        g.fill(CGRect(x: c.x - 5, y: c.y + 3.5, width: 10, height: 7))      // the touch screen
    }

    override func accessibilityRole() -> NSAccessibility.Role? { return .radioButton }
    override func accessibilityValue() -> Any? { return on ? 1 : 0 }
}

// One of the game's options: its name and a switch that's gold when on.
// The whole row toggles it.
final class Switch: Control {
    var on = false { didSet { needsDisplay = true } }
    var toggled: (() -> Void)?

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let w = bounds.width, h = bounds.height
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        if hover { Look.box(g, r.insetBy(dx: 1, dy: 1), 10, Look.white(10), nil) }
        if focusShown {
            g.addPath(Look.rounded(r.insetBy(dx: 1.5, dy: 1.5), 10))
            g.setStrokeColor(Look.alpha(170, Look.gold))
            g.setLineWidth(1.2)
            g.strokePath()
        }
        Look.write(text, Look.strong(14), CGRect(x: 14, y: 0, width: w - 84, height: h), Look.text)

        let track = CGRect(x: w - 14 - 42, y: h / 2 - 11.5, width: 42, height: 23)
        let knob: CGFloat = 8.5, kx = on ? track.maxX - 11.5 : track.minX + 11.5, ky = track.midY
        if on {
            Look.goldBox(g, track, 11.5, hover ? Look.rgb(255, 236, 166) : Look.goldLight, Look.goldDeep)
            Look.glow(g, CGPoint(x: kx, y: ky + 1), knob + 3, knob + 3, Look.alpha(70, Look.goldInk))
            g.setFillColor(Look.rgb(255, 252, 240))
            g.fillEllipse(in: CGRect(x: kx - knob, y: ky - knob, width: knob * 2, height: knob * 2))
        } else {
            Look.box(g, track, 11.5, Look.white(hover ? 20 : 12), Look.white(hover ? 72 : 52))
            g.setFillColor(hover ? Look.text : Look.muted)
            g.fillEllipse(in: CGRect(x: kx - knob + 1, y: ky - knob + 1, width: knob * 2 - 2, height: knob * 2 - 2))
        }
    }

    override func click() {
        on.toggle()
        toggled?()
        super.click()
    }

    override func accessibilityRole() -> NSAccessibility.Role? { return .checkBox }
    override func accessibilityValue() -> Any? { return on ? 1 : 0 }
}
