// SPDX-License-Identifier: GPL-2.0-only
// Starbite Link, Mac viewer: the Starbite Display view.
//
// Presenter: the link queue applies RECTs into the session's frame buffer (sbv); on FRAME_END the
// presenter copies only the damaged areas into one of three IOSurfaces and hands it to a CALayer
// (contentsGravity resizeAspect: scaled to fit, aspect kept). Each surface remembers what it missed, so a
// frame costs a copy of its damage, nothing is redrawn when nothing changed, and the surface on screen is
// never written while Core Animation shows it.
//
// DisplayView: hosts the layers and turns Mac keyboard / mouse / trackpad input into SBLINK input.

import AppKit
import IOSurface
import QuartzCore
import SBLinkC

final class Presenter {
    private let lock = NSLock()
    private var surfaces: [IOSurfaceRef] = []
    private var stale: [[sblk_rect]] = []     // per surface: areas it has not received yet
    private var staleAll: [Bool] = []
    private var displayed = -1, pending = -1, scheduled = false
    private(set) var width = 0, height = 0
    /// main thread: show this surface (w, h in pixels)
    var show: ((IOSurfaceRef, Int, Int) -> Void)?
    /// selftest: hash the surface written for each frame (only while the final check is armed)
    private var hashSurfaces = false
    func setHashSurfaces(_ on: Bool) { lock.lock(); hashSurfaces = on; lock.unlock() }
    /// selftest: keep the surface of the checked frame on screen (later frames are ignored)
    private var frozen = false
    func freeze() { lock.lock(); frozen = true; lock.unlock() }
    private(set) var lastSurfaceHash: UInt64 = 0

    private static let maxStale = 32

    private func makeSurface(_ w: Int, _ h: Int) -> IOSurfaceRef? {
        let bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, w * 4)
        let props: [CFString: Any] = [
            kIOSurfaceWidth: w, kIOSurfaceHeight: h, kIOSurfaceBytesPerElement: 4,
            kIOSurfaceBytesPerRow: bpr, kIOSurfacePixelFormat: 0x4247_5241,  // 'BGRA'
        ]
        guard let s = IOSurfaceCreate(props as CFDictionary) else { return nil }
        if let cs = CGColorSpace(name: CGColorSpace.sRGB), let pl = cs.copyPropertyList() {
            IOSurfaceSetValue(s, kIOSurfaceColorSpace, pl)
        }
        return s
    }

    private func addStale(_ i: Int, _ r: sblk_rect) {
        if staleAll[i] { return }
        stale[i].append(r)
        if stale[i].count > Presenter.maxStale {
            var x0 = Int.max, y0 = Int.max, x1 = 0, y1 = 0
            for d in stale[i] {
                x0 = min(x0, Int(d.x)); y0 = min(y0, Int(d.y))
                x1 = max(x1, Int(d.x) + Int(d.w)); y1 = max(y1, Int(d.y) + Int(d.h))
            }
            stale[i] = [sblk_rect(x: UInt16(x0), y: UInt16(y0), w: UInt16(x1 - x0), h: UInt16(y1 - y0))]
        }
    }

    /// Link queue, from the frame callback: fi.fb is valid only during this call.
    func frame(_ fi: UnsafePointer<sbv_frame_info>) {
        let f = fi.pointee
        let w = Int(f.width), h = Int(f.height)
        lock.lock()
        if frozen { lock.unlock(); return }
        if f.resized != 0 || w != width || h != height || surfaces.isEmpty {
            surfaces = (0..<3).compactMap { _ in makeSurface(w, h) }
            if surfaces.count != 3 { surfaces = []; lock.unlock(); return }
            stale = [[], [], []]
            staleAll = [true, true, true]
            width = w; height = h
            displayed = -1; pending = -1
        } else {
            // a frame with no rects (timing / heartbeat) changes nothing: no copy, no new layer contents
            if f.ndamage == 0 && !hashSurfaces && (displayed >= 0 || pending >= 0) { lock.unlock(); return }
            for i in 0..<3 {
                for k in 0..<Int(f.ndamage) { addStale(i, f.damage[k]) }
            }
        }
        // the pending surface is not on screen yet: write it again; else any surface not on screen,
        // preferring one the window server no longer uses
        var target = pending
        if target < 0 {
            let candidates = (0..<3).filter { $0 != displayed }
            target = candidates.first(where: { !IOSurfaceIsInUse(surfaces[$0]) }) ?? candidates[0]
        }
        let s = surfaces[target]
        IOSurfaceLock(s, [], nil)
        let base = IOSurfaceGetBaseAddress(s).assumingMemoryBound(to: UInt8.self)
        let bpr = IOSurfaceGetBytesPerRow(s)
        let src = f.fb!
        let sstride = Int(f.stride)
        func copy(_ x: Int, _ y: Int, _ rw: Int, _ rh: Int) {
            for row in y..<(y + rh) {
                memcpy(base + row * bpr + x * 4, src + row * sstride + x * 4, rw * 4)
            }
        }
        if staleAll[target] {
            copy(0, 0, w, h)
        } else {
            for r in stale[target] { copy(Int(r.x), Int(r.y), Int(r.w), Int(r.h)) }
        }
        if hashSurfaces { lastSurfaceHash = sbv_fb_hash(base, bpr, UInt32(w), UInt32(h)) }
        IOSurfaceUnlock(s, [], nil)
        stale[target] = []
        staleAll[target] = false
        pending = target
        let needSchedule = !scheduled
        scheduled = true
        lock.unlock()
        if needSchedule { DispatchQueue.main.async { self.presentPending() } }
    }

    private func presentPending() {
        lock.lock()
        scheduled = false
        guard pending >= 0 else { lock.unlock(); return }
        let s = surfaces[pending]
        displayed = pending
        pending = -1
        let w = width, h = height
        lock.unlock()
        show?(s, w, h)
    }
}

final class DisplayView: NSView {
    let displayLayer = CALayer()
    let overlayBox = CALayer()
    let overlayLayer = CATextLayer()
    var link: OpaquePointer?
    var options: Options
    /// display size in pixels (frame buffer), main thread
    private(set) var displaySize = CGSize(width: 1280, height: 720)
    var connected = false
    var onCaptureChange: ((Bool) -> Void)?
    var onResizeSettled: ((CGSize) -> Void)?

    private(set) var captured = false
    private var buttons: UInt32 = 0
    private var lastSent = (x: Int32.min, y: Int32.min)
    private var pendingPos: (x: Int32, y: Int32)? = nil
    private var moveScheduled = false
    private var lastMoveTime: CFTimeInterval = 0
    private var wheelAccX = 0.0, wheelAccY = 0.0
    private var keysDown = Set<UInt16>()   // mac key codes reported down (non-modifier)
    private var modsDown = Set<UInt16>()   // modifier key codes reported down
    private var resizeWork: DispatchWorkItem?
    private lazy var blankCursor: NSCursor = {
        let img = NSImage(size: NSSize(width: 1, height: 1))
        return NSCursor(image: img, hotSpot: .zero)
    }()

    init(frame: NSRect, options: Options) {
        self.options = options
        super.init(frame: frame)
        let root = CALayer()
        root.backgroundColor = NSColor.black.cgColor
        layer = root
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        displayLayer.contentsGravity = .resizeAspect
        displayLayer.magnificationFilter = .linear
        displayLayer.minificationFilter = .trilinear
        displayLayer.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull()]
        root.addSublayer(displayLayer)
        overlayLayer.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull(), "string": NSNull(),
                                "hidden": NSNull()]
        overlayBox.actions = overlayLayer.actions
        overlayBox.backgroundColor = NSColor(white: 0, alpha: 0.62).cgColor
        overlayBox.cornerRadius = 7
        overlayLayer.isWrapped = false
        overlayLayer.alignmentMode = .left
        overlayBox.addSublayer(overlayLayer)
        root.addSublayer(overlayBox)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override var isOpaque: Bool { true }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        displayLayer.frame = bounds
        layoutOverlay()
        CATransaction.commit()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        let s = window?.backingScaleFactor ?? 2
        overlayLayer.contentsScale = s
        displayLayer.contentsScale = s
    }

    // ---- presenting

    func show(surface: IOSurfaceRef, width: Int, height: Int) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        displayLayer.contents = surface
        // crisp at 1:1, smooth when scaled
        let r = contentRect()
        let scale = (window?.backingScaleFactor ?? 2) * r.width / CGFloat(max(width, 1))
        displayLayer.magnificationFilter = abs(scale - 1) < 0.01 ? .nearest : .linear
        CATransaction.commit()
        let newSize = CGSize(width: width, height: height)
        if newSize != displaySize {
            displaySize = newSize
            window?.invalidateCursorRects(for: self)
        }
    }

    /// selftest: what Core Animation draws for the display layer, rendered offscreen at 1 pixel per point
    /// (BGRA, top row first, rows of w*4 bytes); renderView(): the whole view (display + overlay)
    func renderDisplayLayer(_ w: Int, _ h: Int) -> [UInt8]? {
        guard let cs = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        displayLayer.render(in: ctx)     // the caller checks that the display is drawn 1:1 at (0,0)
        let p = ctx.data!.assumingMemoryBound(to: UInt8.self)
        return Array(UnsafeBufferPointer(start: p, count: w * h * 4))
    }

    func renderView() -> CGImage? {
        let w = Int(bounds.width), h = Int(bounds.height)
        guard w > 0, h > 0, let cs = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: cs,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return nil }
        layer?.render(in: ctx)
        return ctx.makeImage()
    }

    // ---- overlay

    func setOverlay(_ text: String?) {
        guard let text = text else {
            overlayBox.isHidden = true
            return
        }
        let font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        let attr = NSAttributedString(string: text, attributes: [.font: font, .foregroundColor: NSColor.white])
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        overlayLayer.string = attr
        overlayBox.isHidden = false
        layoutOverlay()
        CATransaction.commit()
    }

    private func layoutOverlay() {
        guard let attr = overlayLayer.string as? NSAttributedString else { return }
        let sz = attr.size()
        let pad: CGFloat = 8
        let w = ceil(sz.width) + 2 * pad, h = ceil(sz.height) + 2 * pad
        overlayBox.frame = CGRect(x: 10, y: bounds.height - h - 10, width: w, height: h)
        overlayLayer.frame = CGRect(x: pad, y: pad, width: ceil(sz.width) + 1, height: ceil(sz.height))
        overlayLayer.contentsScale = window?.backingScaleFactor ?? 2
    }

    // ---- geometry: view points <-> display pixels

    /// where the display is drawn inside the view (aspect fit, centred), view coordinates
    func contentRect() -> CGRect {
        let fw = max(displaySize.width, 1), fh = max(displaySize.height, 1)
        let s = min(bounds.width / fw, bounds.height / fh)
        let w = fw * s, h = fh * s
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }

    func viewToDisplay(_ p: CGPoint) -> (x: Int32, y: Int32) {
        let r = contentRect()
        let x = (p.x - r.minX) / r.width * displaySize.width
        let y = (r.maxY - p.y) / r.height * displaySize.height   // AppKit y up, display y down
        return (Int32(clamping: Int(floor(x))), Int32(clamping: Int(floor(y))))
    }

    func displayToView(_ x: CGFloat, _ y: CGFloat) -> CGPoint {
        let r = contentRect()
        return CGPoint(x: r.minX + x / displaySize.width * r.width, y: r.maxY - y / displaySize.height * r.height)
    }

    // ---- capture

    func setCaptured(_ on: Bool) {
        if on == captured { return }
        captured = on
        if !on { releaseAll() }
        window?.invalidateCursorRects(for: self)
        onCaptureChange?(on)
    }

    override func resetCursorRects() {
        if captured && connected && !options.showCursor {
            addCursorRect(contentRect(), cursor: blankCursor)   // only for a source that draws its pointer in the frames
        }
    }

    var lockBits: UInt16 {
        // Caps Lock follows the Mac; Num Lock is reported on (a Mac keypad always types digits)
        (NSEvent.modifierFlags.contains(.capsLock) ? UInt16(SBLK_MOD_CAPS) : 0) | UInt16(SBLK_MOD_NUM)
    }

    func releaseAll() {
        keysDown.removeAll()
        modsDown.removeAll()
        if buttons != 0 { buttons = 0 }
        if let l = link { sbl_link_release_all(l, lockBits) }
    }

    // ---- keyboard

    /// Called by the app's local event monitor for every key event of this window. true = consumed.
    func handleKey(_ e: NSEvent) -> Bool {
        guard captured, let l = link else { return false }
        let cmdAsCtrl = options.cmdAsCtrl
        switch e.type {
        case .keyDown, .keyUp:
            let down = e.type == .keyDown
            if let (page, usage) = KeyMap.hid(e.keyCode, cmdAsCtrl: cmdAsCtrl) {
                if down { keysDown.insert(e.keyCode) } else { keysDown.remove(e.keyCode) }
                // auto-repeat: Windows does not repeat injected keys, so repeated downs are forwarded
                sbl_link_key(l, page, usage, down ? 1 : 0, lockBits)
            } else if down, let s = e.characters, !s.isEmpty, s.unicodeScalars.allSatisfy({ $0.value >= 0x20 }) {
                sbl_link_text(l, s)   // a key with no HID position: type its text
            }
            return true
        case .flagsChanged:
            if e.keyCode == 0x39 {   // Caps Lock: macOS reports each toggle once; Windows toggles on a press
                sbl_link_key(l, 0x07, 0x39, 1, lockBits)
                sbl_link_key(l, 0x07, 0x39, 0, lockBits)
                return true
            }
            guard let bits = KeyMap.modifierBits(e.keyCode),
                  let (page, usage) = KeyMap.hid(e.keyCode, cmdAsCtrl: cmdAsCtrl) else { return true }
            let raw = e.modifierFlags.rawValue
            let down: Bool
            if raw & 0x20FF != 0 {
                down = raw & bits.device != 0
            } else {
                down = raw & bits.flag.rawValue != 0     // synthesized events without device bits
            }
            let was = modsDown.contains(e.keyCode)
            if down == was { return true }
            if down { modsDown.insert(e.keyCode) } else { modsDown.remove(e.keyCode) }
            sbl_link_key(l, page, usage, down ? 1 : 0, lockBits)
            // Ctrl + Option + Cmd together (and nothing else) gives the keyboard and pointer back to the Mac
            let f = e.modifierFlags
            if down && f.contains(.control) && f.contains(.option) && f.contains(.command) && keysDown.isEmpty {
                setCaptured(false)
            }
            return true
        default:
            return false
        }
    }

    override func keyDown(with event: NSEvent) { if !handleKey(event) { /* not captured: ignore, no beep */ } }
    override func keyUp(with event: NSEvent) { _ = handleKey(event) }
    override func flagsChanged(with event: NSEvent) { _ = handleKey(event) }

    // ---- pointer

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for t in trackingAreas { removeTrackingArea(t) }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    private func point(_ e: NSEvent) -> (x: Int32, y: Int32) {
        viewToDisplay(convert(e.locationInWindow, from: nil))
    }

    private func sendPointer(_ p: (x: Int32, y: Int32), wheel: (Int16, Int16) = (0, 0)) {
        guard let l = link else { return }
        pendingPos = nil
        lastSent = p
        lastMoveTime = CACurrentMediaTime()
        sbl_link_pointer(l, p.x, p.y, buttons, wheel.0, wheel.1)
    }

    /// moves are coalesced to one per display refresh; buttons and wheel go at once
    private func moved(_ e: NSEvent) {
        guard captured else { return }
        let p = point(e)
        if p == lastSent && pendingPos == nil { return }
        pendingPos = p
        if moveScheduled { return }
        let hz = Double(max(window?.screen?.maximumFramesPerSecond ?? 60, 30))
        let wait = max(0, 1.0 / hz - (CACurrentMediaTime() - lastMoveTime))
        moveScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + wait) { [weak self] in
            guard let self = self else { return }
            self.moveScheduled = false
            if let p = self.pendingPos { self.sendPointer(p) }
        }
    }

    private func button(_ e: NSEvent, _ bit: UInt32, _ down: Bool) {
        if down && !captured {
            window?.makeFirstResponder(self)
            setCaptured(true)
        }
        guard captured else { return }
        if down { buttons |= bit } else { buttons &= ~bit }
        sendPointer(point(e))
    }

    private func otherBit(_ e: NSEvent) -> UInt32 {
        switch e.buttonNumber {
        case 2: return UInt32(SBLK_BTN_MIDDLE)
        case 3: return UInt32(SBLK_BTN_BACK)
        case 4: return UInt32(SBLK_BTN_FORWARD)
        default: return 0
        }
    }

    override func mouseMoved(with e: NSEvent) { moved(e) }
    override func mouseDragged(with e: NSEvent) { moved(e) }
    override func rightMouseDragged(with e: NSEvent) { moved(e) }
    override func otherMouseDragged(with e: NSEvent) { moved(e) }
    override func mouseDown(with e: NSEvent) { button(e, UInt32(SBLK_BTN_LEFT), true) }
    override func mouseUp(with e: NSEvent) { button(e, UInt32(SBLK_BTN_LEFT), false) }
    override func rightMouseDown(with e: NSEvent) { button(e, UInt32(SBLK_BTN_RIGHT), true) }
    override func rightMouseUp(with e: NSEvent) { button(e, UInt32(SBLK_BTN_RIGHT), false) }
    override func otherMouseDown(with e: NSEvent) { button(e, otherBit(e), true) }
    override func otherMouseUp(with e: NSEvent) { button(e, otherBit(e), false) }

    override func scrollWheel(with e: NSEvent) {
        guard captured else { return }
        // Windows: 120 per notch, + = away from the user / right. AppKit deltas already include the
        // user's "natural scrolling" choice; precise (trackpad) deltas are points: 40 points per notch.
        let k = e.hasPreciseScrollingDeltas ? 3.0 : 120.0
        wheelAccY += Double(e.scrollingDeltaY) * k
        wheelAccX -= Double(e.scrollingDeltaX) * k
        let dy = Int16(clamping: Int(wheelAccY.rounded(.towardZero)))
        let dx = Int16(clamping: Int(wheelAccX.rounded(.towardZero)))
        wheelAccY -= Double(dy)
        wheelAccX -= Double(dx)
        if dx == 0 && dy == 0 { return }
        let p = pendingPos ?? (lastSent.x == Int32.min ? point(e) : lastSent)
        sendPointer(p, wheel: (dx, dy))
    }

    // ---- resize: ask the source for a matching mode once the size settles

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        if inLiveResize { return }
        scheduleResizeSettled()
    }

    override func viewDidEndLiveResize() {
        super.viewDidEndLiveResize()
        scheduleResizeSettled()
    }

    private func scheduleResizeSettled() {
        resizeWork?.cancel()
        let w = DispatchWorkItem { [weak self] in
            guard let self = self else { return }
            self.onResizeSettled?(self.bounds.size)
        }
        resizeWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: w)
    }
}
