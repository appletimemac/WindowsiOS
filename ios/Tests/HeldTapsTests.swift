// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Testing

struct HeldTapsTests {
    private final class Token {}
    private let a = Token(), b = Token(), c = Token(), d = Token()
    private var ia: ObjectIdentifier { ObjectIdentifier(a) }
    private var ib: ObjectIdentifier { ObjectIdentifier(b) }
    private var ic: ObjectIdentifier { ObjectIdentifier(c) }
    private var id: ObjectIdentifier { ObjectIdentifier(d) }

    @Test func twoFingerTapIsARightClickAtTheFirstFinger() {
        var held = HeldTaps()
        _ = held.land([(ia, CGPoint(x: 100, y: 200))], time: 10)
        _ = held.land([(ib, CGPoint(x: 160, y: 210))], time: 10.03)
        #expect(held.lift([ia], cancelled: false) == nil)
        #expect(held.lift([ib], cancelled: false) == .rightClick(CGPoint(x: 100, y: 200)))
        #expect(held.isEmpty)
    }

    @Test func fingersLandingAndLiftingTogetherStillRightClick() {
        var held = HeldTaps()
        _ = held.land([(ia, CGPoint(x: 5, y: 5)), (ib, CGPoint(x: 40, y: 5))], time: 1)
        #expect(held.lift([ia, ib], cancelled: false) == .rightClick(CGPoint(x: 5, y: 5)))
    }

    @Test func quickSingleTapStaysATapForWindows() {
        var held = HeldTaps()
        _ = held.land([(ia, CGPoint(x: 30, y: 40))], time: 2)
        #expect(held.lift([ia], cancelled: false) == .tap(CGPoint(x: 30, y: 40)))
    }

    @Test func threeFingerTapOpensTheMenu() {
        var held = HeldTaps()
        _ = held.land([(ia, .zero), (ib, CGPoint(x: 20, y: 0)), (ic, CGPoint(x: 40, y: 0))], time: 3)
        #expect(held.lift([ia, ib, ic], cancelled: false) == .threeFingerTap)
    }

    @Test func aLoneFingerWaitsBrieflyAndASecondExtendsTheWindow() throws {
        var held = HeldTaps()
        let firstDeadline = held.land([(ia, .zero)], time: 5)
        let first = try #require(firstDeadline)
        #expect(abs(first - (5 + HeldTaps.holdWindow)) < 1e-9)
        let secondDeadline = held.land([(ib, CGPoint(x: 30, y: 0))], time: 5.05)
        let second = try #require(secondDeadline)
        #expect(abs(second - (5 + HeldTaps.multiTapWindow)) < 1e-9)
    }

    @Test func fourFingersGoToWindowsAtOnce() {
        var held = HeldTaps()
        _ = held.land([(ia, .zero), (ib, .zero), (ic, .zero)], time: 0)
        #expect(held.land([(id, .zero)], time: 0.01) == nil)
        #expect(held.release().count == 4)
        #expect(held.isEmpty)
    }

    @Test func movementBeyondTheSlopIsADrag() {
        var held = HeldTaps()
        _ = held.land([(ia, CGPoint(x: 100, y: 100)), (ib, CGPoint(x: 150, y: 100))], time: 0)
        #expect(!held.movedTooFar { $0 == ia ? CGPoint(x: 108, y: 104) : CGPoint(x: 150, y: 100) })
        #expect(held.movedTooFar { $0 == ib ? CGPoint(x: 150, y: 130) : CGPoint(x: 100, y: 100) })
    }

    @Test func releaseHandsBackOnlyFingersStillDown() {
        var held = HeldTaps()
        _ = held.land([(ia, CGPoint(x: 1, y: 1)), (ib, CGPoint(x: 2, y: 2))], time: 0)
        #expect(held.lift([ia], cancelled: false) == nil)
        let down = held.release()
        #expect(down.map(\.id) == [ib])
        #expect(down.first?.start == CGPoint(x: 2, y: 2))
        #expect(held.isEmpty)
    }

    @Test func aCancelledTouchTurnsTheTapIntoNothing() {
        var held = HeldTaps()
        _ = held.land([(ia, .zero), (ib, .zero)], time: 0)
        #expect(held.lift([ia], cancelled: true) == nil)
        #expect(held.lift([ib], cancelled: false) == .nothing)
    }

    @Test func aNewGestureStartsClean() {
        var held = HeldTaps()
        _ = held.land([(ia, .zero)], time: 0)
        _ = held.lift([ia], cancelled: true)
        _ = held.land([(ib, CGPoint(x: 9, y: 9))], time: 1)
        #expect(held.since == 1)
        #expect(held.lift([ib], cancelled: false) == .tap(CGPoint(x: 9, y: 9)))
    }
}
