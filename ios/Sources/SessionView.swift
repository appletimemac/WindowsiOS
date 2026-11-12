// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

struct SessionView: View {
    @ObservedObject var vm: VMViewModel
    @State private var fabExpanded = false
    @State private var confirmingShutDown = false
    /// Bottom leading by default: clear of the Windows tray and clock, and of a maximized
    /// window's close button.
    @AppStorage("fabCorner") private var fabCorner: FABCorner = .bottomLeading

    var body: some View {
        // A reader that ignores the safe area reads its insets as zero (measured on the iOS 27.2
        // simulator), so the outer reader stays inside it and reports the insets (camera, home
        // indicator and, while it is up, the keyboard). The inner reader spans the whole window
        // for the fold and the layouts, which pad themselves by those insets.
        GeometryReader { safe in
            let insets = safe.safeAreaInsets
            GeometryReader { geo in
                windowContent(geo: geo, insets: insets)
            }
            .ignoresSafeArea()
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .defersSystemGestures(on: .all)
    }

    @ViewBuilder
    private func windowContent(geo: GeometryProxy, insets: EdgeInsets) -> some View {
        let size = geo.size
        let fold = activeFold(in: geo)
        let laptopActive = vm.isLaptopLayoutActive(fold: fold)
        // Floating chrome lives in the pane before the crease (the lid in laptop layout),
        // inside the safe area.
        let pane = PostureResolver.primaryPane(windowSize: size, fold: fold, laptop: laptopActive)
            .intersection(safeRect(size: size, insets: insets))

        ZStack {
            Color.black.ignoresSafeArea()

            // Invisible UIKit host for iOS 27.1 UIHingeInteraction & system edge-gesture deferral
            HingeSensorView { statusRaw, angleDeg in
                vm.updateHinge(statusRaw: statusRaw, angleDegrees: angleDeg)
            }
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)

            if laptopActive {
                laptopLayout(size: size, insets: insets, fold: fold)
                    .transition(.opacity)
            } else {
                studioLayout(insets: insets)
                    .transition(.opacity)
            }

            if !vm.isVMRunning {
                BootCard(vm: vm)
                    .padding(20)
                    .frame(width: pane.width, height: pane.height)
                    .position(x: pane.midX, y: pane.midY)
            }

            FloatingActionMenu(isExpanded: $fabExpanded, corner: $fabCorner, region: pane) {
                fabItems(laptopActive: laptopActive)
            }
        }
        .sheet(item: $vm.activeSafariURL) { item in
            SafariSheet(url: item.url)
                .ignoresSafeArea()
                .preferredColorScheme(.dark)
        }
        .alert("Shut down Windows?", isPresented: $confirmingShutDown) {
            Button("Shut Down", role: .destructive) {
                WinIOSBridge.shared.pressPowerButton()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Windows gets a power-button press and closes its apps.")
        }
    }

    /// The hinge's division reserved region (iOS 27.1), active only while the Duo is partially
    /// folded. Read from this full-window reader; the 0x0 hinge host never intersects the fold.
    private func activeFold(in geo: GeometryProxy) -> FoldGeometry? {
        var divisionFrames: [CGRect] = []
        if #available(iOS 27.1, *) {
            divisionFrames = geo.reservedRegions(kind: .division).filter(\.isActive).map(\.frame)
        }
        return PostureResolver.fold(divisionFrames: divisionFrames, hinge: vm.hingeStatus, windowSize: geo.size)
    }

    private func safeRect(size: CGSize, insets: EdgeInsets) -> CGRect {
        CGRect(
            x: insets.leading,
            y: insets.top,
            width: max(0, size.width - insets.leading - insets.trailing),
            height: max(0, size.height - insets.top - insets.bottom)
        )
    }

    private func openFAB() {
        withAnimation(.fabBounce) { fabExpanded = true }
    }

    private func setPosture(_ mode: PostureMode) {
        withAnimation(.spring(response: 0.28, dampingFraction: 0.88)) {
            vm.postureMode = mode
            if mode == .laptop { vm.systemKeyboardVisible = false }
        }
    }

    // MARK: - Layouts

    /// Windows edge to edge inside the safe area; the canvas letterboxes the guest framebuffer.
    private func studioLayout(insets: EdgeInsets) -> some View {
        MetalDisplayView(
            trackpadMode: $vm.trackpadMode,
            rightClickArmed: $vm.rightClickArmed,
            keyboardVisible: $vm.systemKeyboardVisible,
            onThreeFingerTap: openFAB
        )
        .padding(insets)
    }

    /// Windows on the lid, the keyboard deck below; the split lands on the crease when folded.
    private func laptopLayout(size: CGSize, insets: EdgeInsets, fold: FoldGeometry?) -> some View {
        let split = PostureResolver.laptopSplit(height: size.height, fold: fold)
        return VStack(spacing: 0) {
            MetalDisplayView(
                trackpadMode: $vm.trackpadMode,
                rightClickArmed: $vm.rightClickArmed,
                keyboardVisible: .constant(false),
                onThreeFingerTap: openFAB
            )
            .padding(EdgeInsets(top: insets.top, leading: insets.leading, bottom: 0, trailing: insets.trailing))
            .frame(width: size.width, height: split.lid)

            // The physical crease while half-folded: nothing interactive here.
            if split.crease > 0 {
                Color.black
                    .frame(width: size.width, height: split.crease)
            }

            LaptopDeckView(vm: vm)
                .frame(width: size.width, height: split.deck)
        }
        .frame(width: size.width, height: size.height, alignment: .top)
    }

    // MARK: - FAB menu

    /// Listed top to bottom; the last item sits next to the button.
    @ViewBuilder
    private func fabItems(laptopActive: Bool) -> some View {
        if vm.isVMRunning {
            FABMenuItem(title: "Shut Down", icon: "power", role: .destructive) {
                confirmingShutDown = true
            }
            FABMenuItem(title: "Task Manager", icon: "waveform.path.ecg") {
                WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe0), NSNumber(value: 0xe1)], key: 0x29)
            }
            FABMenuItem(title: "Antigravity", icon: "sparkles") {
                vm.launchGuestAntigravity()
            }
            FABMenuItem(title: "Edge", icon: "globe") {
                vm.launchGuestEdgeBrowser()
            }
            FABMenuItem(title: "Sign In with Safari", icon: "safari", isOn: vm.useSafariForOAuth) {
                vm.toggleOAuthBrowserMode()
            }
            FABMenuItem(title: "Right Click", icon: "computermouse", isOn: vm.rightClickArmed) {
                vm.rightClickArmed.toggle()
            }
            FABMenuItem(title: "Trackpad", icon: "rectangle.and.hand.point.up.left", isOn: vm.trackpadMode) {
                vm.trackpadMode.toggle()
            }
            if !laptopActive {
                FABMenuItem(title: "Keyboard", icon: "keyboard", isOn: vm.systemKeyboardVisible) {
                    vm.systemKeyboardVisible.toggle()
                }
            }
        }
        if vm.postureMode != .auto {
            FABMenuItem(title: "Follow Hinge", icon: PostureMode.auto.iconName) {
                setPosture(.auto)
            }
        }
        if laptopActive {
            FABMenuItem(title: "Full Screen", icon: "arrow.up.left.and.arrow.down.right") {
                setPosture(.fullScreen)
            }
        } else {
            FABMenuItem(title: "Laptop", icon: "laptopcomputer") {
                setPosture(.laptop)
            }
        }
        if vm.isVMRunning {
            FABMenuItem(title: "Start", icon: "square.grid.2x2") {
                WinIOSBridge.shared.sendWindowsKey()
            }
        }
    }
}

// MARK: - Boot card

private struct BootCard: View {
    @ObservedObject var vm: VMViewModel

    var body: some View {
        ViewThatFits(in: .vertical) {
            card
            ScrollView { card }
                .scrollBounceBehavior(.basedOnSize)
        }
        .padding(24)
        .frame(maxWidth: 420)
        .glassEffect(.regular, in: .rect(cornerRadius: 28))
    }

    private var card: some View {
        VStack(spacing: 18) {
            WindowsLogoMark(size: 36, color: .white)
                .accessibilityHidden(true)

            VStack(spacing: 6) {
                Text("Windows 11")
                    .font(.title2.weight(.semibold))
                Text(vm.vmStatusText)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }

            if vm.isUnpacking {
                ProgressView(value: vm.unpackProgress)
                    .frame(maxWidth: 260)
            }

            VStack(alignment: .leading, spacing: 8) {
                Label(vm.isJITLive ? "JIT enabled" : "JIT not enabled",
                      systemImage: vm.isJITLive ? "bolt.fill" : "bolt.slash")
                if vm.guestRAMMB > 0 {
                    Label("\(Self.size(megabytes: vm.guestRAMMB)) for Windows", systemImage: "memorychip")
                }
                Label("\(Self.size(megabytes: vm.availableMemoryMB)) headroom", systemImage: "gauge.with.dots.needle.33percent")
            }
            .font(.footnote)
            .foregroundStyle(.secondary)

            if !vm.isUnpacking {
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: 12) { buttons }
                    VStack(spacing: 12) { buttons }
                }
                .controlSize(.large)
            }
        }
    }

    @ViewBuilder
    private var buttons: some View {
        if !vm.isJITLive {
            Button("Enable JIT", systemImage: "bolt") {
                vm.requestStikDebugJIT()
            }
            .buttonStyle(.glass)
        }
        Button(vm.isJITLive ? "Start Windows" : "Try Again",
               systemImage: vm.isJITLive ? "play.fill" : "arrow.clockwise") {
            vm.startOrRetry()
        }
        .buttonStyle(.glassProminent)
    }

    private static func size(megabytes: UInt64) -> String {
        Int64(megabytes << 20).formatted(.byteCount(style: .memory))
    }
}
