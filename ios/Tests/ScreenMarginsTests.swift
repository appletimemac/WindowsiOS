// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Testing

struct ScreenMarginsTests {
    private let innerLandscape = CGRect(x: 0, y: 0, width: 951, height: 669)
    private let innerPortrait = CGRect(x: 0, y: 0, width: 669, height: 951)
    private let outerPortrait = CGRect(x: 0, y: 0, width: 466, height: 678)
    /// n device pixels at 3x, computed the way the margins are so exact comparison holds.
    private func px(_ n: CGFloat) -> CGFloat { n / 3 }

    private func margins(_ area: CGRect, radius: CGFloat, cameras: [CGRect] = [], mode: PictureMode = .fullWindow, scale: CGFloat = 3) -> PictureInsets {
        ScreenMargins.insets(in: area, radii: DisplayCorners(all: radius), cameras: cameras, mode: mode, scale: scale)
    }

    // MARK: margins

    @Test(arguments: [PictureMode.fullWindow, .lid, .deck])
    func squareCornersNeedNoMargin(mode: PictureMode) {
        #expect(margins(innerLandscape, radius: 0, mode: mode) == PictureInsets())
    }

    @Test func radius55IsSixteenPointOneRoundedUpToThePixel() {
        let m = margins(innerLandscape, radius: 55)
        // 55 * (1 - 1/sqrt 2) = 16.109 pt = 48.33 px, so 49 px.
        #expect(m == PictureInsets(top: px(49), left: px(49), bottom: px(49), right: px(49)))
        #expect(m.top > 55 * ScreenMargins.cornerFactor)
        #expect(m.top - 55 * ScreenMargins.cornerFactor < px(1))
    }

    @Test func theMarginPutsThePictureCornerInsideTheRoundedCorner() {
        let radius: CGFloat = 55
        let m = margins(innerLandscape, radius: radius)
        // The picture's corner, measured from the circle's centre (radius, radius) in the display's corner.
        let dx = radius - m.left, dy = radius - m.top
        #expect(dx * dx + dy * dy <= radius * radius)
    }

    @Test(arguments: [2, 3] as [CGFloat])
    func marginsLandOnWholeDevicePixels(scale: CGFloat) {
        for radius: CGFloat in [20, 44, 55, 62, 77.7] {
            let m = margins(innerLandscape, radius: radius, scale: scale)
            for edge in [m.top, m.left, m.bottom, m.right] {
                #expect(abs(edge * scale - (edge * scale).rounded()) < 1e-9)
                #expect(edge >= radius * ScreenMargins.cornerFactor)
            }
        }
    }

    @Test func aCameraOnTheTopEdgeRaisesOnlyTheTop() {
        let camera = CGRect(x: 400, y: 0, width: 150, height: 40)
        let m = margins(innerLandscape, radius: 55, cameras: [camera])
        #expect(m.top == 40)
        #expect(m.left == px(49))
        #expect(m.bottom == px(49))
        #expect(m.right == px(49))
    }

    @Test func aCameraOnTheLeftEdgeRaisesOnlyTheLeft() {
        let camera = CGRect(x: 0, y: 300, width: 40, height: 80)
        let m = margins(innerLandscape, radius: 55, cameras: [camera])
        #expect(m.left == 40)
        #expect(m.top == px(49))
        #expect(m.bottom == px(49))
        #expect(m.right == px(49))
    }

    @Test func aCameraInsideTheMarginChangesNothing() {
        let camera = CGRect(x: 400, y: 0, width: 150, height: 12)
        #expect(margins(innerLandscape, radius: 55, cameras: [camera]) == margins(innerLandscape, radius: 55))
    }

    @Test func aCameraBelowTheLidDoesNotMoveTheLid() {
        let lid = CGRect(x: 0, y: 0, width: 669, height: 463)
        let camera = CGRect(x: 250, y: 900, width: 150, height: 40)
        #expect(margins(lid, radius: 55, cameras: [camera], mode: .lid) == margins(lid, radius: 55, mode: .lid))
    }

    @Test func theLidMeetsOnlyTheTopCornersAndTheDeckOnlyTheBottomOnes() {
        let radii = DisplayCorners(topLeading: 55, topTrailing: 50, bottomLeading: 20, bottomTrailing: 18)
        let lid = ScreenMargins.insets(in: CGRect(x: 0, y: 0, width: 669, height: 463), radii: radii, cameras: [], mode: .lid, scale: 3)
        // The top takes the larger corner's share (16.11 pt, 49 px); the 50 pt corner then needs only 13.24 pt.
        #expect(lid == PictureInsets(top: px(49), left: px(49), bottom: 0, right: px(40)))
        // 20 * 0.2929 = 5.86 pt = 17.6 px, so 18 px; the 18 pt corner then needs 4.71 pt, 15 px.
        let deck = ScreenMargins.insets(in: CGRect(x: 0, y: 488, width: 669, height: 463), radii: radii, cameras: [], mode: .deck, scale: 3)
        #expect(deck == PictureInsets(top: 0, left: px(18), bottom: px(18), right: px(15)))
    }

    @Test func eachEdgeTakesOnlyWhatItsOwnCornersNeed() {
        let radii = DisplayCorners(topLeading: 10, topTrailing: 55, bottomLeading: 30, bottomTrailing: 20)
        let m = ScreenMargins.insets(in: innerLandscape, radii: radii, cameras: [], mode: .fullWindow, scale: 3)
        // Top from the 55 pt corner (16.11 pt), bottom and left from the 30 pt one (8.79 pt, 27 px).
        #expect(m == PictureInsets(top: px(49), left: px(27), bottom: px(27), right: px(49)))
    }

    @Test func aNearlySquareHingeSideKeepsItsEdge() {
        // The outer display in the simulator: 8 pt corners on the hinge side, 59 pt on the other.
        let radii = DisplayCorners(topLeading: 8, topTrailing: 59, bottomLeading: 8, bottomTrailing: 59)
        let m = ScreenMargins.insets(in: CGRect(x: 0, y: 0, width: 466, height: 678), radii: radii, cameras: [], mode: .fullWindow, scale: 3)
        #expect(m == PictureInsets(top: px(52), left: 0, bottom: px(52), right: px(52)))
    }

    @Test(arguments: [[55, 55, 55, 55], [8, 59, 8, 59], [10, 55, 30, 20], [0, 40, 0, 0], [62, 12, 44, 30]] as [[CGFloat]])
    func everyCornerOfThePictureIsInsideItsRoundedCorner(r: [CGFloat]) {
        let radii = DisplayCorners(topLeading: r[0], topTrailing: r[1], bottomLeading: r[2], bottomTrailing: r[3])
        for area in [innerLandscape, CGRect(x: 0, y: 0, width: 466, height: 678)] {
            let m = ScreenMargins.cornerInsets(radii, in: area)
            for (radius, a, b) in [(r[0], m.left, m.top), (r[1], m.right, m.top), (r[2], m.left, m.bottom), (r[3], m.right, m.bottom)] {
                let dx = max(0, radius - a), dy = max(0, radius - b)
                #expect(a >= radius || b >= radius || dx * dx + dy * dy <= radius * radius + 1e-6)
            }
        }
    }

    @Test func anEdgeComesBackDownToWhatTheOtherLeavesIt() {
        let radii = DisplayCorners(topLeading: 62, topTrailing: 12, bottomLeading: 44, bottomTrailing: 20)
        let m = ScreenMargins.insets(in: innerLandscape, radii: radii, cameras: [], mode: .fullWindow, scale: 3)
        // The left takes the 62 pt corner's share (18.16 pt), the bottom then needs 8.39 pt for the 44 pt
        // corner, and with that the 20 pt corner needs only 3.72 pt of the right edge, not its own share (5.86).
        #expect(m == PictureInsets(top: px(55), left: px(55), bottom: px(26), right: px(12)))
    }

    @Test func theOrderOfTheCamerasDoesNotMatter() {
        // Raising the left edge clears both; clearing the first by the top would only add to that.
        let c1 = CGRect(x: 0, y: 0, width: 40, height: 25), c2 = CGRect(x: 0, y: 300, width: 40, height: 50)
        let want = PictureInsets(top: 0, left: 40, bottom: 0, right: 0)
        #expect(margins(innerLandscape, radius: 0, cameras: [c1, c2]) == want)
        #expect(margins(innerLandscape, radius: 0, cameras: [c2, c1]) == want)
    }

    @Test func anEdgeRaisedForACameraThinsTheMarginsBesideIt() {
        // A top edge 55 pt down is already past both 55 pt top corners, so the lid's sides need nothing.
        let lid = CGRect(x: 0, y: 0, width: 669, height: 463)
        let camera = CGRect(x: 250, y: 0, width: 150, height: 55)
        #expect(margins(lid, radius: 55, cameras: [camera], mode: .lid) == PictureInsets(top: 55, left: 0, bottom: 0, right: 0))
    }

    @Test(arguments: [
        [CGRect(x: 400, y: 0, width: 150, height: 37)],
        [CGRect(x: 0, y: 300, width: 40, height: 80), CGRect(x: 420, y: 0, width: 110, height: 30)],
        [CGRect(x: 900, y: 600, width: 51, height: 69), CGRect(x: 10, y: 10, width: 30, height: 30), CGRect(x: 460, y: 640, width: 60, height: 29)],
    ])
    func noCameraIsCoveredAndEveryCornerIsClear(cameras: [CGRect]) {
        for radii in [DisplayCorners(all: 55), DisplayCorners(topLeading: 8, topTrailing: 59, bottomLeading: 8, bottomTrailing: 59)] {
            let m = ScreenMargins.insets(in: innerLandscape, radii: radii, cameras: cameras, mode: .fullWindow, scale: 3)
            #expect(m == ScreenMargins.insets(in: innerLandscape, radii: radii, cameras: cameras.reversed(), mode: .fullWindow, scale: 3))
            let picture = m.inset(innerLandscape)
            #expect(picture.width > 600 && picture.height > 400)
            for camera in cameras {
                let hit = picture.intersection(camera)
                #expect(hit.isNull || hit.width < 1e-6 || hit.height < 1e-6)
            }
            for (radius, a, b) in [(radii.topLeading, m.left, m.top), (radii.topTrailing, m.right, m.top),
                                   (radii.bottomLeading, m.left, m.bottom), (radii.bottomTrailing, m.right, m.bottom)] {
                let dx = max(0, radius - a), dy = max(0, radius - b)
                #expect(a >= radius || b >= radius || dx * dx + dy * dy <= radius * radius + 1e-6)
            }
        }
    }

    @Test func negativeRadiiAreClampedToSquare() {
        let radii = DisplayCorners(topLeading: -4, topTrailing: -1, bottomLeading: 0, bottomTrailing: -9)
        #expect(ScreenMargins.insets(in: innerLandscape, radii: radii, cameras: [], mode: .fullWindow, scale: 3) == PictureInsets())
    }

    // MARK: guest picture

    @Test func innerLandscapeAtOnePointFiveIs1376By954() throws {
        let m = margins(innerLandscape, radius: 55)
        let available = m.inset(innerLandscape)
        let picture = try #require(GuestPicture(in: available, density: 1.5, scale: 3))
        #expect(picture.size == GuestSize(width: 1376, height: 954))
        // 1376 / 1.5 by 954 / 1.5 points, centred in 918.33 x 636.33, origin on the pixel grid.
        #expect(abs(picture.frame.width - px(2752)) < 1e-9)
        #expect(abs(picture.frame.height - 636) < 1e-9)
        #expect(abs(picture.frame.minX - px(50)) < 1e-9)
        #expect(abs(picture.frame.minY - px(49)) < 1e-9)
        #expect(available.contains(picture.frame.insetBy(dx: 1e-9, dy: 1e-9)))
    }

    @Test func innerPortraitMirrorsLandscape() throws {
        let available = margins(innerPortrait, radius: 55).inset(innerPortrait)
        let picture = try #require(GuestPicture(in: available, density: 1.5, scale: 3))
        #expect(picture.size == GuestSize(width: 954, height: 1376))
    }

    @Test(arguments: [1, 1.5, 2] as [CGFloat])
    func theCanvasHoldsWholeDevicePixelsAtEveryDensity(density: CGFloat) throws {
        let available = margins(innerLandscape, radius: 55).inset(innerLandscape)
        let picture = try #require(GuestPicture(in: available, density: density, scale: 3))
        #expect(picture.size.width % 2 == 0)
        #expect(picture.size.height % 2 == 0)
        let pixelsW = picture.frame.width * 3, pixelsH = picture.frame.height * 3
        #expect(abs(pixelsW - pixelsW.rounded()) < 1e-6)
        #expect(abs(pixelsH - pixelsH.rounded()) < 1e-6)
        // One device pixel per guest pixel at 3x and density 3 would be 1; here k = 3 / density.
        #expect(abs(pixelsW - CGFloat(picture.size.width) * 3 / density) < 1e-6)
        #expect(abs(pixelsH - CGFloat(picture.size.height) * 3 / density) < 1e-6)
    }

    @Test func theOuterDisplayNeverRequestsUnder640AndKeepsItsAspect() throws {
        let available = margins(outerPortrait, radius: 55).inset(outerPortrait)
        let standard = try #require(GuestPicture(in: available, density: 1.5, scale: 3))
        #expect(standard.size == GuestSize(width: 650, height: 968))
        // A lower density would ask for 433 px; both axes move up together instead.
        let fast = try #require(GuestPicture(in: available, density: 1.0, scale: 3))
        #expect(fast.size.width >= 640)
        #expect(fast.size.height >= 480)
        let aspect = available.width / available.height
        #expect(abs(CGFloat(fast.size.width) / CGFloat(fast.size.height) - aspect) < 2 / CGFloat(fast.size.height))
        #expect(abs(fast.frame.width / fast.frame.height - aspect) < 2 / CGFloat(fast.size.height))
    }

    @Test func aNarrowWindowRaisesTheDensityForBothAxes() throws {
        let narrow = CGRect(x: 0, y: 0, width: 400, height: 600)
        let picture = try #require(GuestPicture(in: narrow, density: 1.5, scale: 3))
        #expect(picture.size == GuestSize(width: 640, height: 960))
        #expect(abs(picture.frame.width - 400) < 1e-9)
        #expect(abs(picture.frame.height - 600) < 1e-9)
    }

    @Test func aHugeWindowLowersTheDensityForBothAxes() throws {
        let huge = CGRect(x: 0, y: 0, width: 1366, height: 1024)
        let picture = try #require(GuestPicture(in: huge, density: 3, scale: 2))
        #expect(picture.size.width <= 3840)
        #expect(picture.size.height <= 2160)
        #expect(picture.size == GuestSize(width: 2880, height: 2160))
    }

    @Test func aStripTooLongForBothLimitsStopsAtTheMaximumAndKeepsItsAspect() throws {
        // 480 high needs 4.8 px/pt, which would make it 4800 wide; the guest stops at 3840.
        let wide = try #require(GuestPicture(in: CGRect(x: 0, y: 0, width: 1000, height: 100), scale: 3))
        #expect(wide.size == GuestSize(width: 3840, height: 480))
        #expect(wide.frame == CGRect(x: 100, y: 0, width: 800, height: 100))
        let tall = try #require(GuestPicture(in: CGRect(x: 0, y: 0, width: 320, height: 1200), scale: 3))
        #expect(tall.size == GuestSize(width: 640, height: 2160))
        #expect(tall.frame == CGRect(x: 0, y: 60, width: 320, height: 1080))
    }

    @Test(arguments: [CGRect(x: 0.5, y: 0, width: 400, height: 600), CGRect(x: 10.2, y: 7.9, width: 640.4, height: 480.3)])
    func theCanvasStaysInsideAnAreaThatIsOffThePixelGrid(available: CGRect) throws {
        let picture = try #require(GuestPicture(in: available, scale: 3))
        #expect(picture.frame.minX >= available.minX && picture.frame.maxX <= available.maxX)
        #expect(picture.frame.minY >= available.minY && picture.frame.maxY <= available.maxY)
        for v in [picture.frame.minX, picture.frame.minY, picture.frame.width, picture.frame.height] {
            #expect(abs(v * 3 - (v * 3).rounded()) < 1e-6)
        }
    }

    @Test func nothingToDrawIntoMeansNoPicture() {
        #expect(GuestPicture(in: CGRect(x: 0, y: 0, width: 0.5, height: 600), scale: 3) == nil)
        #expect(GuestPicture(in: .zero, scale: 3) == nil)
    }
}
