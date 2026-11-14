// SPDX-License-Identifier: GPL-2.0-or-later
import CoreGraphics

/// Display corner radii in points, named like SwiftUI's RectangleCornerRadii.
struct DisplayCorners: Equatable {
    var topLeading: CGFloat
    var topTrailing: CGFloat
    var bottomLeading: CGFloat
    var bottomTrailing: CGFloat

    init(topLeading: CGFloat, topTrailing: CGFloat, bottomLeading: CGFloat, bottomTrailing: CGFloat) {
        self.topLeading = max(0, topLeading)
        self.topTrailing = max(0, topTrailing)
        self.bottomLeading = max(0, bottomLeading)
        self.bottomTrailing = max(0, bottomTrailing)
    }

    init(all radius: CGFloat) {
        self.init(topLeading: radius, topTrailing: radius, bottomLeading: radius, bottomTrailing: radius)
    }

    /// Used when the system reports no radii.
    static let fallback = DisplayCorners(all: 55)

    var top: CGFloat { max(topLeading, topTrailing) }
    var bottom: CGFloat { max(bottomLeading, bottomTrailing) }
}

/// Edge insets in window coordinates; left and right are physical, not leading and trailing.
struct PictureInsets: Equatable {
    var top: CGFloat = 0
    var left: CGFloat = 0
    var bottom: CGFloat = 0
    var right: CGFloat = 0

    func inset(_ rect: CGRect) -> CGRect {
        CGRect(x: rect.minX + left, y: rect.minY + top,
               width: max(0, rect.width - left - right), height: max(0, rect.height - top - bottom))
    }
}

/// Which display corners the picture's own corners meet.
enum PictureMode {
    /// Studio, book and the outer display: the whole window, all four corners.
    case fullWindow
    /// The laptop lid meets only the top corners.
    case lid
    /// The laptop deck meets only the bottom corners.
    case deck
}

enum ScreenMargins {
    /// A square corner inset by m on both edges lies on a circle of radius R when m = R (1 - 1/sqrt 2).
    static let cornerFactor: CGFloat = 1 - 1 / CGFloat(2).squareRoot()

    /// The margins that keep the picture's square corners inside the display's rounded ones and the
    /// picture clear of every camera it would otherwise cover. `area` is the part of the window the
    /// picture occupies; cameras are in window coordinates.
    static func insets(in area: CGRect, radii: DisplayCorners, cameras: [CGRect], mode: PictureMode, scale: CGFloat) -> PictureInsets {
        // Corners the picture does not meet count as square.
        var r = radii
        switch mode {
        case .fullWindow: break
        case .lid: r.bottomLeading = 0; r.bottomTrailing = 0
        case .deck: r.topLeading = 0; r.topTrailing = 0
        }
        let m = clearing(cameras, corners: r, in: area)
        return PictureInsets(top: roundedUp(m.top, scale), left: roundedUp(m.left, scale),
                             bottom: roundedUp(m.bottom, scale), right: roundedUp(m.right, scale))
    }

    /// The thinnest insets that clear four corners of different radii, none below `base` (edges already
    /// raised for a camera). One pair of opposite edges takes each corner's even share (R times
    /// cornerFactor, the larger of its two corners); the other pair then needs only what is left to
    /// clear each corner, which is nothing where the first pair already passes the radius (a nearly
    /// square hinge-side corner beside a round one); the first pair then comes back down to what the
    /// second leaves it to do. Both orders are tried and the one that keeps more picture wins; with
    /// equal radii and no base they agree.
    static func cornerInsets(_ r: DisplayCorners, in area: CGRect, atLeast base: PictureInsets = PictureInsets()) -> PictureInsets {
        func share(_ radius: CGFloat) -> CGFloat { radius * cornerFactor }
        // What one edge of a corner still needs when the other edge is inset by `other`. A corner is
        // clear exactly when either edge has what the other leaves it, so lowering one edge to this
        // never uncovers a corner.
        func rest(_ radius: CGFloat, _ other: CGFloat) -> CGFloat {
            guard other < radius else { return 0 }
            let gap = radius - other
            return radius - (radius * radius - gap * gap).squareRoot()
        }
        func rows(_ m: inout PictureInsets) {
            m.top = max(base.top, rest(r.topLeading, m.left), rest(r.topTrailing, m.right))
            m.bottom = max(base.bottom, rest(r.bottomLeading, m.left), rest(r.bottomTrailing, m.right))
        }
        func columns(_ m: inout PictureInsets) {
            m.left = max(base.left, rest(r.topLeading, m.top), rest(r.bottomLeading, m.bottom))
            m.right = max(base.right, rest(r.topTrailing, m.top), rest(r.bottomTrailing, m.bottom))
        }
        var rowsFirst = base
        rowsFirst.top = max(base.top, share(r.topLeading), share(r.topTrailing))
        rowsFirst.bottom = max(base.bottom, share(r.bottomLeading), share(r.bottomTrailing))
        columns(&rowsFirst)
        rows(&rowsFirst)
        var columnsFirst = base
        columnsFirst.left = max(base.left, share(r.topLeading), share(r.bottomLeading))
        columnsFirst.right = max(base.right, share(r.topTrailing), share(r.bottomTrailing))
        rows(&columnsFirst)
        columns(&columnsFirst)
        return kept(columnsFirst, in: area) > kept(rowsFirst, in: area) ? columnsFirst : rowsFirst
    }

    private static func kept(_ m: PictureInsets, in area: CGRect) -> CGFloat {
        let picture = m.inset(area)
        return picture.width * picture.height
    }

    /// More cameras than any display has; past this only the largest are cleared.
    private static let cameraLimit = 6

    /// Corner margins with every camera cleared. A camera is cleared by one edge raised past it, or by
    /// nothing where the margins already miss it. Every choice per camera is tried (a display has one
    /// or two), the corners are solved around the raised edges, and the choice that keeps the most
    /// picture wins, so the result does not depend on the order the cameras come in and a raised edge
    /// thins the margins beside it.
    private static func clearing(_ cameras: [CGRect], corners r: DisplayCorners, in area: CGRect) -> PictureInsets {
        let plain = cornerInsets(r, in: area)
        var inside: [CGRect] = []
        for camera in cameras {
            let hit = camera.intersection(area)
            if !hit.isNull, hit.width > 0, hit.height > 0 { inside.append(hit) }
        }
        guard !inside.isEmpty else { return plain }
        // Largest first, in an order that is the same however the cameras were listed.
        inside.sort { (a: CGRect, b: CGRect) -> Bool in
            let areaA: CGFloat = a.width * a.height, areaB: CGFloat = b.width * b.height
            if areaA != areaB { return areaA > areaB }
            if a.minX != b.minX { return a.minX < b.minX }
            if a.minY != b.minY { return a.minY < b.minY }
            return a.width < b.width
        }
        if inside.count > cameraLimit { inside.removeLast(inside.count - cameraLimit) }
        // Per camera: no raise (0), or the top, bottom, left or right edge moved past it.
        let choices = 5
        var best: PictureInsets?
        var choice = [Int](repeating: 0, count: inside.count)
        search: while true {
            var base = PictureInsets()
            for (camera, pick) in zip(inside, choice) {
                switch pick {
                case 1: base.top = max(base.top, camera.maxY - area.minY)
                case 2: base.bottom = max(base.bottom, area.maxY - camera.minY)
                case 3: base.left = max(base.left, camera.maxX - area.minX)
                case 4: base.right = max(base.right, area.maxX - camera.minX)
                default: break
                }
            }
            let m = cornerInsets(r, in: area, atLeast: base)
            let picture = m.inset(area)
            let clear = picture.width > 0 && picture.height > 0 && inside.allSatisfy {
                let hit = picture.intersection($0)
                return hit.isNull || hit.width <= 1e-6 || hit.height <= 1e-6
            }
            if clear {
                if let found = best, kept(found, in: area) >= kept(m, in: area) {} else { best = m }
            }
            // The next choice, as an odometer.
            var digit = 0
            while true {
                guard digit < choice.count else { break search }
                choice[digit] += 1
                if choice[digit] < choices { break }
                choice[digit] = 0
                digit += 1
            }
        }
        // A camera across the whole area cannot be cleared; the corners still are.
        return best ?? plain
    }

    /// Never negative, and up to the next device pixel.
    static func roundedUp(_ value: CGFloat, _ scale: CGFloat) -> CGFloat {
        guard value > 0, scale > 0 else { return 0 }
        return (value * scale - 1e-6).rounded(.up) / scale
    }
}

struct GuestSize: Hashable {
    var width: Int
    var height: Int
}

/// How many Windows pixels the picture gets per point. Fewer is less work for Windows, more is sharper;
/// Windows' own scale follows it (WindowsScale), so nothing on screen changes size. Windows takes a
/// scale at sign-in, so a new density shows once Windows has restarted.
enum DisplayDensity: String, CaseIterable {
    case fast, standard, retina

    var pixelsPerPoint: CGFloat {
        switch self {
        case .fast: return 1
        case .standard: return 1.5
        case .retina: return 2
        }
    }

    var title: String {
        switch self {
        case .fast: return "Fast"
        case .standard: return "Standard"
        case .retina: return "Retina"
        }
    }
}

/// How large Windows draws its text and controls. Standard is one Windows unit (a pixel at 100
/// percent) to the point.
enum DisplayUISize: String, CaseIterable {
    case smaller, standard, larger

    var factor: CGFloat {
        switch self {
        case .smaller: return 5.0 / 6
        case .standard: return 1
        case .larger: return 7.0 / 6
        }
    }

    var title: String {
        switch self {
        case .smaller: return "Smaller"
        case .standard: return "Standard"
        case .larger: return "Larger"
        }
    }
}

enum WindowsScale {
    /// The scales the app asks Windows for, in percent. The agent in Windows takes none above these
    /// (tools/winios_agent_scale.h).
    static let steps = [100, 125, 150, 175, 200, 225, 250]

    /// The scale that keeps Windows' units at `uiSize.factor` points each when the picture has
    /// `pixelsPerPoint` guest pixels to the point: the nearest step, the lower one on a tie.
    static func percent(pixelsPerPoint: CGFloat, uiSize: DisplayUISize) -> Int {
        let wanted = Double(pixelsPerPoint * uiSize.factor) * 100
        return steps.min { abs(Double($0) - wanted) < abs(Double($1) - wanted) } ?? 100
    }

    /// Guest pixels per point for the picture. Once Windows runs at the scale asked for, that is the
    /// density setting exactly. Until it does (it takes a scale at sign-in, and `runningPercent` is
    /// what its agent says it has now), the picture is sized for the scale Windows has, so its units
    /// are already the size they will be. With no word from Windows yet (0): the setting, when its
    /// Tools have reported a scale before (`toolsReport`: it is starting, at the scale they wrote
    /// down); otherwise the density every Windows without such Tools was set up for.
    static func pixelsPerPoint(density: DisplayDensity, uiSize: DisplayUISize, runningPercent: Int, toolsReport: Bool) -> CGFloat {
        guard runningPercent > 0 else { return toolsReport ? density.pixelsPerPoint : GuestPicture.density }
        let wanted = percent(pixelsPerPoint: density.pixelsPerPoint, uiSize: uiSize)
        return density.pixelsPerPoint * CGFloat(runningPercent) / CGFloat(wanted)
    }

    /// What Settings says under the two display settings. `framebuffer` is what Windows draws now
    /// and `drawnPixelsPerPoint` what the picture on screen really got, when there is one.
    static func note(density: DisplayDensity, uiSize: DisplayUISize, toolsReport: Bool, windowsRunning: Bool,
                     runningPercent: Int, framebuffer: GuestSize?, drawnPixelsPerPoint: CGFloat?) -> String {
        var text = "Fast gives Windows fewer pixels to draw, so it runs lighter. Retina is the sharpest and the heaviest. Windows takes a change the next time it restarts."
        guard toolsReport else {
            return text + " These settings need the current WindowsiOS Tools installed in Windows."
        }
        if density == .fast, uiSize == .smaller {
            // 100 percent is as low as Windows goes, and Fast is already there.
            text += " At Fast, Smaller is the same as Standard."
        }
        guard windowsRunning, runningPercent > 0 else { return text }
        let wanted = percent(pixelsPerPoint: density.pixelsPerPoint, uiSize: uiSize)
        if runningPercent != wanted {
            text += " It is at \(runningPercent) percent now and will be at \(wanted)."
        } else if let framebuffer, framebuffer.width > 0 {
            text += " It is at \(framebuffer.width) by \(framebuffer.height) pixels and \(runningPercent) percent."
        }
        let asked = pixelsPerPoint(density: density, uiSize: uiSize, runningPercent: runningPercent, toolsReport: toolsReport)
        if let drawnPixelsPerPoint, drawnPixelsPerPoint > asked * 1.02 {
            // Windows needs GuestPicture.minimum pixels whatever the display's size in points.
            text += " This display is too small for so few pixels, so Windows looks smaller on it."
        }
        return text
    }
}

/// The Windows picture sized from guest pixels: an even pixel count per axis, a canvas that holds
/// exactly that many guest pixels, and a request to the guest for the same count, so the aspect-fit
/// draw maps one to one once Windows has switched mode.
struct GuestPicture: Equatable {
    let size: GuestSize
    /// In the window's coordinates, in points, on the device pixel grid.
    let frame: CGRect

    /// Guest pixels per point: on a 3x panel this is exactly 2 device pixels per Windows pixel.
    static let density: CGFloat = 1.5
    static let minimum = GuestSize(width: 640, height: 480)
    static let maximum = GuestSize(width: 3840, height: 2160)

    /// Guest pixels per point as drawn: the density asked for, unless the guest's limits moved it
    /// (a display narrower than the minimum needs gets more).
    var pixelsPerPoint: CGFloat {
        frame.width > 0 ? CGFloat(size.width) / frame.width : GuestPicture.density
    }

    /// The largest even W by H with W/P and H/P inside `available`. The density P is the one asked
    /// for, moved only as far as the guest's limits need, and moved for both axes together so the
    /// aspect never changes.
    init?(in available: CGRect, density: CGFloat = GuestPicture.density, scale: CGFloat) {
        guard density > 0, scale > 0 else { return nil }
        // Onto the device pixel grid first, so centring cannot put the canvas outside `available`.
        let eps: CGFloat = 1e-6
        let minX = (available.minX * scale - eps).rounded(.up) / scale, maxX = (available.maxX * scale + eps).rounded(.down) / scale
        let minY = (available.minY * scale - eps).rounded(.up) / scale, maxY = (available.maxY * scale + eps).rounded(.down) / scale
        let aw = maxX - minX, ah = maxY - minY
        guard aw >= 1, ah >= 1 else { return nil }
        let lowest = max(CGFloat(Self.minimum.width) / aw, CGFloat(Self.minimum.height) / ah)
        let highest = min(CGFloat(Self.maximum.width) / aw, CGFloat(Self.maximum.height) / ah)
        let p = max(min(density, highest), lowest)

        // A strip too long for both limits at one density keeps the minimum across it and stops at the
        // maximum along it; the canvas is then shorter than the strip, at the guest's aspect.
        let w = min(2 * Int(((aw * p + eps) / 2).rounded(.down)), Self.maximum.width)
        let h = min(2 * Int(((ah * p + eps) / 2).rounded(.down)), Self.maximum.height)
        // Whole device pixels; exact on a 3x panel at 1, 1.5 and 2.
        let pixelsW = (CGFloat(w) * scale / p + eps).rounded(.down)
        let pixelsH = (CGFloat(h) * scale / p + eps).rounded(.down)
        let frameW = pixelsW / scale, frameH = pixelsH / scale
        // Centred, then down to the pixel grid.
        let x = ((minX + (aw - frameW) / 2) * scale + eps).rounded(.down) / scale
        let y = ((minY + (ah - frameH) / 2) * scale + eps).rounded(.down) / scale
        size = GuestSize(width: w, height: h)
        frame = CGRect(x: x, y: y, width: frameW, height: frameH)
    }
}
