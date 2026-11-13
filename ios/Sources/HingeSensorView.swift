// SPDX-License-Identifier: GPL-2.0-or-later
import QuartzCore
import SwiftUI
import UIKit

/// Shared pointer state between the upper Windows 11 Metal screen and the lower Laptop Glass Trackpad.
@MainActor
final class PointerController {
    static let shared = PointerController()

    var cursorX: Double = 0.5
    var cursorY: Double = 0.5
    private(set) var heldButtons: UInt8 = 0

    private var lastTapTime: CFTimeInterval = 0
    private var lastTapX: Double = 0.5
    private var lastTapY: Double = 0.5

    private init() {}

    func moveAbsolute(x: Double, y: Double, extraButtons: UInt8 = 0) {
        cursorX = min(1.0, max(0.0, x))
        cursorY = min(1.0, max(0.0, y))
        WinIOSBridge.shared.sendTabletNormalizedX(cursorX, y: cursorY, buttons: heldButtons | extraButtons, wheel: 0)
    }

    func moveRelative(dxPoints: CGFloat, dyPoints: CGFloat, padWidth: CGFloat, padHeight: CGFloat, extraButtons: UInt8 = 0) {
        // Natural laptop trackpad ballistics: fine pixel control at low speed, brisk traversal on flicks.
        let dist = hypot(Double(dxPoints), Double(dyPoints))
        let gain = min(2.35, 0.95 + dist * 0.055)
        let refW = Double(max(220.0, padWidth))
        let refH = Double(max(130.0, padHeight))
        let dx = (Double(dxPoints) / refW) * gain
        let dy = (Double(dyPoints) / refH) * gain
        cursorX = min(1.0, max(0.0, cursorX + dx))
        cursorY = min(1.0, max(0.0, cursorY + dy))
        WinIOSBridge.shared.sendTabletNormalizedX(cursorX, y: cursorY, buttons: heldButtons | extraButtons, wheel: 0)
    }

    func setHeldButton(_ mask: UInt8, down: Bool) {
        if down {
            heldButtons |= mask
        } else {
            heldButtons &= ~mask
        }
        WinIOSBridge.shared.sendTabletNormalizedX(cursorX, y: cursorY, buttons: heldButtons, wheel: 0)
    }

    func scroll(steps: Int8) {
        guard steps != 0 else { return }
        WinIOSBridge.shared.sendTabletNormalizedX(cursorX, y: cursorY, buttons: heldButtons, wheel: steps)
    }

    /// Direct-touch tap on the Windows display with automatic double-click coordinate snapping
    /// and hover-before-press sequencing for Windows 11 XAML / Desktop icons.
    func directTapAt(x: Double, y: Double, buttonMask: UInt8) {
        let now = CACurrentMediaTime()
        let dist = hypot(x - lastTapX, y - lastTapY)
        if now - lastTapTime < 0.40 && dist < 0.045 {
            // Snap rapid second tap to the exact pixel of the first tap so Win32 double-clicks register 100% reliably!
            cursorX = lastTapX
            cursorY = lastTapY
        } else {
            cursorX = min(1.0, max(0.0, x))
            cursorY = min(1.0, max(0.0, y))
            lastTapX = cursorX
            lastTapY = cursorY
        }
        lastTapTime = now

        let tx = cursorX
        let ty = cursorY
        // 1. Move cursor onto target so XAML hover states engage
        WinIOSBridge.shared.sendTabletNormalizedX(tx, y: ty, buttons: heldButtons, wheel: 0)
        // 2. Press button after 14 ms
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.014) { [weak self] in
            guard let self else { return }
            WinIOSBridge.shared.sendTabletNormalizedX(tx, y: ty, buttons: self.heldButtons | buttonMask, wheel: 0)
            // 3. Release button after 55 ms
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.055) { [weak self] in
                guard let self else { return }
                WinIOSBridge.shared.sendTabletNormalizedX(tx, y: ty, buttons: self.heldButtons, wheel: 0)
            }
        }
    }

    /// Trackpad tap-to-click at the current cursor position.
    func clickAtCurrent(buttonMask: UInt8) {
        let tx = cursorX
        let ty = cursorY
        WinIOSBridge.shared.sendTabletNormalizedX(tx, y: ty, buttons: heldButtons | buttonMask, wheel: 0)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.048) { [weak self] in
            guard let self else { return }
            WinIOSBridge.shared.sendTabletNormalizedX(tx, y: ty, buttons: self.heldButtons, wheel: 0)
        }
    }
}

/// The F11 deck key's scene rotation. Layout never asks for an orientation: the iPhone Duo inner
/// display ignores supported orientations, and the fold's reserved region decides the posture.
@MainActor
enum SceneOrientationController {
    static func activeWindowScene() -> UIWindowScene? {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .first { $0.activationState == .foregroundActive }
            ?? UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
    }

    /// Cycles scene orientation 90°/180° even when system Orientation Lock is active.
    static func rotateDeckOrientation() {
        guard let scene = activeWindowScene() else { return }
        let next: UIInterfaceOrientationMask
        switch scene.effectiveGeometry.interfaceOrientation {
        case .portrait:
            next = .portraitUpsideDown
        case .portraitUpsideDown:
            next = .landscapeRight
        case .landscapeRight:
            next = .landscapeLeft
        default:
            next = .portrait
        }
        scene.requestGeometryUpdate(.iOS(interfaceOrientations: next)) { _ in }
    }
}

final class HingeObserverViewController: UIViewController {
    var onHingeUpdate: ((Int, Double?) -> Void)?
    private var didInstallHinge = false

    override var prefersStatusBarHidden: Bool { true }
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
    override var prefersHomeIndicatorAutoHidden: Bool { true }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .clear
        view.isUserInteractionEnabled = false
        installHingeObserverIfAvailable()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
        setNeedsUpdateOfHomeIndicatorAutoHidden()
        parent?.setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
        parent?.setNeedsUpdateOfHomeIndicatorAutoHidden()
        installHingeObserverIfAvailable()
    }

    private func installHingeObserverIfAvailable() {
        guard !didInstallHinge else { return }
        didInstallHinge = true
        guard NSClassFromString("UIHingeInteraction") != nil else { return }
        if #available(iOS 27.1, *) {
            let interaction = UIHingeInteraction { [weak self] _, update in
                guard let self else { return }
                guard let hinge = update.hinge else {
                    WinIOSBridge.shared.updateHingeStatus(0, angleDegrees: -1.0)
                    self.onHingeUpdate?(0, nil)
                    return
                }
                let rawStatus = hinge.status.rawValue
                let deg = Double(hinge.angle) * 180.0 / .pi
                WinIOSBridge.shared.updateHingeStatus(rawStatus, angleDegrees: deg)
                self.onHingeUpdate?(rawStatus, deg)
            }
            view.addInteraction(interaction)
        }
    }
}

struct HingeSensorView: UIViewControllerRepresentable {
    var onHingeUpdate: (Int, Double?) -> Void

    func makeUIViewController(context: Context) -> HingeObserverViewController {
        let vc = HingeObserverViewController()
        vc.onHingeUpdate = onHingeUpdate
        return vc
    }

    func updateUIViewController(_ uiViewController: HingeObserverViewController, context: Context) {
        uiViewController.onHingeUpdate = onHingeUpdate
    }
}
