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
    /// A tap that reached Windows, in guest pixels: the software keyboard follows text focus from it.
    var onGuestTap: ((Int, Int) -> Void)?
    var onReturnKey: (() -> Void)?
    /// The software keyboard went away by another road than `keyboardVisible`: its own dismiss key,
    /// or a sheet taking the screen.
    var onKeyboardResigned: (() -> Void)?
    /// Where the Windows picture sits in this view. The view itself runs to the display's edges
    /// and continues the picture's edge pixels around it; touches out there land on the nearest
    /// pixel of the picture.
    var pictureFrame: CGRect? {
        didSet { if pictureFrame != oldValue { sendPictureRect() } }
    }

    private var activeDragButtons: UInt8 = 0
    private var panLastPoint: CGPoint = .zero
    private var scrollAccum: CGFloat = 0
    private let tapHaptic = UIImpactFeedbackGenerator(style: .light)

    /// Touch mode drives the guest's HID touchscreen with raw touches once Windows has configured
    /// it; trackpad mode and the time before that keep the gesture-driven absolute pointer.
    private var digitizerActive = false
    /// Touches Windows knows about, with the HID contact id each was given. Windows' own touch
    /// gestures apply, so press and hold is the right click.
    private var contacts: [ObjectIdentifier: Contact] = [:]
    private static let maxContacts = 5

    private struct Contact {
        let touch: UITouch
        let slot: UInt8
    }

    /// The first finger of a touch sequence, while it can still be a tap: one finger, lifted soon,
    /// close to where it landed.
    private var tapStart: (point: CGPoint, time: TimeInterval)?
    private static let tapSlop: CGFloat = 12
    private static let tapDuration: TimeInterval = 0.6

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
            sendPictureRect()
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        updateDrawableScale()
        sendPictureRect()
    }

    private func sendPictureRect() {
        guard bounds.width > 0, bounds.height > 0 else { return }
        let picture = pictureFrame ?? bounds
        let rect = CGRect(x: picture.minX / bounds.width, y: picture.minY / bounds.height,
                          width: picture.width / bounds.width, height: picture.height / bounds.height)
        WinIOSBridge.shared.setPictureRect(rect, for: metalLayer)
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

    private func addContact(_ touch: UITouch) {
        guard contacts[ObjectIdentifier(touch)] == nil else { return }
        let used = Set(contacts.values.map(\.slot))
        guard let slot = (0..<UInt8(Self.maxContacts)).first(where: { !used.contains($0) }) else { return }
        contacts[ObjectIdentifier(touch)] = Contact(touch: touch, slot: slot)
    }

    /// Sends every contact Windows has, with the tip up for the lifting ones, which go out once.
    private func sendTouchFrame(lifting: Set<ObjectIdentifier> = []) {
        var frame: [WinIOSTouchContact] = []
        for (id, contact) in contacts {
            let p = guestPoint(contact.touch.location(in: self))
            frame.append(WinIOSTouchContact(id: contact.slot, tip: lifting.contains(id) ? 0 : 1, x: p.x, y: p.y))
        }
        guard !frame.isEmpty else { return }
        frame.withUnsafeBufferPointer { buf in
            WinIOSBridge.shared.sendTouchContacts(buf.baseAddress, count: buf.count)
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if contacts.isEmpty {
            setDigitizerActive(!trackpadMode && !rightClickArmed && WinIOSBridge.shared.touchscreenReady)
        }
        guard digitizerActive else { return super.touchesBegan(touches, with: event) }
        if contacts.isEmpty, touches.count == 1, let first = touches.first {
            tapStart = (first.location(in: self), first.timestamp)
        } else {
            tapStart = nil
        }
        touches.forEach(addContact)
        sendTouchFrame()
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard digitizerActive else { return super.touchesMoved(touches, with: event) }
        if let start = tapStart, let touch = touches.first {
            let at = touch.location(in: self)
            if hypot(at.x - start.point.x, at.y - start.point.y) > Self.tapSlop { tapStart = nil }
        }
        sendTouchFrame()
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard digitizerActive else { return super.touchesEnded(touches, with: event) }
        let tap = tapStart
        tapStart = nil
        endTouches(touches)
        if let tap, let touch = touches.first, touch.timestamp - tap.time <= Self.tapDuration {
            let at = touch.location(in: self)
            if hypot(at.x - tap.point.x, at.y - tap.point.y) <= Self.tapSlop { reportGuestTap(normalizedPoint(for: at)) }
        }
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard digitizerActive else { return super.touchesCancelled(touches, with: event) }
        tapStart = nil
        endTouches(touches)
    }

    private func reportGuestTap(_ normalized: (Double, Double)) {
        let width = Double(WinIOSBridge.shared.framebufferWidth), height = Double(WinIOSBridge.shared.framebufferHeight)
        guard width > 0, height > 0 else { return }
        onGuestTap?(Int(min(width - 1, normalized.0 * width)), Int(min(height - 1, normalized.1 * height)))
    }

    private func endTouches(_ touches: Set<UITouch>) {
        let lifting = Set(touches.map(ObjectIdentifier.init)).filter { contacts[$0] != nil }
        guard !lifting.isEmpty else { return }
        sendTouchFrame(lifting: lifting)
        for id in lifting { contacts[id] = nil }
        // With no finger down the gesture recognizers come back, so switching to trackpad or
        // right-click mode works from the very next touch.
        if contacts.isEmpty { setDigitizerActive(false) }
    }

    private func updateDrawableScale() {
        // Off-window there is no display to match; didMoveToWindow runs this again on attach.
        guard let window else { return }
        let scale = window.screen.scale
        contentScaleFactor = scale
        metalLayer.contentsScale = scale
        // The window, or the lid, is a whole number of device pixels, so this is exact.
        let w = max(1.0, (bounds.width * scale).rounded())
        let h = max(1.0, (bounds.height * scale).rounded())
        metalLayer.drawableSize = CGSize(width: w, height: h)
        if window.bounds.width > 0 {
            WinIOSBridge.shared.updateScreenSize(window.bounds.size, scale: scale)
        }
    }

    private func normalizedPoint(for pt: CGPoint) -> (Double, Double) {
        // The same fit the renderer makes: the guest's frame inside the picture's place.
        let picture = pictureFrame ?? bounds
        let bw = max(1.0, picture.width)
        let bh = max(1.0, picture.height)
        let fbW = Double(WinIOSBridge.shared.framebufferWidth)
        let fbH = Double(WinIOSBridge.shared.framebufferHeight)

        var rect = picture
        if fbW > 0 && fbH > 0 {
            let viewAspect = Double(bw / bh)
            let fbAspect = fbW / fbH
            if viewAspect > fbAspect {
                let fittedW = CGFloat(Double(bh) * fbAspect)
                rect = CGRect(x: picture.minX + (bw - fittedW) * 0.5, y: picture.minY, width: fittedW, height: bh)
            } else {
                let fittedH = CGFloat(Double(bw) / fbAspect)
                rect = CGRect(x: picture.minX, y: picture.minY + (bh - fittedH) * 0.5, width: bw, height: fittedH)
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
            if mask == 1 { reportGuestTap((PointerController.shared.cursorX, PointerController.shared.cursorY)) }
        } else {
            let (nx, ny) = normalizedPoint(for: gr.location(in: self))
            PointerController.shared.directTapAt(x: nx, y: ny, buttonMask: mask)
            if mask == 1 { reportGuestTap((nx, ny)) }
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

    override func resignFirstResponder() -> Bool {
        let resigned = super.resignFirstResponder()
        if resigned { onKeyboardResigned?() }
        return resigned
    }
    var hasText: Bool { true }

    func insertText(_ text: String) {
        for ch in text {
            sendCharacterAsHID(ch)
        }
        if text.contains(where: \.isNewline) { onReturnKey?() }
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
    /// The picture's place in the view's own coordinates.
    var pictureFrame: CGRect
    var onThreeFingerTap: () -> Void
    var onGuestTap: ((Int, Int) -> Void)?
    var onReturnKey: (() -> Void)?

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
        uiView.onGuestTap = onGuestTap
        uiView.onReturnKey = onReturnKey
        uiView.onKeyboardResigned = { [weak uiView] in
            DispatchQueue.main.async {
                if self.keyboardVisible, uiView?.isFirstResponder != true { self.keyboardVisible = false }
            }
        }
        uiView.pictureFrame = pictureFrame
        // Each hop looks again: the state may have changed, or the view left the window, before it runs.
        if keyboardVisible && !uiView.isFirstResponder {
            DispatchQueue.main.async {
                guard self.keyboardVisible, uiView.window != nil, !uiView.isFirstResponder else { return }
                // Refused (a sheet has the screen): the state says so, rather than trying again on every update.
                if !uiView.becomeFirstResponder() { self.keyboardVisible = false }
            }
        } else if !keyboardVisible && uiView.isFirstResponder {
            DispatchQueue.main.async {
                guard !self.keyboardVisible, uiView.isFirstResponder else { return }
                _ = uiView.resignFirstResponder()
            }
        }
    }
}
