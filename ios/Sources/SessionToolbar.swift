// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

// The session's controls: one Liquid Glass bar that stays tucked behind a grabber, so Windows keeps
// the whole picture until the bar is pulled out. From the top it runs across (the open inner
// display); from the side the system keeps its own bars on (the outer display, the inner display in
// landscape) it runs down that edge.

/// Where the bar is pulled from, which is also the way it runs.
enum ToolbarDock: Equatable {
    case top
    case side(HorizontalEdge)

    var isVertical: Bool { self != .top }
}

enum ToolbarMetrics {
    static let grabberLength: CGFloat = 44
    static let grabberThickness: CGFloat = 6
    /// From the picture's edge to the open bar, and from the bar's ends to the picture's.
    static let barInset: CGFloat = 8
    static let grabberGap: CGFloat = 8
    static let barPadding: CGFloat = 5
    static let itemSpacing: CGFloat = 2
    /// How far the grabber answers a touch beyond its pill: to each side, and past each end.
    static let grabberReach: CGFloat = 14
    static let grabberEndReach: CGFloat = 12
    /// How far a pull on the grabber has to travel, and how little a touch may move to be a tap.
    static let pull: CGFloat = 14
    static let tapSlop: CGFloat = 10
}

extension Animation {
    /// The bar springs out; with Reduce Motion it fades, which still needs an animation to run in.
    static func toolbar(reduceMotion: Bool) -> Animation {
        reduceMotion ? .easeInOut(duration: 0.15) : .spring(response: 0.35, dampingFraction: 0.8)
    }
}

/// Tucks the toolbar away; its controls call it before they act.
struct ToolbarCloseAction {
    let close: () -> Void
    func callAsFunction() { close() }
}

extension EnvironmentValues {
    @Entry var toolbarClose: ToolbarCloseAction? = nil
}

struct SessionToolbar<Items: View>: View {
    @Binding var isOpen: Bool
    let dock: ToolbarDock
    /// The pane the bar belongs to, out to the display's edge, in this view's coordinate space.
    let outer: CGRect
    /// The part of `outer` the Windows picture covers. The open bar sits inside it; the grabber
    /// waits in the strip between the two, where nothing of Windows is drawn.
    let region: CGRect
    /// Cameras that are on, in the same space: the grabber does not wait behind one.
    var cameras: [CGRect] = []
    @ViewBuilder var items: () -> Items

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.layoutDirection) private var layoutDirection
    @Namespace private var glass

    var body: some View {
        let frame = containerFrame
        ZStack {
            if isOpen {
                Color.clear
                    .contentShape(Rectangle())
                    .onTapGesture { setOpen(false) }
                    .accessibilityElement()
                    .accessibilityLabel("Close toolbar")
                    .accessibilityAddTraits(.isButton)
            }

            GlassEffectContainer(spacing: ToolbarMetrics.grabberGap) {
                stack
            }
            .padding(dockEdge, edgePadding)
            .frame(width: frame.width, height: frame.height, alignment: dockAlignment)
            .position(x: frame.midX, y: frame.midY)
        }
        // Bars keep their size past this; the large content viewer shows a control's name instead.
        .dynamicTypeSize(...DynamicTypeSize.accessibility2)
        .environment(\.toolbarClose, ToolbarCloseAction { setOpen(false) })
        .sensoryFeedback(.impact(flexibility: .soft, intensity: 0.6), trigger: isOpen)
        .onChange(of: dock) { isOpen = false }
    }

    // MARK: Placement

    /// Across the picture and from the display's top edge for a top dock; across the pane and
    /// along the picture for a side dock, so the bar centres on what shows of Windows (the software
    /// keyboard shortens it).
    private var containerFrame: CGRect {
        switch dock {
        case .top:
            return CGRect(x: region.minX, y: outer.minY, width: region.width, height: max(0, region.maxY - outer.minY))
        case .side:
            return CGRect(x: outer.minX, y: region.minY, width: outer.width, height: region.height)
        }
    }

    private var isOnLeft: Bool {
        guard case .side(let edge) = dock else { return false }
        return (edge == .leading) == (layoutDirection == .leftToRight)
    }

    /// The band between the display's edge and the picture on the docked side.
    private var strip: CGFloat {
        switch dock {
        case .top: return max(0, region.minY - outer.minY)
        case .side: return max(0, isOnLeft ? region.minX - outer.minX : outer.maxX - region.maxX)
        }
    }

    private var edgePadding: CGFloat {
        if isOpen { return strip + ToolbarMetrics.barInset }
        let centred = max(3, (strip - ToolbarMetrics.grabberThickness) / 2)
        // A camera in the strip (an iPhone's, across the top) would hide the grabber: it waits just
        // inside the picture there.
        let pill = grabberMeetsCamera(at: centred) ? strip + 4 : centred
        // The grabber's frame reaches past its pill, so it starts that much nearer the edge.
        return pill - ToolbarMetrics.grabberReach
    }

    private func grabberMeetsCamera(at padding: CGFloat) -> Bool {
        guard !cameras.isEmpty else { return false }
        let frame = containerFrame
        let length = ToolbarMetrics.grabberLength, thickness = ToolbarMetrics.grabberThickness
        let pill: CGRect
        switch dock {
        case .top:
            pill = CGRect(x: frame.midX - length / 2, y: frame.minY + padding, width: length, height: thickness)
        case .side:
            let x = isOnLeft ? frame.minX + padding : frame.maxX - padding - thickness
            pill = CGRect(x: x, y: frame.midY - length / 2, width: thickness, height: length)
        }
        let reach = pill.insetBy(dx: -4, dy: -4)
        return cameras.contains { $0.intersects(reach) }
    }

    private var dockEdge: Edge.Set {
        switch dock {
        case .top: return .top
        case .side(let edge): return edge == .leading ? .leading : .trailing
        }
    }

    private var dockAlignment: Alignment {
        switch dock {
        case .top: return .top
        case .side(let edge): return edge == .leading ? .leading : .trailing
        }
    }

    /// The grabber is the bar's inner end, like a drawer's handle: pulling it in brings the bar out
    /// between it and the edge.
    @ViewBuilder
    private var stack: some View {
        switch dock {
        case .top:
            VStack(spacing: ToolbarMetrics.grabberGap - ToolbarMetrics.grabberReach) {
                if isOpen { bar }
                grabber
            }
        case .side(let edge):
            HStack(spacing: ToolbarMetrics.grabberGap - ToolbarMetrics.grabberReach) {
                if isOpen, edge == .leading { bar }
                grabber
                if isOpen, edge == .trailing { bar }
            }
        }
    }

    // MARK: Bar

    private var bar: some View {
        let layout = dock.isVertical
            ? AnyLayout(VStackLayout(spacing: ToolbarMetrics.itemSpacing))
            : AnyLayout(HStackLayout(spacing: ToolbarMetrics.itemSpacing))
        let axis: Axis.Set = dock.isVertical ? .vertical : .horizontal
        let room = max(0, (dock.isVertical ? region.height : region.width) - 2 * ToolbarMetrics.barInset)
        return ViewThatFits(in: axis) {
            layout { items() }
            ScrollView(axis) { layout { items() } }
                .scrollIndicators(.hidden)
                .scrollBounceBehavior(.basedOnSize)
        }
        .padding(ToolbarMetrics.barPadding)
        .clipShape(.capsule)
        .glassEffect(.regular, in: .capsule)
        .glassEffectID("bar", in: glass)
        .frame(maxWidth: dock.isVertical ? nil : room, maxHeight: dock.isVertical ? room : nil)
        .transition(barTransition)
    }

    private var barTransition: AnyTransition {
        guard !reduceMotion else { return .opacity }
        let anchor: UnitPoint
        switch dock {
        case .top: anchor = .top
        case .side(let edge): anchor = edge == .leading ? .leading : .trailing
        }
        return .opacity.combined(with: .scale(scale: 0.8, anchor: anchor))
    }

    // MARK: Grabber

    private var grabber: some View {
        let length = ToolbarMetrics.grabberLength, thickness = ToolbarMetrics.grabberThickness
        let across = thickness + 2 * ToolbarMetrics.grabberReach, along = length + 2 * ToolbarMetrics.grabberEndReach
        return Capsule()
            .fill(.white.opacity(0.55))
            .frame(width: dock.isVertical ? thickness : length, height: dock.isVertical ? length : thickness)
            .glassEffect(.regular.interactive(), in: .capsule)
            .glassEffectID("grabber", in: glass)
            // What a thumb has to find: more than the pill, and part of the layout, so nothing clips it.
            .frame(width: dock.isVertical ? across : along, height: dock.isVertical ? along : across)
            .contentShape(Rectangle())
            .gesture(grabberGesture)
            .accessibilityElement()
            .accessibilityLabel("Windows controls")
            .accessibilityValue(isOpen ? "Open" : "Tucked away")
            .accessibilityHint(isOpen ? "Tucks the toolbar away" : "Brings out the toolbar")
            .accessibilityAddTraits(.isButton)
            .accessibilityAction { setOpen(!isOpen) }
    }

    /// One gesture for both, so a short pull is never also a tap: a touch that stays put toggles the
    /// bar, a pull brings it out and a push sends it back.
    private var grabberGesture: some Gesture {
        DragGesture(minimumDistance: 0)
            .onEnded { value in
                let inward = inwardDistance(value.translation)
                if hypot(value.translation.width, value.translation.height) < ToolbarMetrics.tapSlop {
                    setOpen(!isOpen)
                } else if inward > ToolbarMetrics.pull {
                    setOpen(true)
                } else if inward < -ToolbarMetrics.pull {
                    setOpen(false)
                }
            }
    }

    private func inwardDistance(_ translation: CGSize) -> CGFloat {
        switch dock {
        case .top: return translation.height
        case .side: return isOnLeft ? translation.width : -translation.width
        }
    }

    private func setOpen(_ open: Bool) {
        withAnimation(.toolbar(reduceMotion: reduceMotion)) {
            isOpen = open
        }
    }
}

// MARK: - Controls

/// A control's symbol in the bar's slot, with its name for VoiceOver and the large content viewer.
struct ToolbarItemLabel<Icon: View>: View {
    let title: String
    var isOn = false
    @ViewBuilder var icon: () -> Icon

    @ScaledMetric(relativeTo: .title3) private var scaledSide: CGFloat = 44
    /// Grows with larger text, and never shrinks under the smallest target a finger should be given.
    private var side: CGFloat { max(44, scaledSide) }

    var body: some View {
        icon()
            .font(.title3.weight(.medium))
            .foregroundStyle(.primary)
            .frame(width: side, height: side)
            .background(.tint.opacity(isOn ? 0.5 : 0), in: .circle)
            .contentShape(.circle)
            .accessibilityLabel(title)
            .accessibilityShowsLargeContentViewer {
                VStack {
                    icon()
                    Text(title)
                }
            }
    }
}

/// A button on the session toolbar. It tucks the bar away, then acts.
struct ToolbarButton<Icon: View>: View {
    @Environment(\.toolbarClose) private var close

    let title: String
    var isOn = false
    let action: () -> Void
    @ViewBuilder var icon: () -> Icon

    var body: some View {
        Button {
            close?()
            action()
        } label: {
            ToolbarItemLabel(title: title, isOn: isOn, icon: icon)
        }
        .buttonStyle(ToolbarItemButtonStyle())
        .accessibilityAddTraits(isOn ? .isSelected : [])
    }
}

extension ToolbarButton where Icon == Image {
    init(title: String, systemImage: String, isOn: Bool = false, action: @escaping () -> Void) {
        self.init(title: title, isOn: isOn, action: action) {
            Image(systemName: systemImage)
        }
    }
}

private struct ToolbarItemButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .background(.white.opacity(configuration.isPressed ? 0.2 : 0), in: .circle)
            .scaleEffect(configuration.isPressed ? 0.92 : 1)
            .animation(.snappy(duration: 0.15), value: configuration.isPressed)
    }
}
