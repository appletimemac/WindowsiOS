// SPDX-License-Identifier: GPL-2.0-or-later
import QuartzCore
import SwiftUI
import UIKit

// MARK: - Authentic 4-Pane Microsoft Surface / Windows Emblem

struct WindowsLogoMark: View {
    var size: CGFloat = 11
    var color: Color = .white

    var body: some View {
        let gap = max(1.1, size * 0.12)
        let tile = (size - gap) * 0.5
        VStack(spacing: gap) {
            HStack(spacing: gap) {
                Rectangle().fill(color).frame(width: tile, height: tile)
                Rectangle().fill(color).frame(width: tile, height: tile)
            }
            HStack(spacing: gap) {
                Rectangle().fill(color).frame(width: tile, height: tile)
                Rectangle().fill(color).frame(width: tile, height: tile)
            }
        }
        .frame(width: size, height: size)
    }
}

// MARK: - Precision Haptic Matte-Glass Touchpad (UIKit 120Hz + Invisible Bottom-Corner Click Zones)

final class TrackpadGlassUIView: UIView {
    var onActivityChanged: ((Bool) -> Void)?
    var onCornerClickState: ((Int) -> Void)? // 0 = none, 1 = left held, 2 = right held

    private var panLast: CGPoint = .zero
    private var scrollAccum: CGFloat = 0
    private var lastTapEndedTime: CFTimeInterval = 0
    private var tapDragActive: Bool = false
    private weak var heldCornerTouch: UITouch?
    private var heldCornerButton: UInt8 = 0

    private let clickHaptic = UIImpactFeedbackGenerator(style: .rigid)
    private let releaseHaptic = UIImpactFeedbackGenerator(style: .soft)
    private let rightHaptic = UIImpactFeedbackGenerator(style: .medium)

    override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        backgroundColor = .clear
        isMultipleTouchEnabled = true
        isUserInteractionEnabled = true

        let singleTap = UITapGestureRecognizer(target: self, action: #selector(onSingleTap(_:)))
        singleTap.numberOfTouchesRequired = 1
        singleTap.cancelsTouchesInView = false
        addGestureRecognizer(singleTap)

        let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(onTwoFingerTap(_:)))
        twoFingerTap.numberOfTouchesRequired = 2
        twoFingerTap.cancelsTouchesInView = false
        addGestureRecognizer(twoFingerTap)

        let movePan = UIPanGestureRecognizer(target: self, action: #selector(onOneFingerPan(_:)))
        movePan.minimumNumberOfTouches = 1
        movePan.maximumNumberOfTouches = 1
        movePan.cancelsTouchesInView = false
        addGestureRecognizer(movePan)

        let scrollPan = UIPanGestureRecognizer(target: self, action: #selector(onTwoFingerPan(_:)))
        scrollPan.minimumNumberOfTouches = 2
        scrollPan.maximumNumberOfTouches = 2
        scrollPan.cancelsTouchesInView = false
        if #available(iOS 13.4, *) {
            scrollPan.allowedScrollTypesMask = .all
        }
        addGestureRecognizer(scrollPan)
    }

    // Invisible Surface Haptic Touchpad bottom-left (hold-drag) & bottom-right (right-click) zones
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        guard heldCornerTouch == nil, bounds.height > 20, bounds.width > 40 else { return }
        for t in touches {
            let p = t.location(in: self)
            if p.y >= bounds.height * 0.74 {
                if p.x <= bounds.width * 0.28 {
                    heldCornerTouch = t
                    heldCornerButton = 1
                    clickHaptic.impactOccurred(intensity: 0.72)
                    PointerController.shared.setHeldButton(1, down: true)
                    onCornerClickState?(1)
                    break
                } else if p.x >= bounds.width * 0.72 {
                    heldCornerTouch = t
                    heldCornerButton = 2
                    rightHaptic.impactOccurred(intensity: 0.78)
                    PointerController.shared.setHeldButton(2, down: true)
                    onCornerClickState?(2)
                    break
                }
            }
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        releaseCornerIfNeeded(touches)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesCancelled(touches, with: event)
        releaseCornerIfNeeded(touches)
    }

    private func releaseCornerIfNeeded(_ touches: Set<UITouch>) {
        if let held = heldCornerTouch, touches.contains(held) {
            let btn = heldCornerButton
            heldCornerTouch = nil
            heldCornerButton = 0
            releaseHaptic.impactOccurred(intensity: 0.35)
            PointerController.shared.setHeldButton(btn, down: false)
            onCornerClickState?(0)
        }
    }

    @objc private func onSingleTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        let p = gr.location(in: self)
        // Bottom-right haptic corner tap acts as crisp Right-Click!
        if p.y >= bounds.height * 0.74 && p.x >= bounds.width * 0.72 {
            rightHaptic.impactOccurred(intensity: 0.75)
            PointerController.shared.clickAtCurrent(buttonMask: 2)
            return
        }
        lastTapEndedTime = CACurrentMediaTime()
        clickHaptic.impactOccurred(intensity: 0.55)
        PointerController.shared.clickAtCurrent(buttonMask: 1)
    }

    @objc private func onTwoFingerTap(_ gr: UITapGestureRecognizer) {
        guard gr.state == .ended else { return }
        rightHaptic.impactOccurred(intensity: 0.75)
        PointerController.shared.clickAtCurrent(buttonMask: 2)
    }

    @objc private func onOneFingerPan(_ gr: UIPanGestureRecognizer) {
        let loc = gr.location(in: self)
        switch gr.state {
        case .began:
            panLast = loc
            onActivityChanged?(true)
            if CACurrentMediaTime() - lastTapEndedTime < 0.28 {
                tapDragActive = true
                clickHaptic.impactOccurred(intensity: 0.45)
                PointerController.shared.setHeldButton(1, down: true)
            }
        case .changed:
            let dx = loc.x - panLast.x
            let dy = loc.y - panLast.y
            panLast = loc
            PointerController.shared.moveRelative(
                dxPoints: dx,
                dyPoints: dy,
                padWidth: bounds.width,
                padHeight: bounds.height
            )
        case .ended, .cancelled, .failed:
            if tapDragActive {
                tapDragActive = false
                PointerController.shared.setHeldButton(1, down: false)
            }
            onActivityChanged?(false)
        default:
            break
        }
    }

    @objc private func onTwoFingerPan(_ gr: UIPanGestureRecognizer) {
        let trans = gr.translation(in: self)
        switch gr.state {
        case .began:
            scrollAccum = 0
            onActivityChanged?(true)
        case .changed:
            let delta = trans.y - scrollAccum
            if abs(delta) >= 10 {
                let steps = Int(delta / 10)
                scrollAccum += CGFloat(steps) * 10
                let clamped = Int8(clamping: max(-8, min(8, steps)))
                PointerController.shared.scroll(steps: clamped)
            }
        case .ended, .cancelled, .failed:
            onActivityChanged?(false)
        default:
            break
        }
    }
}

struct TrackpadGlassRepresentable: UIViewRepresentable {
    @Binding var isTouching: Bool
    @Binding var cornerClickState: Int

    func makeUIView(context: Context) -> TrackpadGlassUIView {
        let v = TrackpadGlassUIView(frame: .zero)
        v.onActivityChanged = { active in
            DispatchQueue.main.async { self.isTouching = active }
        }
        v.onCornerClickState = { state in
            DispatchQueue.main.async { self.cornerClickState = state }
        }
        return v
    }

    func updateUIView(_ uiView: TrackpadGlassUIView, context: Context) {}
}

// MARK: - Surface Laptop 6-Row Keyboard Model

private enum FunctionAction: Equatable {
    case escape
    case startMenu
    case search
    case explorer
    case edgeBrowser
    case antigravityApp
    case taskManager
    case altTab
    case showDesktop
    case rightClickArm
    case dragLockToggle
    case cycleDeckLayout
    case rotateScreen
    case openStudioMode
    case acpiPower
    case forwardDelete
}

private enum KeyRole: Equatable {
    case functionKey(action: FunctionAction, fnHid: UInt8?, fnTag: String?, symbol: String?, text: String?)
    case regular(hid: UInt8, primary: String, shifted: String?, fnHid: UInt8?, fnLabel: String?, homing: Bool)
    case modifier(hid: UInt8, label: String, isWindowsKey: Bool)
    case capsLock
    case fnToggle
    case upDownStack // Authentic Surface half-height stacked Up / Down arrow pair!
}

private struct LaptopKeySpec: Identifiable {
    let id: String
    let widthWeight: CGFloat
    let role: KeyRole

    static func fnKey(
        _ id: String,
        action: FunctionAction,
        fnHid: UInt8? = nil,
        fnTag: String? = nil,
        symbol: String? = nil,
        text: String? = nil,
        w: CGFloat = 1.0
    ) -> LaptopKeySpec {
        LaptopKeySpec(id: id, widthWeight: w, role: .functionKey(action: action, fnHid: fnHid, fnTag: fnTag, symbol: symbol, text: text))
    }

    static func key(
        _ primary: String,
        shifted: String? = nil,
        hid: UInt8,
        w: CGFloat = 1.0,
        fnHid: UInt8? = nil,
        fnLabel: String? = nil,
        homing: Bool = false
    ) -> LaptopKeySpec {
        LaptopKeySpec(
            id: "\(primary)_\(hid)",
            widthWeight: w,
            role: .regular(hid: hid, primary: primary, shifted: shifted, fnHid: fnHid, fnLabel: fnLabel, homing: homing)
        )
    }

    static func mod(_ id: String, label: String, isWindowsKey: Bool = false, hid: UInt8, w: CGFloat) -> LaptopKeySpec {
        LaptopKeySpec(id: id, widthWeight: w, role: .modifier(hid: hid, label: label, isWindowsKey: isWindowsKey))
    }
}

// MARK: - Midnight-Sapphire Surface Laptop Ultra Lower Deck

/// The iOS dark keyboard, in system colours: letter keys on a lighter grey than the system keys,
/// a latched modifier lit white like Shift on iOS.
private enum KeyboardStyle {
    static let deck = Color(uiColor: .systemGray6)
    static let key = Color(uiColor: .systemGray2)
    static let systemKey = Color(uiColor: .systemGray4)
    static let pressedKey = Color(uiColor: .systemGray)
    static let latchedKey = Color.white
    static let keyShadow = Color.black.opacity(0.35)
    static let trackpad = Color(uiColor: .systemGray5)
}

struct LaptopDeckView: View {
    @ObservedObject var vm: VMViewModel
    /// Where the keys and trackpad stay clear of the display's rounded corners and cameras; the
    /// deck surface itself runs to the edges.
    var insets = PictureInsets()

    @State private var shiftLatched: Bool = false
    @State private var ctrlLatched: Bool = false
    @State private var altLatched: Bool = false
    @State private var winLatched: Bool = false
    @State private var capsLocked: Bool = false
    @State private var fnActive: Bool = false

    @State private var dragLocked: Bool = false
    @State private var padTouching: Bool = false
    @State private var cornerClickState: Int = 0

    private let keyDownHaptic = UIImpactFeedbackGenerator(style: .rigid)
    private let keyUpHaptic = UIImpactFeedbackGenerator(style: .soft)

    // Row 0: Half-height Surface Laptop Function Row (replaces floating toolbars cleanly!)
    private static let functionRow: [LaptopKeySpec] = [
        .fnKey("esc",   action: .escape,          text: "Esc", w: 1.08),
        .fnKey("f1",    action: .startMenu,       fnHid: 0x3a, fnTag: "F1"),
        .fnKey("f2",    action: .search,          fnHid: 0x3b, fnTag: "F2", symbol: "magnifyingglass"),
        .fnKey("f3",    action: .explorer,        fnHid: 0x3c, fnTag: "F3", symbol: "folder"),
        .fnKey("f4",    action: .edgeBrowser,     fnHid: 0x3d, fnTag: "F4", symbol: "globe"),
        .fnKey("f5",    action: .antigravityApp,  fnHid: 0x3e, fnTag: "F5", symbol: "sparkles"),
        .fnKey("f6",    action: .altTab,          fnHid: 0x3f, fnTag: "F6", symbol: "rectangle.on.rectangle"),
        .fnKey("f7",    action: .showDesktop,     fnHid: 0x40, fnTag: "F7", symbol: "menubar.rectangle"),
        .fnKey("f8",    action: .rightClickArm,   fnHid: 0x41, fnTag: "F8", symbol: "computermouse"),
        .fnKey("f9",    action: .dragLockToggle,  fnHid: 0x42, fnTag: "F9", symbol: "hand.draw"),
        .fnKey("f10",   action: .cycleDeckLayout, fnHid: 0x43, fnTag: "F10", symbol: "rectangle.split.2x1"),
        .fnKey("f11",   action: .rotateScreen,    fnHid: 0x44, fnTag: "F11", symbol: "rotate.right"),
        .fnKey("f12",   action: .openStudioMode,  fnHid: 0x45, fnTag: "F12", symbol: "arrow.up.left.and.arrow.down.right"),
        .fnKey("del",   action: .forwardDelete,   text: "Del", w: 1.15),
    ]

    // Rows 1–5: Authentic Surface Laptop ANSI Layout with Stacked Up/Down Inverted-T Arrows
    private static let mainRows: [[LaptopKeySpec]] = [
        // Row 1: Number row
        [
            .key("`", shifted: "~", hid: 0x35),
            .key("1", shifted: "!", hid: 0x1e),
            .key("2", shifted: "@", hid: 0x1f),
            .key("3", shifted: "#", hid: 0x20),
            .key("4", shifted: "$", hid: 0x21),
            .key("5", shifted: "%", hid: 0x22),
            .key("6", shifted: "^", hid: 0x23),
            .key("7", shifted: "&", hid: 0x24),
            .key("8", shifted: "*", hid: 0x25),
            .key("9", shifted: "(", hid: 0x26),
            .key("0", shifted: ")", hid: 0x27),
            .key("-", shifted: "_", hid: 0x2d),
            .key("=", shifted: "+", hid: 0x2e),
            .key("Backspace", hid: 0x2a, w: 1.65),
        ],
        // Row 2: QWERTY
        [
            .key("Tab", hid: 0x2b, w: 1.48),
            .key("q", shifted: "Q", hid: 0x14),
            .key("w", shifted: "W", hid: 0x1a),
            .key("e", shifted: "E", hid: 0x08),
            .key("r", shifted: "R", hid: 0x15),
            .key("t", shifted: "T", hid: 0x17),
            .key("y", shifted: "Y", hid: 0x1c),
            .key("u", shifted: "U", hid: 0x18),
            .key("i", shifted: "I", hid: 0x0c),
            .key("o", shifted: "O", hid: 0x12),
            .key("p", shifted: "P", hid: 0x13),
            .key("[", shifted: "{", hid: 0x2f),
            .key("]", shifted: "}", hid: 0x30),
            .key("\\", shifted: "|", hid: 0x31, w: 1.17),
        ],
        // Row 3: Home row
        [
            LaptopKeySpec(id: "caps", widthWeight: 1.75, role: .capsLock),
            .key("a", shifted: "A", hid: 0x04),
            .key("s", shifted: "S", hid: 0x16),
            .key("d", shifted: "D", hid: 0x07),
            .key("f", shifted: "F", hid: 0x09, homing: true),
            .key("g", shifted: "G", hid: 0x0a),
            .key("h", shifted: "H", hid: 0x0b),
            .key("j", shifted: "J", hid: 0x0d, homing: true),
            .key("k", shifted: "K", hid: 0x0e),
            .key("l", shifted: "L", hid: 0x0f),
            .key(";", shifted: ":", hid: 0x33),
            .key("'", shifted: "\"", hid: 0x34),
            .key("Enter", hid: 0x28, w: 1.90),
        ],
        // Row 4: Full dual-Shift row (clean Surface ANSI symmetry)
        [
            .mod("lshift", label: "Shift", hid: 0xe1, w: 2.22),
            .key("z", shifted: "Z", hid: 0x1d),
            .key("x", shifted: "X", hid: 0x1b),
            .key("c", shifted: "C", hid: 0x06),
            .key("v", shifted: "V", hid: 0x19),
            .key("b", shifted: "B", hid: 0x05),
            .key("n", shifted: "N", hid: 0x11),
            .key("m", shifted: "M", hid: 0x10),
            .key(",", shifted: "<", hid: 0x36),
            .key(".", shifted: ">", hid: 0x37),
            .key("/", shifted: "?", hid: 0x38),
            .mod("rshift", label: "Shift", hid: 0xe5, w: 2.42),
        ],
        // Row 5: Bottom modifier row + wide Spacebar + Surface Inverted-T Stacked Arrows
        [
            .mod("lctrl", label: "Ctrl", hid: 0xe0, w: 1.25),
            LaptopKeySpec(id: "fn", widthWeight: 1.10, role: .fnToggle),
            .mod("lwin", label: "Win", isWindowsKey: true, hid: 0xe3, w: 1.20),
            .mod("lalt", label: "Alt", hid: 0xe2, w: 1.20),
            .key("Space", hid: 0x2c, w: 5.35),
            .mod("ralt", label: "Alt", hid: 0xe6, w: 1.15),
            .key("←", hid: 0x50, w: 1.05, fnHid: 0x4a, fnLabel: "Home"),
            LaptopKeySpec(id: "updown", widthWeight: 1.05, role: .upDownStack),
            .key("→", hid: 0x4f, w: 1.05, fnHid: 0x4d, fnLabel: "End"),
        ],
    ]

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            let topLip: CGFloat = max(9.0, insets.top)
            let bottomLip: CGFloat = max(6.0, insets.bottom)
            let contentW = max(0, w - insets.left - insets.right)

            ZStack(alignment: .bottom) {
                KeyboardStyle.deck

                VStack(spacing: 0) {
                    Color.clear
                        .frame(height: topLip)

                    let bodyH = max(140.0, h - topLip - bottomLip)

                    switch vm.deckLayout {
                    case .split:
                        let kbdH = max(142.0, bodyH * 0.625)
                        let padH = max(64.0, bodyH - kbdH - 12.0)
                        VStack(spacing: 8) {
                            recessedSurfaceKeyboardWell(height: kbdH)
                                .frame(height: kbdH)
                                .padding(.horizontal, 10)

                            flushSurfaceHapticTouchpad(width: contentW, height: padH, expanded: false)
                                .frame(height: padH)
                        }
                        .padding(.top, 4)

                    case .trackpad:
                        ZStack(alignment: .topTrailing) {
                            flushSurfaceHapticTouchpad(width: contentW, height: bodyH - 12.0, expanded: true)
                                .frame(height: bodyH - 12.0)
                                .padding(.top, 6)

                            // The only on-deck way back from the full trackpad (F10 is hidden here).
                            Button("Keyboard", systemImage: "keyboard") {
                                keyDownHaptic.impactOccurred(intensity: 0.5)
                                withAnimation(.snappy(duration: 0.2)) {
                                    vm.deckLayout = .split
                                }
                            }
                            .buttonStyle(.glass)
                            .padding(.top, 14)
                            .padding(.trailing, 18)
                        }

                    case .keyboard:
                        recessedSurfaceKeyboardWell(height: bodyH - 10.0)
                            .frame(height: bodyH - 10.0)
                            .padding(.horizontal, 10)
                            .padding(.top, 5)
                    }

                    Spacer(minLength: 0)
                }
                .padding(.leading, insets.left)
                .padding(.trailing, insets.right)
            }
            .frame(width: w, height: h)
        }
        .onDisappear {
            // A fold or posture switch tears the deck down; never leave the guest's button held.
            if dragLocked {
                dragLocked = false
                PointerController.shared.setHeldButton(1, down: false)
            }
        }
    }

    // MARK: - Keyboard

    private func recessedSurfaceKeyboardWell(height: CGFloat) -> some View {
        let spacingV: CGFloat = 3.2
        let padV: CGFloat = 5.0
        let availRowsH = max(110.0, height - padV * 2 - spacingV * 5)
        // Row 0 is 0.68x height (authentic low-profile Surface function key strip); Rows 1..5 are 1.0x
        let unitH = availRowsH / 5.68
        let fnRowH = max(16.5, unitH * 0.68)
        let stdRowH = max(21.0, unitH)

        return VStack(spacing: spacingV) {
            // Row 0: Surface Function Row
            keyboardRowView(row: Self.functionRow, rowHeight: fnRowH, isFunctionStrip: true)
                .frame(height: fnRowH)

            // Rows 1..5: Main Chiclet Matrix
            ForEach(0..<Self.mainRows.count, id: \.self) { idx in
                keyboardRowView(row: Self.mainRows[idx], rowHeight: stdRowH, isFunctionStrip: false)
                    .frame(height: stdRowH)
            }
        }
        .padding(.horizontal, 5.5)
        .padding(.vertical, padV)
    }

    private func keyboardRowView(row: [LaptopKeySpec], rowHeight: CGFloat, isFunctionStrip: Bool) -> some View {
        GeometryReader { rowGeo in
            let totalWeight = row.reduce(CGFloat(0)) { $0 + $1.widthWeight }
            let spacingH: CGFloat = 3.2
            let availW = max(10.0, rowGeo.size.width - spacingH * CGFloat(max(0, row.count - 1)))

            HStack(spacing: spacingH) {
                ForEach(row) { spec in
                    let kw = availW * (spec.widthWeight / totalWeight)
                    if spec.role == .upDownStack {
                        SurfaceStackedArrowView(
                            isFnActive: fnActive,
                            onTapArrow: { up in
                                keyDownHaptic.impactOccurred(intensity: 0.68)
                                let code: UInt8 = up ? (fnActive ? 0x4b : 0x52) : (fnActive ? 0x4e : 0x51)
                                sendModifiedTap(code)
                            }
                        )
                        .frame(width: kw, height: rowHeight)
                    } else {
                        SurfaceKeycapView(
                            spec: spec,
                            isFunctionStrip: isFunctionStrip,
                            isShiftActive: shiftLatched || capsLocked,
                            isLatchedOrLED: isKeyLEDLit(spec),
                            isFnActive: fnActive,
                            onKeyDown: { handleKeyDown(spec, viewportWidth: rowGeo.size.width) },
                            onKeyUp: { handleKeyUp(spec) }
                        )
                        .frame(width: kw, height: rowHeight)
                    }
                }
            }
        }
    }

    private func isKeyLEDLit(_ spec: LaptopKeySpec) -> Bool {
        switch spec.role {
        case .capsLock:
            return capsLocked
        case .fnToggle:
            return fnActive
        case .modifier(let hid, _, _):
            switch hid {
            case 0xe1, 0xe5: return shiftLatched
            case 0xe0: return ctrlLatched
            case 0xe2, 0xe6: return altLatched
            case 0xe3: return winLatched
            default: return false
            }
        case .functionKey(let action, _, _, _, _):
            switch action {
            case .rightClickArm: return vm.rightClickArmed
            case .dragLockToggle: return dragLocked
            case .cycleDeckLayout: return vm.deckLayout != .split
            default: return false
            }
        case .regular, .upDownStack:
            return false
        }
    }

    private func sendModifiedTap(_ hid: UInt8) {
        if ctrlLatched { WinIOSBridge.shared.sendKeyHid(0xe0, down: true) }
        if altLatched  { WinIOSBridge.shared.sendKeyHid(0xe2, down: true) }
        if winLatched  { WinIOSBridge.shared.sendKeyHid(0xe3, down: true) }
        if shiftLatched { WinIOSBridge.shared.sendKeyHid(0xe1, down: true) }
        WinIOSBridge.shared.tapKeyHid(hid)
        releaseLatchedModifiers()
    }

    private func releaseLatchedModifiers() {
        if shiftLatched { WinIOSBridge.shared.sendKeyHid(0xe1, down: false); shiftLatched = false }
        if ctrlLatched  { WinIOSBridge.shared.sendKeyHid(0xe0, down: false); ctrlLatched = false }
        if altLatched   { WinIOSBridge.shared.sendKeyHid(0xe2, down: false); altLatched = false }
        if winLatched   { WinIOSBridge.shared.sendKeyHid(0xe3, down: false); winLatched = false }
    }

    private func handleKeyDown(_ spec: LaptopKeySpec, viewportWidth: CGFloat) {
        keyDownHaptic.impactOccurred(intensity: 0.7)
        switch spec.role {
        case .capsLock:
            capsLocked.toggle()
            WinIOSBridge.shared.tapKeyHid(0x39)

        case .fnToggle:
            fnActive.toggle()

        case .modifier(let hid, _, _):
            toggleModifierLatch(hid: hid)

        case .functionKey(let action, let fnHid, _, _, _):
            if fnActive, let fnHid {
                sendModifiedTap(fnHid)
                return
            }
            performFunctionAction(action)

        case .regular(let hid, _, _, let fnHid, _, _):
            let actualHid = (fnActive && fnHid != nil) ? fnHid! : hid
            if ctrlLatched { WinIOSBridge.shared.sendKeyHid(0xe0, down: true) }
            if altLatched  { WinIOSBridge.shared.sendKeyHid(0xe2, down: true) }
            if winLatched  { WinIOSBridge.shared.sendKeyHid(0xe3, down: true) }
            if shiftLatched { WinIOSBridge.shared.sendKeyHid(0xe1, down: true) }
            WinIOSBridge.shared.sendKeyHid(actualHid, down: true)

        case .upDownStack:
            break
        }
    }

    private func handleKeyUp(_ spec: LaptopKeySpec) {
        switch spec.role {
        case .regular(let hid, _, _, let fnHid, _, _):
            keyUpHaptic.impactOccurred(intensity: 0.22)
            let actualHid = (fnActive && fnHid != nil) ? fnHid! : hid
            WinIOSBridge.shared.sendKeyHid(actualHid, down: false)
            releaseLatchedModifiers()
        case .functionKey(let action, _, _, _, _) where action == .forwardDelete:
            keyUpHaptic.impactOccurred(intensity: 0.22)
        default:
            break
        }
    }

    private func performFunctionAction(_ action: FunctionAction) {
        switch action {
        case .escape:
            WinIOSBridge.shared.sendEscape()
        case .startMenu:
            WinIOSBridge.shared.sendWindowsKey()
        case .search:
            WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe3)], key: 0x16) // Win+S
        case .explorer:
            WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe3)], key: 0x08) // Win+E
        case .edgeBrowser:
            vm.launchGuestEdgeBrowser()
        case .antigravityApp:
            vm.launchGuestAntigravity()
        case .taskManager:
            WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe0), NSNumber(value: 0xe1)], key: 0x29)
        case .altTab:
            WinIOSBridge.shared.sendAltTab()
        case .showDesktop:
            WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe3)], key: 0x07) // Win+D
        case .rightClickArm:
            vm.rightClickArmed.toggle()
        case .dragLockToggle:
            dragLocked.toggle()
            PointerController.shared.setHeldButton(1, down: dragLocked)
        case .cycleDeckLayout:
            withAnimation(.snappy(duration: 0.2)) {
                vm.deckLayout = vm.deckLayout.next
            }
        case .rotateScreen:
            SceneOrientationController.rotateDeckOrientation()
        case .openStudioMode:
            withAnimation(.spring(response: 0.28, dampingFraction: 0.88)) {
                vm.postureMode = .fullScreen
            }
        case .acpiPower:
            WinIOSBridge.shared.pressPowerButton()
        case .forwardDelete:
            WinIOSBridge.shared.tapKeyHid(0x4c)
        }
    }

    private func toggleModifierLatch(hid: UInt8) {
        switch hid {
        case 0xe1, 0xe5: shiftLatched.toggle()
        case 0xe0: ctrlLatched.toggle()
        case 0xe2, 0xe6: altLatched.toggle()
        case 0xe3:
            if winLatched {
                winLatched = false
                WinIOSBridge.shared.sendWindowsKey()
            } else {
                winLatched = true
            }
        default: break
        }
    }

    // MARK: - Seamless Flush Midnight-Sapphire Precision Haptic Glass Touchpad

    private func flushSurfaceHapticTouchpad(width: CGFloat, height: CGFloat, expanded: Bool) -> some View {
        let padW = expanded
            ? max(240.0, width - 36.0)
            : min(max(246.0, width * 0.56), width - 64.0)
        let activeGlow = padTouching || cornerClickState != 0 || dragLocked

        return HStack(spacing: 0) {
            Spacer(minLength: 0)

            ZStack(alignment: .bottom) {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(KeyboardStyle.trackpad)
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(Color.white.opacity(activeGlow ? 0.35 : 0.08), lineWidth: 1)

                // Hairline tactile corner feedback glow when left/right bottom haptic zone or drag-lock is engaged
                if cornerClickState != 0 || dragLocked {
                    HStack {
                        if cornerClickState == 1 || dragLocked {
                            Capsule()
                                .fill(Color.white.opacity(0.65))
                                .frame(width: 34, height: 1.8)
                        }
                        Spacer()
                        if cornerClickState == 2 {
                            Capsule()
                                .fill(Color.white.opacity(0.65))
                                .frame(width: 34, height: 1.8)
                        }
                    }
                    .padding(.horizontal, 14)
                    .padding(.bottom, 5)
                    .allowsHitTesting(false)
                }

                TrackpadGlassRepresentable(
                    isTouching: $padTouching,
                    cornerClickState: $cornerClickState
                )
            }
            .frame(width: padW, height: height)

            Spacer(minLength: 0)
        }
    }
}

// MARK: - Surface Stacked Up / Down Half-Height Arrow Pair

private struct SurfaceStackedArrowView: View {
    let isFnActive: Bool
    let onTapArrow: (Bool) -> Void

    @State private var upPressed = false
    @State private var downPressed = false

    var body: some View {
        GeometryReader { geo in
            let halfH = max(8.0, (geo.size.height - 1.8) * 0.5)
            VStack(spacing: 1.8) {
                halfArrowButton(isUp: true, pressed: $upPressed)
                    .frame(height: halfH)
                halfArrowButton(isUp: false, pressed: $downPressed)
                    .frame(height: halfH)
            }
        }
    }

    private func halfArrowButton(isUp: Bool, pressed: Binding<Bool>) -> some View {
        let cornerTop: CGFloat = isUp ? 4.5 : 1.5
        let cornerBot: CGFloat = isUp ? 1.5 : 4.5
        return ZStack {
            UnevenRoundedRectangle(
                topLeadingRadius: cornerTop,
                bottomLeadingRadius: cornerBot,
                bottomTrailingRadius: cornerBot,
                topTrailingRadius: cornerTop,
                style: .continuous
            )
            .fill(pressed.wrappedValue ? KeyboardStyle.pressedKey : KeyboardStyle.systemKey)
            .shadow(color: KeyboardStyle.keyShadow, radius: 0, x: 0, y: 1)

            Image(systemName: isUp ? "chevron.up" : "chevron.down")
                .font(.system(size: 7.5, weight: .bold))
                .foregroundStyle(Color.white)
        }
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !pressed.wrappedValue {
                        pressed.wrappedValue = true
                        onTapArrow(isUp)
                    }
                }
                .onEnded { _ in
                    pressed.wrappedValue = false
                }
        )
    }
}

// MARK: - Matte Obsidian-Midnight Surface Chiclet Keycap

private struct SurfaceKeycapView: View {
    let spec: LaptopKeySpec
    let isFunctionStrip: Bool
    let isShiftActive: Bool
    let isLatchedOrLED: Bool
    let isFnActive: Bool
    let onKeyDown: () -> Void
    let onKeyUp: () -> Void

    @State private var isPressed: Bool = false
    @State private var repeatTimer: Timer? = nil
    /// Resets even when the system cancels the touch (which skips onEnded).
    @GestureState private var isTouching = false

    var body: some View {
        let radius: CGFloat = isFunctionStrip ? 3.8 : 4.6

        ZStack {
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(keyFill)
                .shadow(color: KeyboardStyle.keyShadow, radius: 0, x: 0, y: 1)

            keycapLegend
        }
        .gesture(
            DragGesture(minimumDistance: 0)
                .updating($isTouching) { _, touching, _ in
                    touching = true
                }
                .onChanged { _ in
                    if !isPressed {
                        isPressed = true
                        onKeyDown()
                        startAutoRepeatIfApplicable()
                    }
                }
                .onEnded { _ in
                    releaseKey()
                }
        )
        .onChange(of: isTouching) { _, touching in
            if !touching { releaseKey() }
        }
        // A key held through a fold loses its gesture's onEnded as well.
        .onDisappear { releaseKey() }
    }

    /// Stops auto-repeat and sends the key-up once, whichever of onEnded, a cancelled touch or a
    /// teardown gets here first.
    private func releaseKey() {
        stopAutoRepeat()
        if isPressed {
            isPressed = false
            onKeyUp()
        }
    }

    /// Letter, number and space keys are the lighter grey; modifiers and the function row the darker.
    private var isCharacterKey: Bool {
        if case .regular(_, let primary, _, _, _, _) = spec.role {
            return primary.count == 1 || primary == "Space"
        }
        return false
    }

    private var keyFill: Color {
        if isLatchedOrLED { return KeyboardStyle.latchedKey }
        if isPressed { return isCharacterKey ? KeyboardStyle.pressedKey : KeyboardStyle.key }
        return isCharacterKey ? KeyboardStyle.key : KeyboardStyle.systemKey
    }

    @ViewBuilder
    private var keycapLegend: some View {
        let fg: Color = isLatchedOrLED ? .black : .white
        let subFg = fg.opacity(0.55)

        switch spec.role {
        case .functionKey(let action, _, let fnTag, let symbol, let text):
            if isFnActive, let fnTag {
                Text(fnTag)
                    .font(.system(size: 9.0, weight: .semibold))
                    .foregroundStyle(fg)
            } else if action == .startMenu {
                WindowsLogoMark(size: 8.8, color: fg.opacity(0.92))
            } else if let text {
                Text(text)
                    .font(.system(size: 8.8, weight: .medium))
                    .foregroundStyle(fg.opacity(0.88))
            } else if let symbol {
                Image(systemName: symbol)
                    .font(.system(size: 8.8, weight: .medium))
                    .foregroundStyle(fg.opacity(0.88))
            }

        case .capsLock:
            Text("Caps")
                .font(.system(size: 9.8, weight: .medium))
                .foregroundStyle(fg.opacity(0.90))

        case .fnToggle:
            Text("Fn")
                .font(.system(size: 9.8, weight: .medium))
                .foregroundStyle(fg.opacity(0.90))

        case .modifier(_, let label, let isWindowsKey):
            if isWindowsKey {
                WindowsLogoMark(size: 10.5, color: fg.opacity(0.92))
            } else {
                Text(label)
                    .font(.system(size: 9.8, weight: .medium))
                    .foregroundStyle(fg.opacity(0.90))
            }

        case .regular(_, let primary, let shifted, _, let fnLabel, let homing):
            if isFnActive, let fnLabel {
                Text(fnLabel)
                    .font(.system(size: 9.2, weight: .semibold))
                    .foregroundStyle(fg)
            } else if primary == "Space" {
                EmptyView() // Clean unadorned matte Surface spacebar!
            } else if primary == "Backspace" {
                Image(systemName: "delete.left")
                    .font(.system(size: 10.5, weight: .regular))
                    .foregroundStyle(fg.opacity(0.88))
            } else if primary == "←" || primary == "→" {
                Image(systemName: primary == "←" ? "chevron.left" : "chevron.right")
                    .font(.system(size: 9.0, weight: .bold))
                    .foregroundStyle(fg.opacity(0.90))
            } else if let shifted, shifted.uppercased() != primary.uppercased() {
                VStack(spacing: 0) {
                    Text(shifted)
                        .font(.system(size: 7.8, weight: .regular))
                        .foregroundStyle(isShiftActive ? fg : subFg)
                    Text(primary)
                        .font(.system(size: 10.8, weight: .medium))
                        .foregroundStyle(isShiftActive ? subFg : fg)
                }
            } else {
                let text = (isShiftActive && shifted != nil) ? shifted! : (primary.count == 1 ? primary.uppercased() : primary)
                VStack(spacing: 1.5) {
                    Text(text)
                        .font(.system(size: primary.count > 1 ? 9.8 : 11.5, weight: .medium))
                        .foregroundStyle(fg.opacity(0.94))
                    if homing {
                        Capsule()
                            .fill(fg.opacity(0.45))
                            .frame(width: 8, height: 1.1)
                    }
                }
            }

        case .upDownStack:
            EmptyView()
        }
    }

    private func startAutoRepeatIfApplicable() {
        var repeatCode: UInt8? = nil
        switch spec.role {
        case .regular(let hid, _, _, let fnHid, _, _):
            let actual = (isFnActive && fnHid != nil) ? fnHid! : hid
            if [0x2a, 0x2c, 0x4c, 0x4f, 0x50].contains(actual) {
                repeatCode = actual
            }
        case .functionKey(let action, _, _, _, _) where action == .forwardDelete:
            repeatCode = 0x4c
        default:
            break
        }
        guard let code = repeatCode else { return }

        stopAutoRepeat()
        repeatTimer = Timer.scheduledTimer(withTimeInterval: 0.36, repeats: false) { _ in
            Task { @MainActor in
                guard self.isPressed else { return }
                self.repeatTimer = Timer.scheduledTimer(withTimeInterval: 0.065, repeats: true) { _ in
                    Task { @MainActor in
                        guard self.isPressed else {
                            self.stopAutoRepeat()
                            return
                        }
                        WinIOSBridge.shared.tapKeyHid(code)
                    }
                }
            }
        }
    }

    private func stopAutoRepeat() {
        repeatTimer?.invalidate()
        repeatTimer = nil
    }
}
