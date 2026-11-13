// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Testing

struct DisplaySettingsTests {
    private func percent(_ density: DisplayDensity, _ size: DisplayUISize) -> Int {
        WindowsScale.percent(pixelsPerPoint: density.pixelsPerPoint, uiSize: size)
    }

    @Test func windowsScaleFollowsTheDensitySoAUnitStaysAPoint() {
        #expect(percent(.fast, .standard) == 100)
        #expect(percent(.standard, .standard) == 150)
        #expect(percent(.retina, .standard) == 200)
    }

    @Test func theSizeSettingMovesWindowsOneStepEitherWay() {
        #expect(percent(.standard, .smaller) == 125)
        #expect(percent(.standard, .larger) == 175)
        #expect(percent(.retina, .smaller) == 175)
        #expect(percent(.retina, .larger) == 225)
        #expect(percent(.fast, .larger) == 125)
    }

    @Test func windowsNeverGoesUnderOneHundredPercent() {
        #expect(percent(.fast, .smaller) == 100)
        #expect(WindowsScale.percent(pixelsPerPoint: 0.5, uiSize: .smaller) == 100)
    }

    @Test func aDensityBetweenStepsTakesTheNearerOneAndTheLowerOnATie() {
        #expect(WindowsScale.percent(pixelsPerPoint: 1.36, uiSize: .standard) == 125)
        #expect(WindowsScale.percent(pixelsPerPoint: 1.39, uiSize: .standard) == 150)
        #expect(WindowsScale.percent(pixelsPerPoint: 1.375, uiSize: .standard) == 125)
        #expect(WindowsScale.percent(pixelsPerPoint: 1.125, uiSize: .standard) == 100)
        // The agent takes nothing above 250 (tools/winios_agent_scale.h), so the app asks for no more.
        #expect(WindowsScale.percent(pixelsPerPoint: 9, uiSize: .standard) == 250)
        #expect(WindowsScale.steps.max() == 250)
    }

    private func pixels(_ density: DisplayDensity, _ size: DisplayUISize, running: Int, toolsReport: Bool = true) -> CGFloat {
        WindowsScale.pixelsPerPoint(density: density, uiSize: size, runningPercent: running, toolsReport: toolsReport)
    }

    @Test(arguments: DisplayDensity.allCases, DisplayUISize.allCases)
    func thePictureHasTheDensitySetOnceWindowsRunsAtItsScale(density: DisplayDensity, size: DisplayUISize) {
        #expect(pixels(density, size, running: percent(density, size)) == density.pixelsPerPoint)
    }

    @Test(arguments: DisplayDensity.allCases, DisplayUISize.allCases)
    func whileWindowsStartsThePictureHasTheDensitySet(density: DisplayDensity, size: DisplayUISize) {
        #expect(pixels(density, size, running: 0) == density.pixelsPerPoint)
        #expect(pixels(density, size, running: -1) == density.pixelsPerPoint)
    }

    @Test(arguments: DisplayDensity.allCases, DisplayUISize.allCases)
    func toolsFromBeforeTheSettingsKeepTheDensityTheyWereMadeFor(density: DisplayDensity, size: DisplayUISize) {
        #expect(pixels(density, size, running: 0, toolsReport: false) == 1.5)
    }

    @Test func aNewDensityWaitsForWindowsToRestart() {
        // Windows is at 150 percent; Retina and Fast were just chosen. The picture stays at 1.5.
        #expect(pixels(.retina, .standard, running: 150) == 1.5)
        #expect(pixels(.fast, .standard, running: 150) == 1.5)
        // Windows restarted at 200 percent with Standard chosen since: it stays at 2 until the next one.
        #expect(pixels(.standard, .standard, running: 200) == 2)
    }

    @Test func aNewSizeShowsAtOnceAndSharpensWhenWindowsRestarts() {
        // Larger at Standard asks for 175 percent. At 150 the picture gets fewer pixels, so Windows'
        // units are already 7/6 of a point.
        let before = pixels(.standard, .larger, running: 150)
        #expect(abs(before - 1.5 * 150 / 175) < 1e-9)
        #expect(abs(1.5 / before - 7.0 / 6) < 1e-9)
        #expect(pixels(.standard, .larger, running: 175) == 1.5)
    }

    @Test(arguments: DisplayDensity.allCases, DisplayUISize.allCases)
    func windowsUnitsAreTheSizeTheyWillBeWhateverScaleItRunsAt(density: DisplayDensity, size: DisplayUISize) {
        let settled = density.pixelsPerPoint / (CGFloat(percent(density, size)) / 100)
        for running in WindowsScale.steps {
            #expect(abs(pixels(density, size, running: running) / (CGFloat(running) / 100) - settled) < 1e-9)
        }
    }

    // MARK: What Settings says

    private let base = "Fast gives Windows fewer pixels to draw, so it runs lighter. Retina is the sharpest and the heaviest. Windows takes a change the next time it restarts."

    private func note(_ density: DisplayDensity = .standard, _ size: DisplayUISize = .standard, toolsReport: Bool = true,
                      windowsRunning: Bool = true, running: Int = 150, drawn: CGFloat? = nil) -> String {
        WindowsScale.note(density: density, uiSize: size, toolsReport: toolsReport, windowsRunning: windowsRunning,
                          runningPercent: running, framebuffer: GuestSize(width: 1378, height: 954), drawnPixelsPerPoint: drawn)
    }

    @Test func settledItSaysWhatWindowsIsAt() {
        #expect(note() == base + " It is at 1378 by 954 pixels and 150 percent.")
    }

    @Test func aPendingChangeSaysBothScales() {
        #expect(note(.retina) == base + " It is at 150 percent now and will be at 200.")
        #expect(note(.standard, .larger) == base + " It is at 150 percent now and will be at 175.")
    }

    @Test func withWindowsOffOrStillStartingItSaysNoScale() {
        #expect(note(windowsRunning: false) == base)
        #expect(note(.retina, windowsRunning: false, running: 150) == base)
        #expect(note(running: 0) == base)
    }

    @Test func toolsFromBeforeTheSettingsAreNamedWhateverElseIsTrue() {
        let needed = base + " These settings need the current WindowsiOS Tools installed in Windows."
        #expect(note(toolsReport: false, running: 0) == needed)
        #expect(note(.retina, toolsReport: false, windowsRunning: false, running: 0) == needed)
        #expect(note(.fast, .smaller, toolsReport: false, running: 0) == needed)
    }

    @Test func smallerAtFastIsSaidToChangeNothing() {
        #expect(percent(.fast, .smaller) == percent(.fast, .standard))
        #expect(note(.fast, .smaller, running: 100) == base + " At Fast, Smaller is the same as Standard. It is at 1378 by 954 pixels and 100 percent.")
        #expect(!note(.fast, .larger, running: 125).contains("Smaller"))
        #expect(!note(.standard, .smaller, running: 125).contains("the same as"))
    }

    @Test func aDisplayTooSmallForTheDensityIsSaidToLookSmaller() throws {
        let small = " This display is too small for so few pixels, so Windows looks smaller on it."
        // The Duo's outer display under its camera: Windows needs 640 pixels across, more than Fast gives it.
        let outer = try #require(GuestPicture(in: CGRect(x: 0, y: 82, width: 448.67, height: 578.67),
                                              density: pixels(.fast, .standard, running: 100), scale: 3))
        #expect(outer.size.width == 640)
        #expect(outer.pixelsPerPoint > 1.4)
        #expect(note(.fast, running: 100, drawn: outer.pixelsPerPoint).hasSuffix(small))
        // The inner display has room for Fast, and the outer one for Standard.
        let inner = try #require(GuestPicture(in: CGRect(x: 16.33, y: 16.33, width: 918.33, height: 636.33),
                                              density: pixels(.fast, .standard, running: 100), scale: 3))
        #expect(abs(inner.pixelsPerPoint - 1) < 0.01)
        #expect(!note(.fast, running: 100, drawn: inner.pixelsPerPoint).contains("too small"))
        let outerStandard = try #require(GuestPicture(in: CGRect(x: 0, y: 82, width: 448.67, height: 578.67),
                                                      density: pixels(.standard, .standard, running: 150), scale: 3))
        #expect(!note(drawn: outerStandard.pixelsPerPoint).contains("too small"))
        // While a change waits for the restart the picture is sized for the scale Windows has, and is judged against that.
        #expect(!note(.fast, running: 150, drawn: 1.5).contains("too small"))
        #expect(note(.fast, running: 150, drawn: 1.6).hasSuffix(small))
    }
}
