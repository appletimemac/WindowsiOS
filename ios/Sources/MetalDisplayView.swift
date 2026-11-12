// SPDX-License-Identifier: GPL-2.0-or-later
import Metal
import QuartzCore
import SwiftUI
import UIKit

final class MetalCanvasUIView: UIView, UIKeyInput {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
    var trackpadMode: Bool = false
    var rightClickArmed: Bool = false
    var onRightClickConsumed: (() -> Void)?
    var onThreeFingerTap: (() -> Void)?

    private var activeDragButtons: UInt8 = 0
    private var panLastPoint: CGPoint = .zero
    private var scrollAccum: CGFloat = 0
    private let tapHaptic = UIImpactFeedbackGenerator(style: .light)

    /// Touch mode drives the guest's HID touchscreen with raw touches once Windows has configured
    /// it; trackpad mode and the time before that keep the gesture-driven absolute pointer.
    private var digitizerActive = false
    /// Touches Windows knows about, with the HID contact id each was given.
    private var contacts: [ObjectIdentifier: Contact] = [:]
    /// A gesture's first fingers, not yet sent to Windows (see HeldTaps), and their touches.
    private var held = HeldTaps()
    private var heldTouches: [ObjectIdentifier: UITouch] = [:]
    private var heldRelease: DispatchWorkItem?
    /// Quick taps already pressed in Windows whose lift is still to be sent, by contact id.
    private var tapsInFlight: [UInt8: (x: UInt16, y: UInt16)] = [:]
    private var pendingSizeRequest: DispatchWorkItem?
    private var requestedGuestSize: CGSize = .zero
    /// Guest pixels per point: on a 3x panel this is exactly 2 device pixels per Windows pixel.
    private static let guestPixelsPerPoint: CGFloat = 1.5
    private static let maxContacts = 5

    private struct Contact {
        let touch: UITouch
        let slot: UInt8
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        backgroundColor = .black
        isMultipleTouchEnabled = true
        isUserInteractionEnabled = true

        metalLayer.device = MTLCreateSystemDefaultDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.contentsGravity = .resizeAspect
        updateDrawableScale()

        let singleTap = UITapGestureRecognizer(target: self, action: #selector(handleSingleTap(_:)))
        singleTap.numberOfTouchesRequired = 1
        addGestureRecognizer(singleTap)

        let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(handleTwoFingerTap(_:)))
        twoFingerTap.numberOfTouchesRequired = 2
        addGestureRecognizer(twoFingerTap)

        let threeFingerTap = UITapGestureRecognizer(target: self, action: #selector(handleThreeFingerTap(_:)))
        threeFingerTap.numberOfTouchesRequired = 3
        addGestureRecognizer(threeFingerTap)

        let dragPan = UIPanGestureRecognizer(target: self, action: #selector(handleOneFingerPan(_:)))
        dragPan.minimumNumberOfTouches = 1
        dragPan.maximumNumberOfTouches = 1
        addGestureRecognizer(dragPan)

        let scrollPan = UIPanGestureRecognizer(target: self, action: #selector(handleTwoFingerScroll(_:)))
        scrollPan.minimumNumberOfTouches = 2
        scrollPan.maximumNumberOfTouches = 2
        if #available(iOS 13.4, *) {
            scrollPan.allowedScrollTypesMask = .all
        }
        addGestureRecognizer(scrollPan)

        let longPress = UILongPressGestureRecognizer(target: self, action: #selector(handleLongPress(_:)))
        longPress.minimumPressDuration = 0.45
        longPress.allowableMovement = 12
        addGestureRecognizer(longPress)

        if #available(iOS 13.4, *) {
            let hover = UIHoverGestureRecognizer(target: self, action: #selector(handleHover(_:)))
            addGestureRecognizer(hover)
        }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        updateDrawableScale()
        if window != nil {
            WinIOSBridge.shared.bindMetalLayer(metalLayer)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateDrawableScale()
        requestGuestSizeWhenSettled()
    }

    /// Ask the guest (virtio-gpu) to render at this view's shape, once the size stops changing,
    /// so a fold or posture animation does not trigger a burst of Windows mode changes.
    private func requestGuestSizeWhenSettled() {
        let factor = Self.guestPixelsPerPoint
        let target = CGSize(width: (bounds.width * factor / 2).rounded() * 2,
                            height: (bounds.height * factor / 2).rounded() * 2)
        guard target.width >= 640, target.height >= 480, target != requestedGuestSize else { return }
        pendingSizeRequest?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.requestedGuestSize = target
            WinIOSBridge.shared.requestDisplayWidth(UInt32(target.width), height: UInt32(target.height))
        }
        pendingSizeRequest = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35, execute: work)
    }

    // MARK: - Raw touches to the guest touchscreen

    private func setDigitizerActive(_ active: Bool) {
        guard active != digitizerActive else { return }
        digitizerActive = active
        for gr in gestureRecognizers ?? [] where !(gr is UIHoverGestureRecognizer) {
            gr.isEnabled = !active
        }
    }

    private func guestPoint(_ point: CGPoint) -> (x: UInt16, y: UInt16) {
        let (nx, ny) = normalizedPoint(for: point)
        return (UInt16(min(32767.0, max(0.0, nx * 32767.0)).rounded()),
                UInt16(min(32767.0, max(0.0, ny * 32767.0)).rounded()))
    }

    private func freeSlot() -> UInt8? {
        let used = Set(contacts.values.map(\.slot)).union(tapsInFlight.keys)
        return (0..<UInt8(Self.maxContacts)).first { !used.contains($0) }
    }

    private func addContact(_ touch: UITouch) {
        guard contacts[ObjectIdentifier(touch)] == nil, let slot = freeSlot() else { return }
        contacts[ObjectIdentifier(touch)] = Contact(touch: touch, slot: slot)
    }

    private func send(_ frame: [WinIOSTouchContact]) {
        guard !frame.isEmpty else { return }
        frame.withUnsafeBufferPointer { buf in
            WinIOSBridge.shared.sendTouchContacts(buf.baseAddress, count: buf.count)
        }
    }

    /// Sends every contact Windows has, with the tip up for the lifting ones, which go out once.
    private func sendTouchFrame(lifting: Set<ObjectIdentifier> = [], liftingTaps: Set<UInt8> = []) {
        var frame: [WinIOSTouchContact] = []
        for (id, contact) in contacts {
            let p = guestPoint(contact.touch.location(in: self))
            frame.append(WinIOSTouchContact(id: contact.slot, tip: lifting.contains(id) ? 0 : 1, x: p.x, y: p.y))
        }
        for (slot, p) in tapsInFlight {
            frame.append(WinIOSTouchContact(id: slot, tip: liftingTaps.contains(slot) ? 0 : 1, x: p.x, y: p.y))
        }
        send(frame)
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if contacts.isEmpty && held.isEmpty {
            setDigitizerActive(!trackpadMode && !rightClickArmed && WinIOSBridge.shared.touchscreenReady)
        }
        guard digitizerActive else { return super.touchesBegan(touches, with: event) }
        if !contacts.isEmpty {
            // Windows already has this gesture: new fingers join it at once.
            touches.forEach(addContact)
            sendTouchFrame()
            return
        }
        for touch in touches { heldTouches[ObjectIdentifier(touch)] = touch }
        let landed = touches.map { (id: ObjectIdentifier($0), at: $0.location(in: self)) }
        if let deadline = held.land(landed, time: CACurrentMediaTime()) {
            scheduleRelease(at: deadline)
        } else {
            releaseHeld()
        }
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard digitizerActive else { return super.touchesMoved(touches, with: event) }
        if !held.isEmpty {
            if held.movedTooFar({ self.heldTouches[$0]?.location(in: self) }) { releaseHeld() }
            return
        }
        sendTouchFrame()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard digitizerActive else { return super.touchesEnded(touches, with: event) }
        endTouches(touches, cancelled: false)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard digitizerActive else { return super.touchesCancelled(touches, with: event) }
        endTouches(touches, cancelled: true)
    }

    private func endTouches(_ touches: Set<UITouch>, cancelled: Bool) {
        let ids = Set(touches.map(ObjectIdentifier.init))
        if let outcome = held.lift(ids, cancelled: cancelled) {
            heldRelease?.cancel()
            heldRelease = nil
            heldTouches.removeAll()
            perform(outcome)
        }
        let lifting = ids.filter { contacts[$0] != nil }
        if !lifting.isEmpty {
            sendTouchFrame(lifting: lifting)
            for id in lifting { contacts[id] = nil }
        }
        settleIfIdle()
    }

    private func scheduleRelease(at deadline: TimeInterval) {
        heldRelease?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.releaseHeld() }
        heldRelease = work
        DispatchQueue.main.asyncAfter(deadline: .now() + max(0, deadline - CACurrentMediaTime()), execute: work)
    }

    /// The held fingers are a real touch gesture: hand the ones still down to Windows, landing
    /// where they first touched and then moving to where they are now.
    private func releaseHeld() {
        heldRelease?.cancel()
        heldRelease = nil
        let down = held.release()
        let touches = heldTouches
        heldTouches.removeAll()
        for finger in down {
            if let touch = touches[finger.id] { addContact(touch) }
        }
        var landing: [WinIOSTouchContact] = []
        for finger in down {
            guard let slot = contacts[finger.id]?.slot else { continue }
            let p = guestPoint(finger.start)
            landing.append(WinIOSTouchContact(id: slot, tip: 1, x: p.x, y: p.y))
        }
        for (slot, p) in tapsInFlight {
            landing.append(WinIOSTouchContact(id: slot, tip: 1, x: p.x, y: p.y))
        }
        send(landing)
        sendTouchFrame()
        settleIfIdle()
    }

    /// Every held finger lifted before the gesture reached Windows, so it was a quick tap.
    private func perform(_ outcome: HeldTaps.Outcome) {
        switch outcome {
        case .tap(let point):
            // Still a touch for Windows: press, then lift a moment later so it registers as a tap.
            guard let slot = freeSlot() else { return }
            tapsInFlight[slot] = guestPoint(point)
            sendTouchFrame()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.03) { [weak self] in
                guard let self, self.tapsInFlight[slot] != nil else { return }
                self.sendTouchFrame(liftingTaps: [slot])
                self.tapsInFlight[slot] = nil
                self.settleIfIdle()
            }
        case .rightClick(let point):
            UIImpactFeedbackGenerator(style: .medium).impactOccurred(intensity: 0.6)
            let (nx, ny) = normalizedPoint(for: point)
            PointerController.shared.directTapAt(x: nx, y: ny, buttonMask: 2)
        case .threeFingerTap:
            UIImpactFeedbackGenerator(style: .soft).impactOccurred()
            onThreeFingerTap?()
        case .nothing:
            break
        }
    }

    /// With no finger down, the gesture recognizers come back, so switching to trackpad or
    /// right-click mode works from the very next touch.
    private func settleIfIdle() {
        if contacts.isEmpty && held.isEmpty && tapsInFlight.isEmpty {
            setDigitizerActive(false)
        }
    }

    private func updateDrawableScale() {
        // Off-window there is no display to match; didMoveToWindow runs this again on attach.
        guard let window else { return }
        let scale = window.screen.scale
        contentScaleFactor = scale
        metalLayer.contentsScale = scale
        let w = max(1.0, bounds.width * scale)
        let h = max(1.0, bounds.height * scale)
        metalLayer.drawableSize = CGSize(width: w, height: h)
        if window.bounds.width > 0 {
            WinIOSBridge.shared.updateScreenSize(window.bounds.size, scale: scale)
        }
    }

    private func normalizedPoint(for pt: CGPoint) -> (Double, Double) {
        let bw = max(1.0, bounds.width)
        let bh = max(1.0, bounds.height)
        let fbW = Double(WinIOSBridge.shared.framebufferWidth)
        let fbH = Double(WinIOSBridge.shared.framebufferHeight)

        var rect = bounds
        if fbW > 0 && fbH > 0 {
            let viewAspect = Double(bw / bh)
            let fbAspect = fbW / fbH
            if viewAspect > fbAspect {
                let fittedW = CGFloat(Double(bh) * fbAspect)
                rect = CGRect(x: (bw - fittedW) * 0.5, y: 0, width: fittedW, height: bh)
            } else {
                let fittedH = CGFloat(Double(bw) / fbAspect)
                rect = CGRect(x: 0, y: (bh - fittedH) * 0.5, width: bw, height: fittedH)
            }
        }
        let nx = min(1.0, max(0.0, Double((pt.x - rect.minX) / max(1.0, rect.width))))
        let ny = min(1.0, max(0.0, Double((pt.y - rect.minY) / max(1.0, rect.height))))
        return (nx, ny)
    }

    @objc private func handleSingleTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        tapHaptic.impactOccurred(intensity: 0.45)
        let mask: UInt8
        if rightClickArmed {
            rightClickArmed = false
            onRightClickConsumed?()
            mask = 2
        } else {
            mask = 1
        }
        if trackpadMode {
            PointerController.shared.clickAtCurrent(buttonMask: mask)
        } else {
            let (nx, ny) = normalizedPoint(for: gr.location(in: self))
            PointerController.shared.directTapAt(x: nx, y: ny, buttonMask: mask)
        }
    }

    @objc private func handleTwoFingerTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        UIImpactFeedbackGenerator(style: .medium).impactOccurred(intensity: 0.6)
        PointerController.shared.clickAtCurrent(buttonMask: 2)
    }

    @objc private func handleThreeFingerTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        UIImpactFeedbackGenerator(style: .soft).impactOccurred()
        onThreeFingerTap?()
    }

    @objc private func handleLongPress(_ gr: UILongPressGestureRecognizer) {
        guard gr.state == .began else { return }
        UIImpactFeedbackGenerator(style: .medium).impactOccurred()
        if trackpadMode {
            PointerController.shared.clickAtCurrent(buttonMask: 2)
        } else {
            let (nx, ny) = normalizedPoint(for: gr.location(in: self))
            PointerController.shared.directTapAt(x: nx, y: ny, buttonMask: 2)
        }
    }

    @objc private func handleOneFingerPan(_ gr: UIPanGestureRecognizer) {
        let loc = gr.location(in: self)
        switch gr.state {
        case .began:
            panLastPoint = loc
            if !trackpadMode {
                let (nx, ny) = normalizedPoint(for: loc)
                activeDragButtons = rightClickArmed ? 2 : 1
                if rightClickArmed {
                    rightClickArmed = false
                    onRightClickConsumed?()
                }
                PointerController.shared.moveAbsolute(x: nx, y: ny, extraButtons: activeDragButtons)
            }
        case .changed:
            if trackpadMode {
                let dx = loc.x - panLastPoint.x
                let dy = loc.y - panLastPoint.y
                panLastPoint = loc
                PointerController.shared.moveRelative(
                    dxPoints: dx,
                    dyPoints: dy,
                    padWidth: bounds.width,
                    padHeight: bounds.height,
                    extraButtons: activeDragButtons
                )
            } else {
                let (nx, ny) = normalizedPoint(for: loc)
                PointerController.shared.moveAbsolute(x: nx, y: ny, extraButtons: activeDragButtons)
            }
        case .ended, .cancelled, .failed:
            if activeDragButtons != 0 {
                activeDragButtons = 0
                PointerController.shared.moveAbsolute(
                    x: PointerController.shared.cursorX,
                    y: PointerController.shared.cursorY,
                    extraButtons: 0
                )
            }
        default:
            break
        }
    }

    @objc private func handleTwoFingerScroll(_ gr: UIPanGestureRecognizer) {
        let trans = gr.translation(in: self)
        if gr.state == .began {
            scrollAccum = 0
        } else if gr.state == .changed {
            let delta = trans.y - scrollAccum
            if abs(delta) >= 12 {
                let steps = Int(delta / 12)
                scrollAccum += CGFloat(steps) * 12
                let clamped = Int8(clamping: max(-8, min(8, steps)))
                PointerController.shared.scroll(steps: clamped)
            }
        }
    }

    @objc private func handleHover(_ gr: UIHoverGestureRecognizer) {
        let (nx, ny) = normalizedPoint(for: gr.location(in: self))
        PointerController.shared.moveAbsolute(x: nx, y: ny, extraButtons: activeDragButtons)
    }

    // MARK: - Software & Hardware Keyboard (USB HID Page 0x07)

    override var canBecomeFirstResponder: Bool { true }
    var hasText: Bool { true }

    func insertText(_ text: String) {
        for ch in text {
            sendCharacterAsHID(ch)
        }
    }

    func deleteBackward() {
        WinIOSBridge.shared.tapKeyHid(0x2a) // Backspace
    }

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            if let key = press.key {
                let code = key.keyCode.rawValue
                if code >= 0x04 && code <= 0xe7 {
                    WinIOSBridge.shared.sendKeyHid(UInt8(code), down: true)
                    handled = true
                }
            }
        }
        if !handled { super.pressesBegan(presses, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            if let key = press.key {
                let code = key.keyCode.rawValue
                if code >= 0x04 && code <= 0xe7 {
                    WinIOSBridge.shared.sendKeyHid(UInt8(code), down: false)
                    handled = true
                }
            }
        }
        if !handled { super.pressesEnded(presses, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }

    private func sendCharacterAsHID(_ ch: Character) {
        guard let (usage, shift) = Self.asciiToHID(ch) else { return }
        if shift { WinIOSBridge.shared.sendKeyHid(0xe1, down: true) }
        WinIOSBridge.shared.sendKeyHid(usage, down: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.03) {
            WinIOSBridge.shared.sendKeyHid(usage, down: false)
            if shift { WinIOSBridge.shared.sendKeyHid(0xe1, down: false) }
        }
    }

    static func asciiToHID(_ ch: Character) -> (UInt8, Bool)? {
        if let ascii = ch.asciiValue {
            switch ascii {
            case 97...122: return (UInt8(0x04 + Int(ascii - 97)), false) // a-z
            case 65...90:  return (UInt8(0x04 + Int(ascii - 65)), true)  // A-Z
            case 49...57:  return (UInt8(0x1e + Int(ascii - 49)), false) // 1-9
            case 48:       return (0x27, false) // 0
            case 10, 13:   return (0x28, false) // Enter
            case 9:        return (0x2b, false) // Tab
            case 32:       return (0x2c, false) // Space
            default: break
            }
        }
        let symbols: [Character: (UInt8, Bool)] = [
            "!": (0x1e, true), "@": (0x1f, true), "#": (0x20, true), "$": (0x21, true),
            "%": (0x22, true), "^": (0x23, true), "&": (0x24, true), "*": (0x25, true),
            "(": (0x26, true), ")": (0x27, true), "-": (0x2d, false), "_": (0x2d, true),
            "=": (0x2e, false), "+": (0x2e, true), "[": (0x2f, false), "{": (0x2f, true),
            "]": (0x30, false), "}": (0x30, true), "\\": (0x31, false), "|": (0x31, true),
            ";": (0x33, false), ":": (0x33, true), "'": (0x34, false), "\"": (0x34, true),
            "`": (0x35, false), "~": (0x35, true), ",": (0x36, false), "<": (0x36, true),
            ".": (0x37, false), ">": (0x37, true), "/": (0x38, false), "?": (0x38, true),
        ]
        return symbols[ch]
    }
}

struct MetalDisplayView: UIViewRepresentable {
    @Binding var trackpadMode: Bool
    @Binding var rightClickArmed: Bool
    @Binding var keyboardVisible: Bool
    var onThreeFingerTap: () -> Void

    func makeUIView(context: Context) -> MetalCanvasUIView {
        let view = MetalCanvasUIView(frame: .zero)
        view.onThreeFingerTap = onThreeFingerTap
        view.onRightClickConsumed = {
            DispatchQueue.main.async { self.rightClickArmed = false }
        }
        return view
    }

    func updateUIView(_ uiView: MetalCanvasUIView, context: Context) {
        uiView.trackpadMode = trackpadMode
        uiView.rightClickArmed = rightClickArmed
        uiView.onThreeFingerTap = onThreeFingerTap
        if keyboardVisible && !uiView.isFirstResponder {
            DispatchQueue.main.async { _ = uiView.becomeFirstResponder() }
        } else if !keyboardVisible && uiView.isFirstResponder {
            DispatchQueue.main.async { _ = uiView.resignFirstResponder() }
        }
    }
}
