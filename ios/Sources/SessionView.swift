// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

struct SessionView: View {
    @ObservedObject var vm: VMViewModel
    @Environment(\.displayScale) private var displayScale
    @Environment(\.layoutDirection) private var layoutDirection
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var toolbarOpen = false
    @State private var showingSettings = false
    @State private var confirmingShutDown = false
    @State private var confirmingHibernate = false

    var body: some View {
        // A reader that ignores the safe area reads its insets as zero (measured on the iOS 27.2
        // simulator), so the outer reader stays inside it, for the software keyboard only: the
        // status bar and home indicator are hidden, and the picture's margins come from the
        // display's corners and cameras instead. The inner reader spans the whole window.
        GeometryReader { safe in
            let keyboardInset = vm.systemKeyboardVisible ? safe.safeAreaInsets.bottom : 0
            GeometryReader { geo in
                windowContent(geo: geo, keyboardInset: keyboardInset)
            }
            .ignoresSafeArea()
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        .defersSystemGestures(on: .all)
    }

    @ViewBuilder
    private func windowContent(geo: GeometryProxy, keyboardInset: CGFloat) -> some View {
        let size = geo.size
        let fold = activeFold(in: geo)
        let laptopActive = vm.isLaptopLayoutActive(fold: fold)
        let layout = makeLayout(geo: geo, fold: fold, laptop: laptopActive, keyboardInset: keyboardInset)
        // The boot card lives in the pane before the crease (the lid in laptop layout), inside the
        // picture's margins.
        let pane = PostureResolver.primaryPane(windowSize: size, fold: fold, laptop: laptopActive)
            .intersection(layout.margins.inset(layout.area))

        ZStack {
            Color.black.ignoresSafeArea()

            // Invisible UIKit host for iOS 27.1 UIHingeInteraction & system edge-gesture deferral
            HingeSensorView { statusRaw, angleDeg in
                vm.updateHinge(statusRaw: statusRaw, angleDegrees: angleDeg)
            }
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)

            if laptopActive {
                laptopLayout(size: size, layout: layout)
                    .transition(.opacity)
            } else {
                canvas(layout.picture, in: layout.area, keyboardVisible: $vm.systemKeyboardVisible, followsTextFocus: true)
                    .transition(.opacity)
            }

            if !vm.isVMRunning {
                BootCard(vm: vm)
                    .padding(20)
                    .frame(width: pane.width, height: pane.height)
                    .position(x: pane.midX, y: pane.midY)
            }

            SystemBarEdgeReader { edge in
                toolbar(barEdge: edge, windowSize: size, fold: fold, laptop: laptopActive, layout: layout)
            }
        }
        // Windows' tablet posture follows the layout: the deck is the keyboard.
        .onChange(of: laptopActive, initial: true) { _, laptop in
            WinIOSBridge.shared.setLaptopPosture(laptop)
            vm.laptopLayoutShowing = laptop
            // The deck is the keyboard there. Left on, the software keyboard would come back with the
            // studio layout and its inset would be counted while nothing shows.
            if laptop { vm.systemKeyboardVisible = false }
        }
        // One request for the canvas that is showing; the model debounces it.
        .onChange(of: layout.picture?.size, initial: true) { _, guest in
            vm.requestGuestSize(guest)
        }
        .onChange(of: layout.report(window: size, scale: displayScale), initial: true) { _, report in
            WinIOSBridge.shared.updateGeometryReport(report)
        }
        .sheet(item: $vm.activeSafariURL) { item in
            SafariSheet(url: item.url)
                .ignoresSafeArea()
                .preferredColorScheme(.dark)
        }
        .sheet(isPresented: $showingSettings) {
            SettingsSheet(vm: vm)
                .preferredColorScheme(.dark)
        }
        .alert("Hibernate Windows?", isPresented: $confirmingHibernate) {
            Button("Hibernate") {
                WinIOSBridge.shared.requestHibernate()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Windows saves your open apps and picks up where you left off next time. Needs WindowsiOS Tools.")
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

    /// The display's corner radii (iOS 27.0), or a typical 55 pt where the system gives none.
    private func displayCorners(in geo: GeometryProxy) -> (corners: DisplayCorners, fromSystem: Bool) {
        if #available(iOS 27.0, *), let r = geo.concentricCornerRadii {
            return (DisplayCorners(topLeading: r.topLeading, topTrailing: r.topTrailing,
                                   bottomLeading: r.bottomLeading, bottomTrailing: r.bottomTrailing), true)
        }
        return (.fallback, false)
    }

    /// Camera cutouts (occlusion reserved regions, iOS 27.1) in window coordinates, the ones that
    /// are off included so the geometry report shows them.
    private func cameraRegions(in geo: GeometryProxy) -> [(frame: CGRect, isActive: Bool)] {
        guard #available(iOS 27.1, *) else { return [] }
        return geo.reservedRegions(kind: .occlusion, options: .includeInactive, layoutDirectionBehavior: .fixed)
            .map { ($0.frame, $0.isActive) }
    }

    /// What the software keyboard always leaves of the picture, in points.
    private static let leastPictureHeight: CGFloat = 120

    private func makeLayout(geo: GeometryProxy, fold: FoldGeometry?, laptop: Bool, keyboardInset: CGFloat) -> SessionLayout {
        let size = geo.size
        let (corners, fromSystem) = displayCorners(in: geo)
        let regions = cameraRegions(in: geo)
        // Only a camera that is on takes room. The inner one sits under the display and shows the
        // picture through while it is off (measured on the Duo: counting it cost a 62 pt band along
        // the whole top edge); the picture moves aside if it ever starts.
        let cameras = regions.filter(\.isActive).map(\.frame)
        let split = PostureResolver.laptopSplit(height: size.height, fold: fold)
        let window = CGRect(origin: .zero, size: size)
        let area = laptop ? CGRect(x: 0, y: 0, width: size.width, height: split.lid) : window
        var margins = ScreenMargins.insets(in: area, radii: corners, cameras: cameras,
                                           mode: laptop ? .lid : .fullWindow, scale: displayScale)
        // The software keyboard (studio only) still pushes Windows up, but not off the screen: a canvas
        // squeezed to nothing would be removed, which dismisses the keyboard that squeezed it.
        if !laptop {
            let room = max(margins.bottom, area.height - margins.top - Self.leastPictureHeight)
            margins.bottom = min(max(margins.bottom, keyboardInset), room)
        }
        let deckArea = CGRect(x: 0, y: split.lid + split.crease, width: size.width, height: split.deck)
        return SessionLayout(
            corners: corners, cornersFromSystem: fromSystem, cameras: regions.map(\.frame), camerasActive: regions.map(\.isActive),
            area: area, margins: margins,
            picture: GuestPicture(in: margins.inset(area), scale: displayScale),
            deckArea: deckArea,
            deckMargins: ScreenMargins.insets(in: deckArea, radii: corners, cameras: cameras, mode: .deck, scale: displayScale)
        )
    }

    private func setToolbarOpen(_ open: Bool) {
        withAnimation(.toolbar(reduceMotion: reduceMotion)) { toolbarOpen = open }
    }

    private func setPosture(_ mode: PostureMode) {
        withAnimation(.spring(response: 0.28, dampingFraction: 0.88)) {
            vm.postureMode = mode
            if mode == .laptop { vm.systemKeyboardVisible = false }
        }
    }

    // MARK: - Layouts

    /// Windows, full screen. The canvas fills `area` (the window, or the lid) to the display's
    /// edges. The picture inside it holds exactly the guest's pixels and stops short of the rounded
    /// corners, so nothing Windows draws is cut off, and the canvas carries the picture's edge
    /// pixels on to the glass, so nothing around it is black.
    @ViewBuilder
    private func canvas(_ picture: GuestPicture?, in area: CGRect, keyboardVisible: Binding<Bool>,
                        followsTextFocus: Bool) -> some View {
        if let picture {
            MetalDisplayView(
                trackpadMode: $vm.trackpadMode,
                rightClickArmed: $vm.rightClickArmed,
                keyboardVisible: keyboardVisible,
                pictureFrame: picture.frame.offsetBy(dx: -area.minX, dy: -area.minY),
                onThreeFingerTap: { setToolbarOpen(true) },
                onGuestTap: followsTextFocus ? { x, y in vm.canvasTapped(guestX: x, guestY: y) } : nil,
                onReturnKey: followsTextFocus ? { vm.returnKeyTyped() } : nil
            )
            .frame(width: area.width, height: area.height)
            .position(x: area.midX, y: area.midY)
        }
    }

    /// Windows on the lid, the keyboard deck below; the split lands on the crease when folded.
    private func laptopLayout(size: CGSize, layout: SessionLayout) -> some View {
        VStack(spacing: 0) {
            canvas(layout.picture, in: layout.area, keyboardVisible: .constant(false), followsTextFocus: false)
                .frame(width: size.width, height: layout.area.height)

            // The physical crease while half-folded: nothing interactive here.
            if layout.deckArea.minY > layout.area.maxY {
                Color.black
                    .frame(width: size.width, height: layout.deckArea.minY - layout.area.maxY)
            }

            LaptopDeckView(vm: vm, insets: layout.deckMargins)
                .frame(width: size.width, height: layout.deckArea.height)
        }
        .frame(width: size.width, height: size.height, alignment: .top)
    }

    // MARK: - Toolbar

    /// The bar runs down the side the system keeps its own bars on, where it keeps them on a side;
    /// otherwise it comes from the top, clear of the Windows taskbar.
    private func toolbar(barEdge: HorizontalEdge?, windowSize: CGSize, fold: FoldGeometry?, laptop: Bool,
                         layout: SessionLayout) -> some View {
        let onRight = barEdge.map { ($0 == .trailing) == (layoutDirection == .leftToRight) } ?? false
        let outer = barEdge == nil
            ? PostureResolver.primaryPane(windowSize: windowSize, fold: fold, laptop: laptop)
            : PostureResolver.sidePane(windowSize: windowSize, fold: fold, laptop: laptop, onRight: onRight)
        return SessionToolbar(isOpen: $toolbarOpen, dock: barEdge.map(ToolbarDock.side) ?? .top,
                              outer: outer, region: outer.intersection(layout.margins.inset(layout.area)),
                              cameras: zip(layout.cameras, layout.camerasActive).filter(\.1).map(\.0)) {
            toolbarItems(laptopActive: laptop)
        }
    }

    @ViewBuilder
    private func toolbarItems(laptopActive: Bool) -> some View {
        if vm.isVMRunning {
            if !laptopActive {
                ToolbarButton(title: "Keyboard",
                              systemImage: vm.systemKeyboardVisible ? "keyboard.chevron.compact.down" : "keyboard",
                              isOn: vm.systemKeyboardVisible) {
                    vm.toggleSystemKeyboard()
                }
            }
            ToolbarButton(title: "Trackpad", systemImage: "rectangle.and.hand.point.up.left", isOn: vm.trackpadMode) {
                vm.trackpadMode.toggle()
            }
            ToolbarButton(title: "Start") {
                WinIOSBridge.shared.sendWindowsKey()
            } icon: {
                StartMark()
            }
        }
        Menu {
            Picker("Screen", selection: Binding(get: { vm.postureMode }, set: { mode in
                setToolbarOpen(false)
                setPosture(mode)
            })) {
                ForEach(PostureMode.allCases, id: \.self) { mode in
                    Label(mode.title, systemImage: mode.iconName).tag(mode)
                }
            }
        } label: {
            ToolbarItemLabel(title: "Screen") {
                Image(systemName: (laptopActive ? PostureMode.laptop : PostureMode.fullScreen).iconName)
            }
        }
        .tint(.primary)
        Menu {
            Section {
                if vm.isVMRunning {
                    Button("Task Manager", systemImage: "waveform.path.ecg") {
                        setToolbarOpen(false)
                        WinIOSBridge.shared.sendKeyCombo(withModifiers: [NSNumber(value: 0xe0), NSNumber(value: 0xe1)], key: 0x29)
                    }
                }
                Button("Settings", systemImage: "gearshape") {
                    setToolbarOpen(false)
                    showingSettings = true
                }
            }
            if vm.isVMRunning {
                Section {
                    Button("Hibernate", systemImage: "moon.zzz") {
                        setToolbarOpen(false)
                        confirmingHibernate = true
                    }
                    Button("Shut Down", systemImage: "power", role: .destructive) {
                        setToolbarOpen(false)
                        confirmingShutDown = true
                    }
                }
            }
        } label: {
            ToolbarItemLabel(title: "More") {
                Image(systemName: "ellipsis")
            }
        }
        .tint(.primary)
    }
}

/// The Windows mark for the Start control. Its size is read inside the toolbar, where Dynamic Type stops
/// at the size the bar's slots stop at.
private struct StartMark: View {
    @ScaledMetric(relativeTo: .title3) private var size: CGFloat = 19

    var body: some View {
        WindowsLogoMark(size: size, color: .white)
    }
}

/// The side the system keeps its own bars on in this pose (iOS 27.1: the outer display, the inner
/// display in landscape), or nil where it keeps them across the top and bottom.
private struct SystemBarEdgeReader<Content: View>: View {
    @ViewBuilder var content: (HorizontalEdge?) -> Content

    var body: some View {
        if #available(iOS 27.1, *) {
            Reader(content: content)
        } else {
            content(nil)
        }
    }

    @available(iOS 27.1, *)
    private struct Reader: View {
        @Environment(\.toolbarVerticalEdge) private var edge
        @ViewBuilder var content: (HorizontalEdge?) -> Content

        var body: some View { content(edge) }
    }
}

// MARK: - Settings

private struct SettingsSheet: View {
    @ObservedObject var vm: VMViewModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Keyboard for Text Fields", isOn: $vm.autoKeyboard)
                } footer: {
                    Text(vm.textFocusAgentPresent
                         ? "In full screen, the keyboard comes up when you tap a text field in Windows and goes away when you tap elsewhere."
                         : "In full screen, the keyboard comes up when you tap a text field in Windows. This needs the latest WindowsiOS Tools installed in Windows.")
                }
                Section {
                    Toggle("Sign In with Safari", isOn: Binding(get: { vm.useSafariForOAuth }, set: { on in
                        if on != vm.useSafariForOAuth { vm.toggleOAuthBrowserMode() }
                    }))
                } footer: {
                    Text("Sign-in pages that Windows apps open come up in Safari here, with your passkeys and saved passwords. When this is off they open in Edge inside Windows.")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .onAppear { vm.refreshTextFocusAgent() }
    }
}

/// What the window's geometry resolved to: the picture's margins and canvas for the layout that
/// is showing (the lid in laptop layout), and the deck's margins.
private struct SessionLayout {
    let corners: DisplayCorners
    let cornersFromSystem: Bool
    let cameras: [CGRect]
    let camerasActive: [Bool]
    /// The part of the window the picture occupies: the window, or the lid.
    let area: CGRect
    let margins: PictureInsets
    let picture: GuestPicture?
    let deckArea: CGRect
    let deckMargins: PictureInsets

    /// Lines for Documents/boot_status.txt, so the real numbers can be read from the phone.
    func report(window: CGSize, scale: CGFloat) -> String {
        func f(_ v: CGFloat) -> String { String(format: "%.2f", v) }
        func edges(_ m: PictureInsets) -> String { "top=\(f(m.top)) left=\(f(m.left)) bottom=\(f(m.bottom)) right=\(f(m.right))" }
        func rect(_ r: CGRect) -> String { "\(f(r.minX)),\(f(r.minY)) \(f(r.width))x\(f(r.height))" }
        let cameraText = cameras.isEmpty ? "none"
            : zip(cameras, camerasActive).map { "\(rect($0)) active=\($1)" }.joined(separator: "; ")
        let canvas = picture.map { rect($0.frame) } ?? "none"
        let guest = picture.map { "\($0.size.width)x\($0.size.height)" } ?? "none"
        return """
        window_pts: \(f(window.width))x\(f(window.height))
        display_scale: \(f(scale))
        corner_radii_pts: tl=\(f(corners.topLeading)) tr=\(f(corners.topTrailing)) bl=\(f(corners.bottomLeading)) br=\(f(corners.bottomTrailing)) source=\(cornersFromSystem ? "system" : "fallback")
        camera_frames_pts: \(cameraText)
        picture_area_pts: \(rect(area))
        picture_margins_pts: \(edges(margins))
        canvas_pts: \(canvas)
        guest_target: \(guest)
        deck_area_pts: \(rect(deckArea))
        deck_margins_pts: \(edges(deckMargins))

        """
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
                Text(vm.stikDebugStatus ?? vm.vmStatusText)
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }

            if vm.isUnpacking {
                ProgressView(value: vm.unpackProgress)
                    .frame(maxWidth: 260)
            }

            VStack(alignment: .leading, spacing: 8) {
                Label(vm.isJITLive ? "JIT enabled" : vm.jitStatusText,
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
