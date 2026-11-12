// SPDX-License-Identifier: GPL-2.0-or-later
import Combine
import Foundation
import SafariServices
import SwiftUI
import UIKit

struct IdentifiableURL: Identifiable {
    let id = UUID()
    let url: URL
}

struct SafariSheet: UIViewControllerRepresentable {
    let url: URL
    func makeUIViewController(context: Context) -> SFSafariViewController {
        let vc = SFSafariViewController(url: url)
        vc.dismissButtonStyle = .done
        return vc
    }
    func updateUIViewController(_ uiViewController: SFSafariViewController, context: Context) {}
}

enum DeckLayout: String, CaseIterable {
    case split = "Split"
    case trackpad = "Trackpad"
    case keyboard = "Keyboard"

    var iconName: String {
        switch self {
        case .split: return "laptopcomputer"
        case .trackpad: return "rectangle.and.hand.point.up.left"
        case .keyboard: return "keyboard"
        }
    }

    var next: DeckLayout {
        switch self {
        case .split: return .trackpad
        case .trackpad: return .keyboard
        case .keyboard: return .split
        }
    }
}

@MainActor
final class VMViewModel: ObservableObject {
    @Published var isJITLive: Bool = false
    @Published var isDebuggerAttached: Bool = false
    @Published var isVMRunning: Bool = false
    @Published var isUnpacking: Bool = false
    @Published var unpackProgress: Double = 0
    @Published var jitStatusText: String = "Checking JIT..."
    @Published var vmStatusText: String = "Initializing..."
    @Published var fbWidth: UInt32 = 0
    @Published var fbHeight: UInt32 = 0
    @Published var presentedFrames: UInt64 = 0
    @Published var availableMemoryMB: UInt64 = 0
    @Published var guestRAMMB: UInt64 = 0

    @Published var usbKeyboardReady: Bool = false
    @Published var usbTabletReady: Bool = false
    @Published var usbEventsDelivered: UInt64 = 0

    // iOS 27.1 UIHinge state (0 = unknown, 1 = closed, 2 = partiallyOpen, 3 = fullyOpen)
    @Published var hingeStatusRaw: Int = 0
    @Published var hingeAngleDegrees: Double? = nil

    @Published var postureMode: PostureMode = .auto
    @Published var deckLayout: DeckLayout = .split
    @Published var trackpadMode: Bool = false
    @Published var rightClickArmed: Bool = false
    @Published var systemKeyboardVisible: Bool = false
    @Published var activeSafariURL: IdentifiableURL? = nil
    @Published var useSafariForOAuth: Bool = WinIOSBridge.shared.useSafariForGuestLinks

    /// What the StikDebug hand-off is doing, shown on the boot card in place of the VM status.
    @Published var stikDebugStatus: String? = nil

    private var didAutoLaunchAttempt = false
    private var didAutoRequestJIT = false
    private var debuggerWatch: Timer? = nil
    private var lastJITAttempt = Date.distantPast
    private var activeObserver: NSObjectProtocol? = nil
    private var oauthPollTimer: Timer? = nil

    init() {
        let bridge = WinIOSBridge.shared
        bridge.installTrapGuard()
        bridge.onStateChanged = { [weak self] in
            Task { @MainActor in
                self?.syncFromBridge()
            }
        }
        syncFromBridge()
        startOAuthPoller()
        // The app is only reported active from here on; the hand-off to StikDebug needs that.
        activeObserver = NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.onAppBecameActive() }
        }
        Task { @MainActor [weak self] in
            self?.onAppBecameActive()
        }
    }

    private func startOAuthPoller() {
        oauthPollTimer?.invalidate()
        oauthPollTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self else { return }
                let b = WinIOSBridge.shared
                if b.takeGuestOAuthDone() {
                    if self.activeSafariURL != nil {
                        UINotificationFeedbackGenerator().notificationOccurred(.success)
                        self.activeSafariURL = nil
                    }
                }
                if let urlStr = b.takeGuestOpenURL(), let u = URL(string: urlStr) {
                    UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                    self.activeSafariURL = IdentifiableURL(url: u)
                }
            }
        }
    }

    func toggleOAuthBrowserMode() {
        useSafariForOAuth.toggle()
        WinIOSBridge.shared.useSafariForGuestLinks = useSafariForOAuth
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }

    var hingeStatus: HingeStatus {
        HingeStatus(rawValue: hingeStatusRaw) ?? .unknown
    }

    /// Layout follows the fold's geometry (the system's division reserved region); the hinge
    /// status only releases a manual choice and drives the haptic.
    func isLaptopLayoutActive(fold: FoldGeometry?) -> Bool {
        PostureResolver.isLaptop(mode: postureMode, fold: fold)
    }

    func updateHinge(statusRaw: Int, angleDegrees: Double?) {
        let old = hingeStatus
        hingeStatusRaw = statusRaw
        hingeAngleDegrees = angleDegrees
        let new = hingeStatus
        guard old != new else { return }
        let resolved = PostureResolver.mode(postureMode, afterHingeChangeFrom: old, to: new)
        if resolved != postureMode {
            withAnimation(.spring(response: 0.28, dampingFraction: 0.88)) {
                postureMode = resolved
            }
        }
        if postureMode == .auto && new == .partiallyOpen && old != .unknown {
            UIImpactFeedbackGenerator(style: .soft).impactOccurred()
        }
    }

    func launchGuestEdgeBrowser() {
        // Win+R -> "msedge" -> Enter
        WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe3)], key: 0x15)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.42) {
            let msedgeHids: [UInt8] = [0x10, 0x16, 0x08, 0x07, 0x0a, 0x08, 0x28] // m s e d g e Enter
            for (idx, code) in msedgeHids.enumerated() {
                DispatchQueue.main.asyncAfter(deadline: .now() + Double(idx) * 0.045) {
                    WinIOSBridge.shared.tapKeyHid(code)
                }
            }
        }
    }

    func launchGuestAntigravity() {
        // Win+R -> "antigravity" -> Enter (registered in HKCU\...\App Paths\antigravity.exe)
        WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe3)], key: 0x15)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.42) {
            // a n t i g r a v i t y Enter
            let agHids: [UInt8] = [0x04, 0x11, 0x17, 0x0c, 0x0a, 0x15, 0x04, 0x19, 0x0c, 0x17, 0x1c, 0x28]
            for (idx, code) in agHids.enumerated() {
                DispatchQueue.main.asyncAfter(deadline: .now() + Double(idx) * 0.040) {
                    WinIOSBridge.shared.tapKeyHid(code)
                }
            }
        }
    }

    func syncFromBridge() {
        let b = WinIOSBridge.shared
        isJITLive = b.isJITLive
        isDebuggerAttached = b.isDebuggerAttached
        isVMRunning = b.isVMRunning
        isUnpacking = b.isUnpacking
        unpackProgress = b.unpackProgress
        jitStatusText = b.jitStatusText
        vmStatusText = b.vmStatusText
        fbWidth = b.framebufferWidth
        fbHeight = b.framebufferHeight
        presentedFrames = b.presentedFrames
        availableMemoryMB = b.availableMemoryMB
        guestRAMMB = b.guestRAMMB
        usbKeyboardReady = b.usbKeyboardReady
        usbTabletReady = b.usbTabletReady
        usbEventsDelivered = b.usbEventsDelivered
    }

    func onAppBecameActive() {
        let b = WinIOSBridge.shared
        if !b.isJITLive && b.isDebuggerAttached {
            _ = b.tryAcquireJIT(true)
        }
        syncFromBridge()
        if b.isJITLive {
            stopWatchingForDebugger()
            stikDebugStatus = nil
            if !b.isVMRunning && !b.isUnpacking && b.hasProvisionedMachine() && !didAutoLaunchAttempt {
                didAutoLaunchAttempt = true
                b.prepareAndStart(with: nil)
                syncFromBridge()
            }
        } else if !b.isDebuggerAttached && b.hasProvisionedMachine() && !didAutoRequestJIT
                    && UIApplication.shared.applicationState == .active {
            // Opened from the Home Screen with no debugger: StikDebug sets up the JIT on the phone.
            // iOS opens other apps only from an active app, so this waits for the active scene.
            requestStikDebugJIT()
        }
    }

    func startOrRetry() {
        didAutoLaunchAttempt = true
        stikDebugStatus = nil
        WinIOSBridge.shared.prepareAndStart(with: nil)
        syncFromBridge()
    }

    /// Hands the JIT setup to StikDebug: it attaches to this process and runs henge-jit.js, which
    /// answers the app's brk handshake (see winios_jit.c) and detaches once the JIT is live.
    func requestStikDebugJIT() {
        // Counts as the launch hand-off too, so returning to the app does not hand off again.
        didAutoRequestJIT = true
        guard let url = Self.stikDebugURL() else {
            stikDebugStatus = "The JIT script is missing from the app"
            return
        }
        stikDebugStatus = "Opening StikDebug"
        UIApplication.shared.open(url) { [weak self] opened in
            Task { @MainActor in
                guard let self else { return }
                if opened {
                    self.stikDebugStatus = "Waiting for StikDebug to attach"
                    self.watchForDebugger()
                } else {
                    self.stikDebugStatus = "Couldn't open StikDebug. Is it installed?"
                }
            }
        }
    }

    /// StikDebug brings this app back to the front before it attaches, so the debugger arrives
    /// while the app is already active: watch for it, then run the handshake and start Windows.
    private func watchForDebugger() {
        stopWatchingForDebugger()
        let deadline = Date().addingTimeInterval(120)
        debuggerWatch = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.debuggerWatchTick(deadline: deadline) }
        }
    }

    private func debuggerWatchTick(deadline: Date) {
        guard debuggerWatch != nil else { return }
        let b = WinIOSBridge.shared
        if b.isJITLive {
            onAppBecameActive()
            return
        }
        if Date() > deadline {
            stopWatchingForDebugger()
            stikDebugStatus = "StikDebug didn't finish the JIT setup. Check that LocalDevVPN is on, then tap Enable JIT."
            return
        }
        // The debugged flag stays set once any debugger has attached, so an attempt can still find
        // nobody answering. That fails safely before anything is committed, so try about once a second.
        guard b.isDebuggerAttached, Date().timeIntervalSince(lastJITAttempt) >= 1 else { return }
        lastJITAttempt = Date()
        stikDebugStatus = "Setting up JIT"
        // On the next turn, so the status draws before the handshake holds the main thread.
        DispatchQueue.main.async { [weak self] in self?.onAppBecameActive() }
    }

    private func stopWatchingForDebugger() {
        debuggerWatch?.invalidate()
        debuggerWatch = nil
    }

    private static func stikDebugURL() -> URL? {
        guard let bundleID = Bundle.main.bundleIdentifier else { return nil }
        // Without the script StikDebug would only attach and detach, leaving the handshake unanswered.
        guard let scriptPath = Bundle.main.path(forResource: "henge-jit", ofType: "js"),
              let script = try? String(contentsOfFile: scriptPath, encoding: .utf8) else { return nil }
        let b64 = Data(script.utf8).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
        return URL(string: "stikjit://enable-jit?bundle-id=\(bundleID)&pid=\(getpid())&script-data=\(b64)")
    }
}
