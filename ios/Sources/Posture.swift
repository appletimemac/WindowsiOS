// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics

enum PostureMode: String, CaseIterable {
    case auto = "Auto"
    case laptop = "Laptop"
    case fullScreen = "Studio"

    var iconName: String {
        switch self {
        case .auto: return "a.circle"
        case .laptop: return "laptopcomputer"
        case .fullScreen: return "ipad.landscape"
        }
    }
}

/// Raw values match UIHingeStatus (UIHinge.h, iOS 27.1).
enum HingeStatus: Int {
    case unknown = 0
    case closed = 1
    case partiallyOpen = 2
    case fullyOpen = 3
}

enum FoldAxis: Equatable {
    /// The crease runs left to right: inner display in portrait, held like a laptop.
    case horizontal
    /// The crease runs top to bottom: inner display in landscape, held like a book.
    case vertical
}

/// The part of the session window the hinge claims, in the window's coordinate space.
struct FoldGeometry: Equatable {
    let axis: FoldAxis
    /// Keep-clear rect, margins included. Zero thickness when synthesized from the hinge alone.
    let frame: CGRect

    init(axis: FoldAxis, frame: CGRect) {
        self.axis = axis
        self.frame = frame
    }

    /// Builds a fold from an active `.division` reserved region. The headers do not promise the
    /// region spans the whole window, so the axis comes from the region's own shape.
    init(divisionFrame: CGRect) {
        self.init(axis: divisionFrame.width >= divisionFrame.height ? .horizontal : .vertical, frame: divisionFrame)
    }
}

enum PostureResolver {
    /// The active fold for this window. Prefers the system's division region; if it is missing
    /// while the hinge reports partially open, falls back to a zero-width crease on the midline
    /// (the inner display folds across its long side, so a portrait window means a horizontal crease).
    static func fold(divisionFrames: [CGRect], hinge: HingeStatus, windowSize: CGSize) -> FoldGeometry? {
        if let frame = divisionFrames.first(where: { $0.width > 0 || $0.height > 0 }) {
            return FoldGeometry(divisionFrame: frame)
        }
        guard hinge == .partiallyOpen, windowSize.width > 0, windowSize.height > 0 else { return nil }
        if windowSize.height > windowSize.width {
            return FoldGeometry(axis: .horizontal, frame: CGRect(x: 0, y: windowSize.height / 2, width: windowSize.width, height: 0))
        }
        return FoldGeometry(axis: .vertical, frame: CGRect(x: windowSize.width / 2, y: 0, width: 0, height: windowSize.height))
    }

    /// Auto shows the laptop deck only when the crease is horizontal; a book-style vertical
    /// crease keeps the full Studio canvas.
    static func isLaptop(mode: PostureMode, fold: FoldGeometry?) -> Bool {
        switch mode {
        case .laptop: return true
        case .fullScreen: return false
        case .auto: return fold?.axis == .horizontal
        }
    }

    /// A manual Laptop or Studio choice lasts until the next fold or unfold. Transitions to or
    /// from an unknown status (no reading yet, or the interaction left the hierarchy) do not count.
    static func mode(_ mode: PostureMode, afterHingeChangeFrom old: HingeStatus, to new: HingeStatus) -> PostureMode {
        guard old != new, old != .unknown, new != .unknown else { return mode }
        return .auto
    }

    /// Lid, crease and deck heights for the clamshell layout. Without a horizontal fold the lid
    /// takes the top half, as upstream did.
    static func laptopSplit(height: CGFloat, fold: FoldGeometry?, minimumDeck: CGFloat = 180) -> (lid: CGFloat, crease: CGFloat, deck: CGFloat) {
        guard let fold, fold.axis == .horizontal, fold.frame.minY > 0, fold.frame.maxY < height else {
            let lid = (height * 0.5).rounded(.down)
            return (lid, 0, max(minimumDeck, height - lid))
        }
        let lid = fold.frame.minY.rounded(.down)
        let crease = fold.frame.maxY.rounded(.up) - lid
        return (lid, crease, max(minimumDeck, height - lid - crease))
    }

    /// Where floating chrome (the boot card) goes so nothing interactive sits on the crease:
    /// the lid in laptop layout, otherwise the pane before the fold, otherwise the whole window.
    static func primaryPane(windowSize: CGSize, fold: FoldGeometry?, laptop: Bool) -> CGRect {
        let full = CGRect(origin: .zero, size: windowSize)
        if laptop {
            return CGRect(x: 0, y: 0, width: windowSize.width, height: laptopSplit(height: windowSize.height, fold: fold).lid)
        }
        guard let fold else { return full }
        switch fold.axis {
        case .vertical:
            return CGRect(x: 0, y: 0, width: max(0, fold.frame.minX), height: windowSize.height)
        case .horizontal:
            return CGRect(x: 0, y: 0, width: windowSize.width, height: max(0, fold.frame.minY))
        }
    }
}
