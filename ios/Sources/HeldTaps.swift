// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Foundation

/// The first fingers of a touch gesture, held back from Windows for a moment. Windows has no
/// two-finger tap on a touchscreen, so a quick two-finger tap becomes a right click and a quick
/// three-finger tap opens the menu; anything longer, or that moves, reaches Windows as touches.
struct HeldTaps {
    /// How long a lone first finger waits for a second one before it goes to Windows.
    static let holdWindow: TimeInterval = 0.08
    /// The longest a two- or three-finger tap may last.
    static let multiTapWindow: TimeInterval = 0.3
    /// Movement, in points, that makes the fingers a drag rather than a tap.
    static let slop: CGFloat = 12
    static let maxFingers = 3

    struct Finger: Equatable {
        let id: ObjectIdentifier
        let start: CGPoint
        var lifted = false
    }

    enum Outcome: Equatable {
        case tap(CGPoint)
        case rightClick(CGPoint)
        case threeFingerTap
        case nothing
    }

    private(set) var fingers: [Finger] = []
    private(set) var since: TimeInterval = 0
    private var cancelled = false

    var isEmpty: Bool { fingers.isEmpty }

    /// Fingers land. Returns when the held fingers go to Windows if they have not all lifted by
    /// then, or nil when they must go now (more fingers than any tap uses).
    mutating func land(_ landed: [(id: ObjectIdentifier, at: CGPoint)], time: TimeInterval) -> TimeInterval? {
        if fingers.isEmpty {
            since = time
            cancelled = false
        }
        fingers += landed.map { Finger(id: $0.id, start: $0.at) }
        guard fingers.count <= Self.maxFingers else { return nil }
        return since + (fingers.count == 1 ? Self.holdWindow : Self.multiTapWindow)
    }

    /// Whether a finger still down has moved far enough to be a drag.
    func movedTooFar(_ location: (ObjectIdentifier) -> CGPoint?) -> Bool {
        fingers.contains { finger in
            guard !finger.lifted, let p = location(finger.id) else { return false }
            return hypot(p.x - finger.start.x, p.y - finger.start.y) > Self.slop
        }
    }

    /// Fingers lift. Once every held finger is up the gesture was a tap: returns what it means
    /// and clears. A cancelled touch (a system interruption) turns the tap into nothing.
    mutating func lift(_ ids: Set<ObjectIdentifier>, cancelled: Bool) -> Outcome? {
        guard !fingers.isEmpty else { return nil }
        for i in fingers.indices where ids.contains(fingers[i].id) {
            fingers[i].lifted = true
        }
        self.cancelled = self.cancelled || cancelled
        guard fingers.allSatisfy(\.lifted) else { return nil }
        defer { fingers.removeAll() }
        if self.cancelled { return .nothing }
        switch fingers.count {
        case 1: return .tap(fingers[0].start)
        case 2: return .rightClick(fingers[0].start)
        case 3: return .threeFingerTap
        default: return .nothing
        }
    }

    /// The fingers are a real touch gesture after all: hands back the ones still down and clears.
    mutating func release() -> [Finger] {
        defer { fingers.removeAll() }
        return fingers.filter { !$0.lifted }
    }
}
