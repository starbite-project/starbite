// SPDX-License-Identifier: GPL-2.0-only
// Starbite Link, Mac viewer: the app. The phone's Windows desktop (the "Starbite Display", an extra monitor
// Windows extends onto) in a Mac window that can go full screen, driven by the Mac's keyboard and mouse.
//
//   StarbiteLink [--auto | --usb | --tcp HOST[:PORT]] [--host HOST[:PORT]] [--cmd-as-ctrl] [--hide-cursor]
//                [--no-overlay] [--no-match-window] [--hidpi] [--verbose]
//   StarbiteLink --selftest --tcp 127.0.0.1:47474 --crc-file F [--headless] [--frames N] [--mode-test WxH]
//                [--snapshot PNG] [--timeout S]
//
// Default: AUTO = USB whenever the Starbite Link device (1209:0002) is plugged in, else TCP to the remembered
// host (127.0.0.1:47474). Menu choices are remembered; command line options override them for one run.

import AppKit
import ImageIO
import SBLinkC
import SystemConfiguration

struct Options {
    var mode: Int32 = Int32(SBL_MODE_AUTO.rawValue)
    var host = "127.0.0.1"
    var port: UInt16 = UInt16(SBLK_TCP_PORT)
    var cmdAsCtrl = false
    // The phone's frames do not contain the Windows pointer (Desktop Duplication leaves it out, and SBLINK v1
    // has no cursor message; win/sender/README.md): the Mac pointer is the only one, so it stays visible.
    var showCursor = true
    var overlay = false          // always show the status overlay (else: while not connected, and 5 s after changes)
    var matchWindow = true
    var hidpi = false
    var verbose = false
    var selftest = false
    var headless = false
    var crcFile: String?
    var frames = 100
    var modeTest = (w: 1024, h: 600)
    var finalSize: (w: Int, h: Int)? = nil   // selftest: the size the checked frame must have (default modeTest)
    var snapshot: String?
    var windowSnapshot: String?
    var timeout = 30.0

    static let defaults = UserDefaults.standard

    static func load() -> Options {
        var o = Options()
        let d = defaults
        if d.object(forKey: "mode") != nil { o.mode = Int32(d.integer(forKey: "mode")) }
        if let h = d.string(forKey: "host") { o.parseHost(h) }
        o.cmdAsCtrl = d.bool(forKey: "cmdAsCtrl")
        if d.object(forKey: "showCursor") != nil { o.showCursor = d.bool(forKey: "showCursor") }
        if d.object(forKey: "overlay") != nil { o.overlay = d.bool(forKey: "overlay") }
        if d.object(forKey: "matchWindow") != nil { o.matchWindow = d.bool(forKey: "matchWindow") }
        o.hidpi = d.bool(forKey: "hidpi")
        return o
    }

    func save() {
        let d = Options.defaults
        d.set(Int(mode), forKey: "mode")
        d.set("\(host):\(port)", forKey: "host")
        d.set(cmdAsCtrl, forKey: "cmdAsCtrl")
        d.set(showCursor, forKey: "showCursor")
        d.set(overlay, forKey: "overlay")
        d.set(matchWindow, forKey: "matchWindow")
        d.set(hidpi, forKey: "hidpi")
    }

    mutating func parseHost(_ s: String) {
        // HOST, HOST:PORT, [v6]:PORT
        var h = s, p: UInt16? = nil
        if h.hasPrefix("["), let close = h.firstIndex(of: "]") {
            let rest = h[h.index(after: close)...]
            if rest.hasPrefix(":") { p = UInt16(rest.dropFirst()) }
            h = String(h[h.index(after: h.startIndex)..<close])
        } else if h.filter({ $0 == ":" }).count == 1, let c = h.lastIndex(of: ":") {
            p = UInt16(h[h.index(after: c)...])
            h = String(h[..<c])
        }
        if !h.isEmpty { host = h }
        if let p = p, p != 0 { port = p }
    }

    mutating func parseArgs(_ args: [String]) -> String? {
        var i = 1
        func value() -> String? { i += 1; return i < args.count ? args[i] : nil }
        while i < args.count {
            let a = args[i]
            switch a {
            case "--auto": mode = Int32(SBL_MODE_AUTO.rawValue)
            case "--usb": mode = Int32(SBL_MODE_USB.rawValue)
            case "--tcp":
                mode = Int32(SBL_MODE_TCP.rawValue)
                if i + 1 < args.count, !args[i + 1].hasPrefix("--"), let v = value() { parseHost(v) }
            case "--host": guard let v = value() else { return "--host needs HOST[:PORT]" }; parseHost(v)
            case "--cmd-as-ctrl": cmdAsCtrl = true
            case "--cmd-as-win": cmdAsCtrl = false
            case "--show-cursor": showCursor = true
            case "--hide-cursor": showCursor = false   // for a source that draws its pointer into the frames
            case "--no-overlay": overlay = false
            case "--overlay": overlay = true
            case "--no-match-window": matchWindow = false
            case "--hidpi": hidpi = true
            case "--verbose": verbose = true
            case "--selftest": selftest = true
            case "--headless": headless = true
            case "--crc-file": crcFile = value()
            case "--frames": frames = Int(value() ?? "") ?? 100
            case "--snapshot": snapshot = value()
            case "--window-snapshot": windowSnapshot = value()
            case "--timeout": timeout = Double(value() ?? "") ?? 30
            case "--mode-test":
                let parts = (value() ?? "").split(separator: "x").compactMap { Int($0) }
                guard parts.count == 2 else { return "--mode-test needs WxH" }
                modeTest = (parts[0], parts[1])
            case "--final-size":
                let parts = (value() ?? "").split(separator: "x").compactMap { Int($0) }
                guard parts.count == 2 else { return "--final-size needs WxH" }
                finalSize = (parts[0], parts[1])
            case "--help", "-h":
                return "usage: StarbiteLink [--auto|--usb|--tcp HOST[:PORT]] [--host HOST[:PORT]] [--cmd-as-ctrl] " +
                    "[--hide-cursor] [--no-overlay] [--no-match-window] [--hidpi] [--verbose]\n" +
                    "       StarbiteLink --selftest --tcp HOST:PORT --crc-file F [--headless] [--frames N] " +
                    "[--mode-test WxH] [--final-size WxH] [--snapshot PNG] [--timeout S]"
            default:
                if a.hasPrefix("-psn_") || a.hasPrefix("-NS") || a.hasPrefix("-Apple") { // launch services / defaults
                    if !a.hasPrefix("-psn_") { i += 1 }
                } else {
                    return "unknown option \(a) (--help)"
                }
            }
            i += 1
        }
        return nil
    }
}

func setCString<T>(_ field: inout T, _ s: String) {
    withUnsafeMutableBytes(of: &field) { buf in
        buf.initializeMemory(as: UInt8.self, repeating: 0)
        let all = Array(s.utf8)
        var n = min(all.count, buf.count - 1)
        while n > 0 && n < all.count && all[n] & 0xC0 == 0x80 { n -= 1 }   // never cut a UTF-8 sequence
        buf.copyBytes(from: all[0..<n])
    }
}

func cString<T>(_ field: T) -> String {
    withUnsafeBytes(of: field) { buf in
        let bytes = buf.prefix(while: { $0 != 0 })
        return String(decoding: bytes, as: UTF8.self)
    }
}

func computerName() -> String {
    if let n = SCDynamicStoreCopyComputerName(nil, nil) as String?, !n.isEmpty { return n }
    return "Mac"
}

// C callbacks (link queue)
private let frameCallback: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<sbv_frame_info>?) -> Void = { ctx, fi in
    guard let ctx = ctx, let fi = fi else { return }
    let app = Unmanaged<AppController>.fromOpaque(ctx).takeUnretainedValue()
    app.presenter.frame(fi)
    app.selftest?.onFrame(fi)
}

private let logCallback: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void = { ctx, line in
    guard let ctx = ctx, let line = line else { return }
    let app = Unmanaged<AppController>.fromOpaque(ctx).takeUnretainedValue()
    let s = String(cString: line)
    if app.options.selftest {
        if !app.options.verbose { FileHandle.standardError.write("link: \(s)\n".data(using: .utf8)!) }
    } else {
        NSLog("%@", s)
    }
}

final class AppController: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var options: Options
    var window: NSWindow!
    var view: DisplayView!
    let presenter = Presenter()
    var link: OpaquePointer?
    var selftest: SelfTest?
    private var statusTimer: Timer?
    private var keyMonitor: Any?
    private var lastConn: Int32 = -1
    private var overlayUntil = Date.distantPast
    private var lastCaptured = false

    init(options: Options) {
        self.options = options
        super.init()
        if options.selftest { selftest = SelfTest(app: self) }
    }

    func applicationDidFinishLaunching(_ n: Notification) {
        buildMenu()
        let screen = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        var size = NSSize(width: 1280, height: 720)
        let fit = min(1, min(screen.width * 0.85 / size.width, screen.height * 0.85 / size.height))
        size = NSSize(width: floor(size.width * fit), height: floor(size.height * fit))
        window = NSWindow(contentRect: NSRect(origin: .zero, size: size),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "Starbite Link"
        window.collectionBehavior = [.fullScreenPrimary]
        window.minSize = NSSize(width: 320, height: 200)
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.setFrameAutosaveName(options.selftest ? "" : "StarbiteLinkWindow")
        view = DisplayView(frame: NSRect(origin: .zero, size: size), options: options)
        window.contentView = view
        presenter.show = { [weak self] s, w, h in self?.view.show(surface: s, width: w, height: h) }
        view.onCaptureChange = { [weak self] _ in self?.updateStatus() }
        view.onResizeSettled = { [weak self] _ in self?.requestWindowMode() }
        view.onDisplaySizeChange = { [weak self] s in self?.fitWindow(toDisplay: s) }
        if options.selftest {
            // a fixed, known size so the selftest's pointer mapping is exact
            window.setContentSize(NSSize(width: options.modeTest.w, height: options.modeTest.h))
        }
        if options.selftest && !options.headless {
            window.orderFrontRegardless()      // visible, but leaves the keyboard focus where the user has it
        } else if !options.headless {
            window.center()
            window.makeKeyAndOrderFront(nil)
            window.makeFirstResponder(view)
            NSApp.activate(ignoringOtherApps: true)
        }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] e in
            guard let self = self, e.window === self.window, self.view.captured else { return e }
            return self.view.handleKey(e) ? nil : e
        }
        startLink()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.updateStatus() }
        updateStatus()
        selftest?.begin()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ n: Notification) {
        if let l = link { sbl_link_stop(l, UInt32(SBLK_BYE_SHUTDOWN)) }
        options.save()
    }

    // ---- link

    func desiredMode() -> (UInt16, UInt16) {
        let scale = options.hidpi ? (window.backingScaleFactor) : 1
        let b = view.bounds.size
        let w = max(640, min(3840, Int(b.width * scale))) & ~1
        let h = max(480, min(2160, Int(b.height * scale))) & ~1
        return (UInt16(w), UInt16(h))
    }

    func startLink() {
        var cfg = sbl_config()
        cfg.mode = Int32(options.mode)
        setCString(&cfg.host, options.host)
        cfg.port = options.port
        setCString(&cfg.name, computerName())
        if options.matchWindow && !options.selftest {
            let (w, h) = desiredMode()
            cfg.pref_w = w
            cfg.pref_h = h
        }
        cfg.refresh_mhz = UInt32((window.screen ?? NSScreen.main)?.maximumFramesPerSecond ?? 60) * 1000
        cfg.verbose = options.verbose ? 1 : 0
        var cbs = sbl_callbacks(ctx: Unmanaged.passUnretained(self).toOpaque(), frame: frameCallback, log: logCallback)
        link = sbl_link_create(&cfg, &cbs)
        view.link = link
        if let l = link { sbl_link_start(l) }
    }

    func restartLink() {
        view.setCaptured(false)
        view.link = nil
        if let l = link { sbl_link_destroy(l) }
        link = nil
        options.save()
        startLink()
        updateStatus()
    }

    /// The source changed the display's size by itself (Windows' orientation setting on the phone, a
    /// re-open on another output): give the window the display's shape, keeping about its area and its top
    /// edge, inside the screen. Not when the window drives the size (Display Follows Window Size with a
    /// source that takes MODE, i.e. the Starbite Display), in full screen, or in the selftest.
    func fitWindow(toDisplay px: CGSize) {
        guard !options.selftest, let w = window, !w.styleMask.contains(.fullScreen),
              px.width > 0, px.height > 0 else { return }
        if options.matchWindow && linkStatus().peer_caps & UInt32(SBLK_CAP_MODE) != 0 { return }
        let cur = view.bounds.size
        guard cur.width > 0, cur.height > 0 else { return }
        let aspect = px.width / px.height
        if abs(cur.width / cur.height - aspect) < 0.02 { return }
        var nw = (cur.width * cur.height * aspect).squareRoot(), nh = nw / aspect
        let vis = (w.screen ?? NSScreen.main)?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let chrome = w.frameRect(forContentRect: .zero).size
        let s = min(1, (vis.width - chrome.width) * 0.95 / nw, (vis.height - chrome.height) * 0.95 / nh)
        nw = floor(nw * s); nh = floor(nh * s)
        var f = w.frameRect(forContentRect: NSRect(x: 0, y: 0, width: nw, height: nh))
        let old = w.frame
        f.origin = NSPoint(x: old.midX - f.width / 2, y: old.maxY - f.height)
        f.origin.x = max(vis.minX, min(f.origin.x, vis.maxX - f.width))
        f.origin.y = max(vis.minY, min(f.origin.y, vis.maxY - f.height))
        w.setFrame(f, display: true, animate: false)
    }

    func requestWindowMode() {
        guard options.matchWindow, !options.selftest, let l = link else { return }
        let (w, h) = desiredMode()
        sbl_link_request_mode(l, w, h)
    }

    // ---- status overlay + title

    func linkStatus() -> sbl_status {
        var st = sbl_status()
        if let l = link { sbl_link_status(l, &st) }
        return st
    }

    func updateStatus() {
        let st = linkStatus()
        let connected = st.conn == Int32(SBL_CONN_RUNNING.rawValue)
        if connected != view.connected {
            view.connected = connected
            window.invalidateCursorRects(for: view)
        }
        if !connected && view.captured && st.conn != lastConn { view.setCaptured(false) }
        if st.conn != lastConn || view.captured != lastCaptured { overlayUntil = Date().addingTimeInterval(5) }
        lastConn = st.conn
        lastCaptured = view.captured
        let peer = cString(st.peer)
        let transport = cString(st.transport)
        let tcp = "TCP \(options.host):\(options.port)"
        var lines: [String] = []
        switch st.conn {
        case Int32(SBL_CONN_RUNNING.rawValue):
            let kind = st.peer_caps & UInt32(SBLK_CAP_EXTEND) != 0 ? "extended display" : "mirror"
            lines.append("\u{25CF} Connected  \(transport)  \u{00B7}  \(peer)  \u{00B7}  \(st.width)\u{00D7}\(st.height) \(kind)")
            lines.append(String(format: "%.1f fps  \u{00B7}  %.1f Mbit/s  \u{00B7}  RTT %.2f ms  \u{00B7}  lag %.1f ms",
                                st.fps, st.mbps, st.rtt_ms, st.lag_ms))
            lines.append(view.captured ? "Keyboard and pointer go to Windows  (Ctrl+Option+Cmd gives them back)"
                                       : "Click the display to control Windows")
            window.title = "Starbite Link \u{2014} \(peer)"
        case Int32(SBL_CONN_HANDSHAKE.rawValue):
            lines.append("\u{25D0} Connecting  \(transport)  (HELLO sent)")
            window.title = "Starbite Link"
        default:
            switch options.mode {
            case Int32(SBL_MODE_USB.rawValue):
                lines.append("\u{25CB} Waiting for the phone on USB (1209:0002)")
            case Int32(SBL_MODE_TCP.rawValue):
                lines.append("\u{25CB} Connecting to \(tcp) (retrying every second)")
            default:
                lines.append("\u{25CB} Waiting for the phone: USB 1209:0002, or \(tcp)")
            }
            if st.usb_present > 0 { lines.append("USB device attached; opening it") }
            else if options.mode != Int32(SBL_MODE_TCP.rawValue) {
                lines.append("Plug the S9 in with Windows running (approve the accessory if macOS asks)")
            }
            window.title = "Starbite Link"
        }
        let err = cString(st.last_error)
        if !connected && !err.isEmpty { lines.append("last error: \(err)") }
        let visible = !connected || options.overlay || Date() < overlayUntil
        view.setOverlay(visible ? lines.joined(separator: "\n") : nil)
    }

    // ---- window

    func windowDidResignKey(_ n: Notification) { view.setCaptured(false) }
    func windowDidEnterFullScreen(_ n: Notification) { requestWindowMode() }
    func windowDidExitFullScreen(_ n: Notification) { requestWindowMode() }

    // ---- menu

    private func item(_ title: String, _ sel: Selector?, _ key: String = "", _ mods: NSEvent.ModifierFlags = .command,
                      tag: Int = 0) -> NSMenuItem {
        let it = NSMenuItem(title: title, action: sel, keyEquivalent: key)
        it.keyEquivalentModifierMask = mods
        it.target = sel == nil ? nil : self
        it.tag = tag
        return it
    }

    private func buildMenu() {
        let main = NSMenu()
        func sub(_ title: String, _ items: [NSMenuItem]) {
            let top = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            let m = NSMenu(title: title)
            items.forEach { m.addItem($0) }
            top.submenu = m
            main.addItem(top)
        }
        let quit = NSMenuItem(title: "Quit Starbite Link", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let hide = NSMenuItem(title: "Hide Starbite Link", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let about = NSMenuItem(title: "About Starbite Link",
                               action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        sub("Starbite Link", [about, .separator(), hide, .separator(), quit])
        sub("Edit", [item("Type Clipboard Text into Windows", #selector(typeClipboard), "v", [.command, .shift])])
        let fs = NSMenuItem(title: "Toggle Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f")
        fs.keyEquivalentModifierMask = [.command, .control]
        sub("View", [fs, .separator(),
                     item("Always Show Status", #selector(toggleOverlay), "i"),
                     item("Display Follows Window Size", #selector(toggleMatch)),
                     item("Use Retina Pixels for the Display Size", #selector(toggleHiDPI)),
                     .separator(),
                     item("Request a Full Frame", #selector(requestKeyframe), "r", [.command, .shift])])
        sub("Link", [item("Automatic (USB, else TCP)", #selector(setMode(_:)), tag: Int(SBL_MODE_AUTO.rawValue)),
                     item("USB Only", #selector(setMode(_:)), tag: Int(SBL_MODE_USB.rawValue)),
                     item("TCP Only", #selector(setMode(_:)), tag: Int(SBL_MODE_TCP.rawValue)),
                     .separator(),
                     item("TCP Address\u{2026}", #selector(askHost)),
                     item("Reconnect", #selector(reconnect), "r")])
        sub("Input", [item("Cmd Is the Windows Key", #selector(setCmd(_:)), tag: 0),
                      item("Cmd Is Ctrl (Ctrl Is the Windows Key)", #selector(setCmd(_:)), tag: 1),
                      .separator(),
                      item("Show the Mac Pointer over the Display", #selector(toggleCursor))])
        let win = NSMenu(title: "Window")
        win.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        let winTop = NSMenuItem(title: "Window", action: nil, keyEquivalent: "")
        winTop.submenu = win
        main.addItem(winTop)
        NSApp.mainMenu = main
        NSApp.windowsMenu = win
    }

    @objc func validateMenuItem(_ it: NSMenuItem) -> Bool {
        switch it.action {
        case #selector(toggleOverlay): it.state = options.overlay ? .on : .off
        case #selector(toggleMatch): it.state = options.matchWindow ? .on : .off
        case #selector(toggleHiDPI): it.state = options.hidpi ? .on : .off
        case #selector(toggleCursor): it.state = options.showCursor ? .on : .off
        case #selector(setMode(_:)): it.state = Int32(it.tag) == options.mode ? .on : .off
        case #selector(setCmd(_:)): it.state = (it.tag == 1) == options.cmdAsCtrl ? .on : .off
        default: break
        }
        return true
    }

    @objc func typeClipboard() {
        guard let s = NSPasteboard.general.string(forType: .string), !s.isEmpty, let l = link else { return }
        sbl_link_text(l, s)
    }

    @objc func toggleOverlay() { options.overlay.toggle(); view.options = options; options.save(); updateStatus() }
    @objc func toggleMatch() { options.matchWindow.toggle(); options.save(); requestWindowMode() }
    @objc func toggleHiDPI() { options.hidpi.toggle(); options.save(); requestWindowMode() }
    @objc func toggleCursor() {
        options.showCursor.toggle(); view.options = options; options.save()
        window.invalidateCursorRects(for: view)
    }
    @objc func requestKeyframe() { if let l = link { sbl_link_request_keyframe(l) } }
    @objc func setMode(_ it: NSMenuItem) { options.mode = Int32(it.tag); restartLink() }
    @objc func setCmd(_ it: NSMenuItem) { view.releaseAll(); options.cmdAsCtrl = it.tag == 1; view.options = options; options.save() }
    // Reconnect also resets the USB device: the phone then gets fresh bus events (Reset, Configured), which a
    // sender that lost them (round 7: a new helper after a sign-out) needs; an interface reopen alone cannot help
    @objc func reconnect() {
        restartLink()
        if let l = link { sbl_link_reset_usb(l) }
    }

    @objc func askHost() {
        let a = NSAlert()
        a.messageText = "TCP address of the phone's link service"
        a.informativeText = "HOST or HOST:PORT (default port \(SBLK_TCP_PORT))."
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        f.stringValue = "\(options.host):\(options.port)"
        a.accessoryView = f
        a.addButton(withTitle: "Connect")
        a.addButton(withTitle: "Cancel")
        if a.runModal() == .alertFirstButtonReturn {
            options.parseHost(f.stringValue.trimmingCharacters(in: .whitespaces))
            if options.mode == Int32(SBL_MODE_USB.rawValue) { options.mode = Int32(SBL_MODE_AUTO.rawValue) }
            restartLink()
        }
    }
}

// ---- headless selftest ------------------------------------------------------------------------------------

/// --selftest: connect (TCP) to the fake sender, receive frames, ask for a mode change, inject synthetic
/// keyboard / pointer / wheel / text input through the same handlers real events use, wait for a PONG (so
/// the sender has processed the input), then compare the composed image (frame buffer AND the IOSurface the
/// layer shows) with the sender's own checksum for that frame id. Exit 0 = pass.
final class SelfTest {
    unowned let app: AppController
    private var count = 0                 // link queue
    private var firstFrame = 0.0, lastFrame = 0.0
    private let lock = NSLock()
    private var armed = false
    private var done = false
    private let t0 = Date()

    init(app: AppController) { self.app = app }

    func say(_ s: String) { print("SELFTEST \(s)"); fflush(stdout) }

    func begin() {
        say("start: \(cString(app.linkStatus().transport)) frames=\(app.options.frames) mode=\(app.options.modeTest)")
        DispatchQueue.main.asyncAfter(deadline: .now() + app.options.timeout) { [weak self] in
            guard let self = self else { return }
            let st = self.app.linkStatus()
            self.finish(false, "timeout after \(self.app.options.timeout) s: frames=\(st.frames) sessions=\(st.sessions) " +
                        "conn=\(st.conn) error=\"\(cString(st.last_error))\"")
        }
    }

    /// link queue
    func onFrame(_ fi: UnsafePointer<sbv_frame_info>) {
        count += 1
        lastFrame = CACurrentMediaTime()
        if count == 1 { firstFrame = lastFrame }
        let f = fi.pointee
        if count == 30 { DispatchQueue.main.async { self.requestMode() } }
        if count == 50 { DispatchQueue.main.async { self.injectInput() } }
        lock.lock()
        let want = app.options.finalSize ?? app.options.modeTest
        let go = armed && count >= app.options.frames && Int(f.width) == want.w && Int(f.height) == want.h &&
            f.valid != 0
        if go { armed = false }
        // the IOSurface checksum costs a full pass: only for the frames that can be the checked one
        let hashNext = armed && count + 1 >= app.options.frames
        lock.unlock()
        app.presenter.setHashSurfaces(hashNext)
        guard go else { return }
        app.presenter.freeze()
        let fbHash = sbv_fb_hash(f.fb, f.stride, f.width, f.height)
        let surfHash = app.presenter.lastSurfaceHash
        let expected = lookup(frameId: f.frame_id)
        if let path = app.options.snapshot { snapshot(f, path) }
        let fid = f.frame_id, w = f.width, h = f.height
        var fbCopy = [UInt8](repeating: 0, count: Int(w) * Int(h) * 4)
        for y in 0..<Int(h) { fbCopy.withUnsafeMutableBufferPointer { _ = memcpy($0.baseAddress! + y * Int(w) * 4, f.fb + y * Int(f.stride), Int(w) * 4) } }
        let avg = Double(count - 1) / max(lastFrame - firstFrame, 0.001)
        DispatchQueue.main.async {
            self.say(String(format: "average %.1f frames/s over %d frames", avg, self.count))
            let hex = { (v: UInt64) in String(format: "%016llx", v) }
            self.say("frame \(fid) (\(self.count) received) \(w)x\(h): fb \(hex(fbHash)) surface \(hex(surfHash)) " +
                     "sender \(expected.map(hex) ?? "missing")")
            // what Core Animation draws from the layer (presenter frozen on this frame; its show() already ran)
            var layerOK = false
            let cr = self.app.view.contentRect()
            if cr != CGRect(x: 0, y: 0, width: CGFloat(w), height: CGFloat(h)) {
                layerOK = true   // scaled (window smaller than the display): no bit-exact comparison possible
                self.say("layer render not compared: the display is scaled into \(cr)")
            } else if let px = self.app.view.renderDisplayLayer(Int(w), Int(h)) {
                let lh = px.withUnsafeBufferPointer { sbv_fb_hash($0.baseAddress, Int(w) * 4, w, h) }
                layerOK = lh == fbHash
                if !layerOK { self.say("layer render \(hex(lh)) differs: \(self.diffCount(px, fbCopy, Int(w), Int(h))) pixels") }
            }
            self.say("layer render (what the window shows) \(layerOK ? "matches" : "DIFFERS")")
            if let path = self.app.options.windowSnapshot, let img = self.app.view.renderView() { self.writePNG(img, path) }
            let ok = expected == fbHash && surfHash == fbHash && layerOK
            self.finish(ok, ok ? "composed image matches the sender" : "image mismatch")
        }
    }

    private func requestMode() {
        guard let l = app.link else { return }
        say("MODE request \(app.options.modeTest.w)x\(app.options.modeTest.h)")
        sbl_link_request_mode(l, UInt16(app.options.modeTest.w), UInt16(app.options.modeTest.h))
    }

    private func injectInput() {
        guard let l = app.link else { return }
        let v = app.view!, wn = app.window.windowNumber
        v.setCaptured(true)
        let ts = ProcessInfo.processInfo.systemUptime
        func key(_ type: NSEvent.EventType, _ code: UInt16, _ flags: NSEvent.ModifierFlags, _ ch: String) {
            guard let e = NSEvent.keyEvent(with: type, location: .zero, modifierFlags: flags, timestamp: ts,
                                           windowNumber: wn, context: nil, characters: ch,
                                           charactersIgnoringModifiers: ch, isARepeat: false, keyCode: code)
            else { say("could not make a key event"); return }
            _ = v.handleKey(e)
        }
        let shift = NSEvent.ModifierFlags(rawValue: NSEvent.ModifierFlags.shift.rawValue | 0x2)
        let cmd = NSEvent.ModifierFlags(rawValue: NSEvent.ModifierFlags.command.rawValue | 0x8)
        key(.flagsChanged, 0x38, shift, "")         // left Shift down
        key(.keyDown, 0x04, shift, "H"); key(.keyUp, 0x04, shift, "H")
        key(.keyDown, 0x22, shift, "I"); key(.keyUp, 0x22, shift, "I")
        key(.flagsChanged, 0x38, [], "")            // left Shift up
        key(.keyDown, 0x31, [], " "); key(.keyUp, 0x31, [], " ")
        key(.flagsChanged, 0x37, cmd, "")           // left Cmd down: the Windows key
        key(.flagsChanged, 0x37, [], "")
        func mouse(_ type: NSEvent.EventType, _ x: CGFloat, _ y: CGFloat) -> NSEvent {
            let loc = v.convert(v.displayToView(x, y), to: nil)
            return NSEvent.mouseEvent(with: type, location: loc, modifierFlags: [], timestamp: ts, windowNumber: wn,
                                      context: nil, eventNumber: 0,
                                      clickCount: type == .mouseMoved ? 0 : 1, pressure: type == .mouseMoved ? 0 : 1)!
        }
        v.mouseMoved(with: mouse(.mouseMoved, 100.5, 200.5))
        v.mouseDown(with: mouse(.leftMouseDown, 100.5, 200.5))
        v.mouseUp(with: mouse(.leftMouseUp, 100.5, 200.5))
        v.rightMouseDown(with: mouse(.rightMouseDown, 300.5, 150.5))
        v.rightMouseUp(with: mouse(.rightMouseUp, 300.5, 150.5))
        if let cg = CGEvent(scrollWheelEvent2Source: nil, units: .line, wheelCount: 1, wheel1: -1, wheel2: 0, wheel3: 0),
           let e = NSEvent(cgEvent: cg) {
            v.scrollWheel(with: e)
        }
        sbl_link_text(l, "Gr\u{00FC}\u{00DF}e \u{2713}")
        say("input injected: Shift+H, Shift+I, Space, Cmd, left click (100,200), right click (300,150), wheel, text")
        // everything above is queued on the link queue in order; a PONG for a PING sent after it proves the
        // sender has read it all
        DispatchQueue.global().async {
            sbl_link_flush(l)
            let before = self.app.linkStatus().pongs
            sbl_link_ping(l)
            self.waitPong(before, tries: 100)
        }
    }

    private func waitPong(_ before: UInt64, tries: Int) {
        let st = app.linkStatus()
        if st.pongs > before {
            say("PONG after the input (rtt \(String(format: "%.2f", st.rtt_ms)) ms): checking the next frame from \(app.options.frames)")
            lock.lock(); armed = true; lock.unlock()
            return
        }
        if tries == 0 { DispatchQueue.main.async { self.finish(false, "no PONG after the input") }; return }
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.05) { self.waitPong(before, tries: tries - 1) }
    }

    private func lookup(frameId: UInt32) -> UInt64? {
        guard let path = app.options.crcFile, let text = try? String(contentsOfFile: path, encoding: .utf8) else { return nil }
        // "<id> <W>x<H> <hash>"; ids restart with each session: the last line with this id is this session's
        for line in text.split(separator: "\n").reversed() {
            let p = line.split(separator: " ")
            if p.count == 3, UInt32(p[0]) == frameId { return UInt64(p[2], radix: 16) }
        }
        return nil
    }

    func diffCount(_ a: [UInt8], _ b: [UInt8], _ w: Int, _ h: Int) -> Int {
        var n = 0
        for i in 0..<(w * h) where a[i * 4] != b[i * 4] || a[i * 4 + 1] != b[i * 4 + 1] || a[i * 4 + 2] != b[i * 4 + 2] { n += 1 }
        return n
    }

    func writePNG(_ img: CGImage, _ path: String) {
        guard let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(d, img, nil)
        CGImageDestinationFinalize(d)
    }

    private func snapshot(_ f: sbv_frame_info, _ path: String) {
        let w = Int(f.width), h = Int(f.height)
        guard let cs = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return }
        let dst = ctx.data!.assumingMemoryBound(to: UInt8.self)
        for y in 0..<h { memcpy(dst + y * ctx.bytesPerRow, f.fb + y * Int(f.stride), w * 4) }
        guard let img = ctx.makeImage(),
              let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)
        else { return }
        CGImageDestinationAddImage(d, img, nil)
        CGImageDestinationFinalize(d)
    }

    func finish(_ ok: Bool, _ why: String) {
        if done { return }
        done = true
        let st = app.linkStatus()
        say(String(format: "%@: %@ (%.1f s, %llu frames, %llu session(s), %.1f fps, %.1f Mbit/s at the end)",
                   ok ? "PASS" : "FAIL", why, Date().timeIntervalSince(t0), st.frames, st.sessions, st.fps, st.mbps))
        if let l = app.link { sbl_link_stop(l, UInt32(SBLK_BYE_NORMAL)) }
        exit(ok ? 0 : 1)
    }
}

// ---- main ----------------------------------------------------------------------------------------------------

// a selftest must not depend on what the menus remembered
var options = CommandLine.arguments.contains("--selftest") ? Options() : Options.load()
if let err = options.parseArgs(CommandLine.arguments) {
    FileHandle.standardError.write((err + "\n").data(using: .utf8)!)
    exit(2)
}
if options.selftest {
    if options.crcFile == nil { FileHandle.standardError.write("--selftest needs --crc-file\n".data(using: .utf8)!); exit(2) }
}
let nsapp = NSApplication.shared
nsapp.setActivationPolicy(options.headless ? .accessory : .regular)
let controller = AppController(options: options)
nsapp.delegate = controller
nsapp.run()
