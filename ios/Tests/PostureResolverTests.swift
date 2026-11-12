// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics
import Testing

struct PostureResolverTests {
    private let innerPortrait = CGSize(width: 669, height: 951)
    private let innerLandscape = CGSize(width: 951, height: 669)
    private let clamshellCrease = CGRect(x: 0, y: 463, width: 669, height: 25)
    private let bookCrease = CGRect(x: 463, y: 0, width: 25, height: 669)

    @Test func divisionRegionShapeDecidesTheAxis() {
        #expect(FoldGeometry(divisionFrame: clamshellCrease).axis == .horizontal)
        #expect(FoldGeometry(divisionFrame: bookCrease).axis == .vertical)
    }

    @Test func autoUsesLaptopOnlyForAHorizontalCrease() {
        let clamshell = PostureResolver.fold(divisionFrames: [clamshellCrease], hinge: .partiallyOpen, windowSize: innerPortrait)
        let book = PostureResolver.fold(divisionFrames: [bookCrease], hinge: .partiallyOpen, windowSize: innerLandscape)
        #expect(PostureResolver.isLaptop(mode: .auto, fold: clamshell))
        #expect(!PostureResolver.isLaptop(mode: .auto, fold: book))
        #expect(!PostureResolver.isLaptop(mode: .auto, fold: nil))
    }

    @Test func pinsOverrideTheFold() {
        let book = FoldGeometry(divisionFrame: bookCrease)
        #expect(PostureResolver.isLaptop(mode: .laptop, fold: book))
        #expect(!PostureResolver.isLaptop(mode: .fullScreen, fold: FoldGeometry(divisionFrame: clamshellCrease)))
    }

    @Test func noRegionAndNoHalfOpenHingeMeansNoFold() {
        #expect(PostureResolver.fold(divisionFrames: [], hinge: .fullyOpen, windowSize: innerPortrait) == nil)
        #expect(PostureResolver.fold(divisionFrames: [], hinge: .unknown, windowSize: innerPortrait) == nil)
    }

    @Test func halfOpenHingeWithoutARegionFallsBackToTheMidline() throws {
        let portrait = try #require(PostureResolver.fold(divisionFrames: [], hinge: .partiallyOpen, windowSize: innerPortrait))
        #expect(portrait.axis == .horizontal)
        #expect(portrait.frame.midY == innerPortrait.height / 2)
        let landscape = try #require(PostureResolver.fold(divisionFrames: [], hinge: .partiallyOpen, windowSize: innerLandscape))
        #expect(landscape.axis == .vertical)
    }

    @Test(arguments: [
        (HingeStatus.fullyOpen, HingeStatus.partiallyOpen, PostureMode.laptop, PostureMode.auto),
        (.partiallyOpen, .fullyOpen, .fullScreen, .auto),
        (.closed, .fullyOpen, .laptop, .auto),
        (.partiallyOpen, .partiallyOpen, .laptop, .laptop),
        (.unknown, .partiallyOpen, .fullScreen, .fullScreen),
        (.fullyOpen, .unknown, .laptop, .laptop),
        (.fullyOpen, .partiallyOpen, .auto, .auto),
    ])
    func manualChoiceHoldsUntilTheNextFold(old: HingeStatus, new: HingeStatus, mode: PostureMode, expected: PostureMode) {
        #expect(PostureResolver.mode(mode, afterHingeChangeFrom: old, to: new) == expected)
    }

    @Test func clamshellSplitLandsOnTheCrease() {
        let split = PostureResolver.laptopSplit(height: 951, fold: FoldGeometry(divisionFrame: clamshellCrease))
        #expect(split.lid == 463)
        #expect(split.crease == 25)
        #expect(split.deck == 463)
    }

    @Test func flatOrBookSplitKeepsTheUpstreamHalves() {
        let flat = PostureResolver.laptopSplit(height: 951, fold: nil)
        #expect(flat.lid == 475)
        #expect(flat.crease == 0)
        let book = PostureResolver.laptopSplit(height: 669, fold: FoldGeometry(divisionFrame: bookCrease))
        #expect(book.lid == 334)
        #expect(book.crease == 0)
    }

    @Test func outerDisplayKeepsTheMinimumDeck() {
        #expect(PostureResolver.laptopSplit(height: 300, fold: nil).deck == 180)
    }

    @Test func bootCardStaysOffTheCrease() {
        let book = FoldGeometry(divisionFrame: bookCrease)
        let pane = PostureResolver.primaryPane(windowSize: innerLandscape, fold: book, laptop: false)
        #expect(pane.maxX <= bookCrease.minX)
        let clamshell = FoldGeometry(divisionFrame: clamshellCrease)
        let lid = PostureResolver.primaryPane(windowSize: innerPortrait, fold: clamshell, laptop: true)
        #expect(lid.maxY <= clamshellCrease.minY)
        #expect(PostureResolver.primaryPane(windowSize: innerPortrait, fold: nil, laptop: false).size == innerPortrait)
    }
}
