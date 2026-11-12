// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

// The Glaus FAB and icon menu, rebuilt for WindowsiOS: a glass button that springs a column of
// icon + label capsules. Spawn is Glaus build 182's: every capsule at once, rising out of its own
// frame with a 0.8 scale from the button's corner, on a 0.35 / 0.8 spring.

enum FABMetrics {
    static let diameter: CGFloat = 62
    static let iconSize: CGFloat = 26
    static let edgeInset: CGFloat = 20
    static let endInset: CGFloat = 24
    static let spacing: CGFloat = 14
}

extension Animation {
    /// Glaus canonBounce.
    static let fabBounce = Animation.spring(response: 0.35, dampingFraction: 0.8)
}

/// Which corner of its region the FAB hugs. Dragging the button snaps it to the nearest corner,
/// so it can be moved off whatever Windows draws there (tray, title-bar buttons).
enum FABCorner: String, CaseIterable {
    case topLeading, topTrailing, bottomLeading, bottomTrailing

    var isTop: Bool { self == .topLeading || self == .topTrailing }
    var isTrailing: Bool { self == .topTrailing || self == .bottomTrailing }

    var alignment: Alignment {
        switch self {
        case .topLeading: return .topLeading
        case .topTrailing: return .topTrailing
        case .bottomLeading: return .bottomLeading
        case .bottomTrailing: return .bottomTrailing
        }
    }

    var unitPoint: UnitPoint {
        switch self {
        case .topLeading: return .topLeading
        case .topTrailing: return .topTrailing
        case .bottomLeading: return .bottomLeading
        case .bottomTrailing: return .bottomTrailing
        }
    }

    static func nearest(to point: CGPoint, in region: CGRect) -> FABCorner {
        switch (point.y < region.midY, point.x > region.midX) {
        case (true, false): return .topLeading
        case (true, true): return .topTrailing
        case (false, false): return .bottomLeading
        case (false, true): return .bottomTrailing
        }
    }

    /// The button's centre when it sits in this corner of `region`.
    func buttonCenter(in region: CGRect) -> CGPoint {
        let r = FABMetrics.diameter / 2
        let x = isTrailing ? region.maxX - FABMetrics.edgeInset - r : region.minX + FABMetrics.edgeInset + r
        let y = isTop ? region.minY + FABMetrics.endInset + r : region.maxY - FABMetrics.endInset - r
        return CGPoint(x: x, y: y)
    }
}

/// Closes the open FAB menu; items call it before running their action. Every instance collapses
/// the one menu above it, so instances compare equal unless the animation they would use differs.
struct FABCloseAction: Equatable {
    let reduceMotion: Bool
    let close: () -> Void
    func callAsFunction() { close() }
    static func == (lhs: FABCloseAction, rhs: FABCloseAction) -> Bool { lhs.reduceMotion == rhs.reduceMotion }
}

extension EnvironmentValues {
    @Entry var fabClose: FABCloseAction? = nil
}

struct FABMenuItem: View {
    @Environment(\.fabClose) private var fabClose
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let title: String
    let icon: String
    var isOn: Bool = false
    var role: ButtonRole? = nil
    let action: () -> Void

    var body: some View {
        Button(role: role) {
            fabClose?.callAsFunction()
            action()
        } label: {
            HStack(spacing: 10) {
                Image(systemName: icon)
                    .font(.subheadline.weight(.semibold))
                    .symbolVariant(isOn ? .fill : .none)
                    .frame(width: 20)
                Text(title)
                    .font(.subheadline.weight(.bold))
            }
            .foregroundStyle(role == .destructive ? AnyShapeStyle(.red) : AnyShapeStyle(.primary))
            .padding(.horizontal, 18)
            .padding(.vertical, 13)
            .glassEffect(isOn ? .regular.tint(.accentColor.opacity(0.45)).interactive() : .regular.interactive(), in: .capsule)
            .glassEffectTransition(reduceMotion ? .identity : .materialize)
            .overlay(Capsule().strokeBorder(.white.opacity(0.16), lineWidth: 1))
            .shadow(color: .black.opacity(0.15), radius: 10, y: 5)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(isOn ? .isSelected : [])
    }
}

struct FloatingActionMenu<Items: View>: View {
    @Binding var isExpanded: Bool
    @Binding var corner: FABCorner
    /// Where the button may sit, in this view's coordinate space (kept off the fold and out of
    /// the safe-area insets by the caller).
    let region: CGRect
    var icon: String = "plus"
    var accessibilityLabel: String = "Windows controls"
    @ViewBuilder var items: () -> Items

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @ScaledMetric(relativeTo: .subheadline) private var capsuleLine: CGFloat = 17
    /// Resets on its own if the system cancels the drag, so the button never strands mid-screen.
    @GestureState(resetTransaction: Transaction(animation: .fabBounce)) private var dragOffset: CGSize = .zero

    var body: some View {
        ZStack {
            if isExpanded {
                Color.black.opacity(0.44)
                    .ignoresSafeArea()
                    .contentShape(Rectangle())
                    .onTapGesture { setExpanded(false) }
                    .accessibilityLabel("Close menu")
                    .accessibilityAddTraits(.isButton)
                    .transition(.opacity)
            }

            menu
                .padding(corner.isTrailing ? .trailing : .leading, FABMetrics.edgeInset)
                .padding(corner.isTop ? .top : .bottom, FABMetrics.endInset)
                .frame(width: max(0, region.width), height: max(0, region.height), alignment: corner.alignment)
                .position(x: region.midX, y: region.midY)
        }
        .environment(\.fabClose, FABCloseAction(reduceMotion: reduceMotion) { setExpanded(false) })
    }

    private var menuAlignment: HorizontalAlignment { corner.isTrailing ? .trailing : .leading }

    /// The button is unconditional so it keeps its identity when a drag moves it between top and
    /// bottom corners; the capsules sit above it in a bottom corner and below it in a top corner.
    private var menu: some View {
        Group(subviews: items()) { subviews in
            VStack(alignment: menuAlignment, spacing: 0) {
                if !corner.isTop { capsuleStack(subviews) }
                button
                if corner.isTop { capsuleStack(subviews) }
            }
        }
    }

    /// As in Glaus, the capsules' glass container stays mounted while the menu is closed, so the
    /// capsules materialize into it instead of arriving with a new container. A stack too tall for
    /// the region (outer display, landscape) scrolls in a band instead.
    @ViewBuilder
    private func capsuleStack(_ subviews: SubviewsCollection) -> some View {
        let useBand = isExpanded && !stackFits(subviews.count)
        let gapEdge: Edge.Set = corner.isTop ? .top : .bottom
        if useBand {
            ScrollView(.vertical) {
                GlassEffectContainer(spacing: FABMetrics.spacing) {
                    VStack(alignment: menuAlignment, spacing: FABMetrics.spacing) {
                        capsules(subviews)
                    }
                    .padding(.vertical, 4)
                }
            }
            .scrollIndicators(.hidden)
            .scrollBounceBehavior(.basedOnSize)
            .defaultScrollAnchor(corner.isTop ? .top : .bottom)
            .frame(height: bandHeight)
            .fixedSize(horizontal: true, vertical: false)
            .padding(gapEdge, FABMetrics.spacing)
        }
        GlassEffectContainer(spacing: FABMetrics.spacing) {
            VStack(alignment: menuAlignment, spacing: FABMetrics.spacing) {
                if isExpanded && !useBand {
                    capsules(subviews)
                }
            }
            .padding(gapEdge, isExpanded && !useBand ? FABMetrics.spacing : 0)
        }
    }

    /// Items are listed for a bottom corner (the last one next to the button); a top corner,
    /// whose capsules hang below the button, mirrors the order.
    @ViewBuilder
    private func capsules(_ subviews: SubviewsCollection) -> some View {
        if corner.isTop {
            ForEach(subviews.reversed()) { subview in
                subview.transition(capsuleTransition)
            }
        } else {
            ForEach(subviews) { subview in
                subview.transition(capsuleTransition)
            }
        }
    }

    private var capsuleTransition: AnyTransition {
        let insertion = AnyTransition.move(edge: corner.isTop ? .top : .bottom)
            .combined(with: .opacity)
            .combined(with: .scale(scale: 0.8, anchor: corner.unitPoint))
        let removal = AnyTransition.opacity.combined(with: .scale(scale: 0.8, anchor: corner.unitPoint))
        return .asymmetric(insertion: insertion, removal: removal)
    }

    private var heightBudget: CGFloat {
        max(0, region.height - FABMetrics.diameter - FABMetrics.spacing - FABMetrics.endInset - 8)
    }

    private func stackFits(_ count: Int) -> Bool {
        guard count > 0 else { return true }
        let stack = CGFloat(count) * (capsuleLine + 26 + FABMetrics.spacing)
        return stack + 24 <= heightBudget
    }

    private var bandHeight: CGFloat {
        min(verticalSizeClass == .compact ? 230 : 460, heightBudget)
    }

    private var button: some View {
        GlassEffectContainer(spacing: FABMetrics.spacing) {
            Button {
                setExpanded(!isExpanded)
            } label: {
                Image(systemName: icon)
                    .font(.system(size: FABMetrics.iconSize, weight: .bold))
                    .foregroundStyle(.primary)
                    .rotationEffect(.degrees(isExpanded ? 45 : 0))
                    .frame(width: FABMetrics.diameter, height: FABMetrics.diameter)
                    .contentShape(Circle())
            }
            .buttonStyle(.plain)
            .glassEffect(.regular.interactive(), in: .circle)
            .overlay(Circle().strokeBorder(.white.opacity(0.16), lineWidth: 1))
            .shadow(color: .black.opacity(0.15), radius: 10, y: 5)
            .accessibilityLabel(accessibilityLabel)
            .accessibilityHint("Drag to move it to another corner")
            .accessibilityAddTraits(isExpanded ? .isSelected : [])
        }
        .offset(dragOffset)
        .highPriorityGesture(
            DragGesture(minimumDistance: 12)
                .updating($dragOffset) { value, state, _ in
                    state = value.translation
                }
                .onChanged { _ in
                    if isExpanded { setExpanded(false) }
                }
                .onEnded { value in
                    let start = corner.buttonCenter(in: region)
                    let landing = CGPoint(x: start.x + value.predictedEndTranslation.width,
                                          y: start.y + value.predictedEndTranslation.height)
                    withAnimation(.fabBounce) {
                        corner = FABCorner.nearest(to: landing, in: region)
                    }
                }
        )
        .sensoryFeedback(.impact(flexibility: .soft, intensity: 0.6), trigger: isExpanded)
    }

    private func setExpanded(_ expanded: Bool) {
        withAnimation(reduceMotion ? nil : .fabBounce) {
            isExpanded = expanded
        }
    }
}
