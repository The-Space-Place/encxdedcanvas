/*  
 * InfiniPaint
 * Copyright (C) 2025-2026 Yousef Khadadeh
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "MeshCanvasComponent.hpp"
#include <Helpers/Serializers.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/utility.hpp>
#include <chrono>
#include <include/core/SkPathTypes.h>
#include <limits>
#include <Eigen/Geometry>
#include "BrushComponentCode.hpp"
#include "CanvasComponent.hpp"
#include "Eigen/Core"
#include "Helpers/MathExtras.hpp"
#include "../TimePoint.hpp"
#include <include/core/SkVertices.h>
#include <include/pathops/SkPathOps.h>
#include <memory>
#include "../DrawCollision.hpp"
#include "CanvasComponentContainer.hpp"
#include "Helpers/SCollision.hpp"
#include "../DrawingProgram/DrawingProgram.hpp"
#include "../World.hpp"
#include <CDT/include/CDT.h>
#include <clipper2/clipper.h>
#include <Helpers/Logger.hpp>

template <typename Archive> void skpath_write(const SkPath& p, Archive& a) {
    std::vector<std::vector<SkPoint>> contours;
    bool moveHappened = false;

    SkPath::Iter iter(p, true);
    for(;;) {
        std::optional<SkPath::IterRec> rec = iter.next();
        if(!rec.has_value())
            break;

        switch(rec->fVerb) {
            case SkPathVerb::kClose:
                if(moveHappened) {
                    contours.back().pop_back();
                    moveHappened = false;
                }
                break;
            case SkPathVerb::kLine:
                contours.back().emplace_back(rec->fPoints[1]);
                break;
            case SkPathVerb::kMove:
                contours.emplace_back();
                contours.back().emplace_back(rec->fPoints[0]);
                moveHappened = true;
                break;
            default:
                throw std::runtime_error("[get_predraw_data_accurate] Illegal verb " + std::to_string(static_cast<unsigned>(rec->fVerb)));
                break;
        }
    }

    a(p.getFillType() == SkPathFillType::kEvenOdd, contours);
}

template <typename Archive> SkPath skpath_read(Archive& a) {
    bool isEvenOdd;
    std::vector<std::vector<SkPoint>> contours;
    a(isEvenOdd, contours);
    // NOTE: setFillType doesn't properly set the fill type for the path IN DEBUG BUILDS. Setting fill type in builder constructor meanwhile works for both debug and release builds
    SkPathBuilder builder(isEvenOdd ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
    for(const std::vector<SkPoint>& contour : contours) {
        if(contour.size() == 0)
            continue;
        builder.moveTo(contour[0]);
        for(uint32_t i = 1; i < contour.size(); i++)
            builder.lineTo(contour[i]);
        builder.close();
    }
    return builder.detach();
}

namespace {
    enum class PenSourceKind : uint8_t {
        NONE = 0,
        POINTS = 1, // Processed points only (line tool, older strokes)
        RAW = 2     // Raw samples plus the settings they were drawn with
    };

    void write_erase_ops(const MeshCanvasComponent::Data& d, cereal::PortableBinaryOutputArchive& a) {
        a(static_cast<uint32_t>(d.eraseOps.size()));
        for(const SkPath& p : d.eraseOps)
            skpath_write(p, a);
    }

    void read_erase_ops(MeshCanvasComponent::Data& d, cereal::PortableBinaryInputArchive& a) {
        uint32_t eraseCount;
        a(eraseCount);
        d.eraseOps.clear();
        for(uint32_t i = 0; i < eraseCount; i++)
            d.eraseOps.emplace_back(skpath_read(a));
    }

    void write_pen_source(const MeshCanvasComponent::Data& d, cereal::PortableBinaryOutputArchive& a) {
        PenSourceKind kind = !d.rawSamples.empty() ? PenSourceKind::RAW : (!d.penPoints.empty() ? PenSourceKind::POINTS : PenSourceKind::NONE);
        a(static_cast<uint8_t>(kind));
        if(kind == PenSourceKind::RAW)
            a(d.rawSamples, d.strokeParams);
        else if(kind == PenSourceKind::POINTS)
            a(d.penPoints);
        a(d.penRoundCaps, d.penWidthScale, d.keptRanges);
        write_erase_ops(d, a);
    }

    // Returns true if penPoints need to be rebuilt from raw samples
    bool read_pen_source(MeshCanvasComponent::Data& d, cereal::PortableBinaryInputArchive& a) {
        uint8_t kind;
        a(kind);
        d.rawSamples.clear();
        d.penPoints.clear();
        if(kind == static_cast<uint8_t>(PenSourceKind::RAW))
            a(d.rawSamples, d.strokeParams);
        else if(kind == static_cast<uint8_t>(PenSourceKind::POINTS))
            a(d.penPoints);
        a(d.penRoundCaps, d.penWidthScale, d.keptRanges);
        read_erase_ops(d, a);
        return !d.rawSamples.empty();
    }

    // EncxdedCanvas 1.0: processed points and outline cuts only
    void read_pen_source_1_0(MeshCanvasComponent::Data& d, cereal::PortableBinaryInputArchive& a) {
        a(d.penPoints, d.penRoundCaps);
        read_erase_ops(d, a);
    }
}

void MeshCanvasComponent::process_raw_samples() {
    if(!d.rawSamples.empty())
        d.penPoints = BrushComponentCode::process_raw_samples(d.rawSamples, d.strokeParams, true);
}

void MeshCanvasComponent::save(cereal::PortableBinaryOutputArchive& a) const {
    skpath_write(d.meshPath, a);
    a(d.color);
    write_pen_source(d, a);
}

void MeshCanvasComponent::load(cereal::PortableBinaryInputArchive& a) {
    d.meshPath = skpath_read(a);
    a(d.color);
    if(read_pen_source(d, a))
        process_raw_samples();
}

void MeshCanvasComponent::save_file(cereal::PortableBinaryOutputArchive& a) const {
    skpath_write(d.meshPath, a);
    a(d.color);
    write_pen_source(d, a);
}

void MeshCanvasComponent::load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) {
    if(version >= VersionNumber(1, 1, 0)) { // Raw samples
        d.meshPath = skpath_read(a);
        a(d.color);
        if(read_pen_source(d, a))
            process_raw_samples();
    }
    else if(version >= VersionNumber(1, 0, 0)) { // EncxdedCanvas 1.0: pen points included
        d.meshPath = skpath_read(a);
        a(d.color);
        read_pen_source_1_0(d, a);
    }
    else if(version >= VersionNumber(0, 6, 0)) {
        d.meshPath = skpath_read(a);
        a(d.color);
    }
    else {
        std::vector<BrushComponentCode::BrushPoint> brushPoints;
        bool hasRoundCaps;
        a(brushPoints, d.color, hasRoundCaps);
        d.penPoints = brushPoints;
        d.penRoundCaps = hasRoundCaps;
        d.meshPath = BrushComponentCode::brush_stroke_to_skpath(brushPoints, hasRoundCaps);
        std::optional<SkPath> simplified = Simplify(d.meshPath);
        if(simplified.has_value())
            d.meshPath = simplified.value();
    }
}

std::unique_ptr<CanvasComponent> MeshCanvasComponent::get_data_copy() const {
    auto toRet = std::make_unique<MeshCanvasComponent>();
    toRet->d = d;
    return toRet;
}

CanvasComponentType MeshCanvasComponent::get_type() const {
    return CanvasComponentType::MESH;
}

void MeshCanvasComponent::change_stroke_color(const Vector4f& newStrokeColor) {
    d.color = newStrokeColor;
}

std::optional<Vector4f> MeshCanvasComponent::get_stroke_color() const {
    return d.color;
}

bool MeshCanvasComponent::has_pen_source() const {
    return !d.penPoints.empty();
}

std::vector<BrushComponentCode::BrushPoint> MeshCanvasComponent::scaled_centerline() const {
    std::vector<BrushComponentCode::BrushPoint> centerline = BrushComponentCode::stroke_centerline(d.penPoints);
    for(auto& p : centerline)
        p.width *= d.penWidthScale;
    return centerline;
}

bool MeshCanvasComponent::regenerate_from_pen_source(bool simplify) {
    if(!has_pen_source())
        return false;
    std::vector<BrushComponentCode::BrushPoint> centerline = scaled_centerline();
    SkPathBuilder builder;
    try {
        if(d.keptRanges.empty()) {
            std::vector<BrushComponentCode::BrushPoint> points = d.penPoints;
            for(auto& p : points)
                p.width *= d.penWidthScale;
            builder.addPath(BrushComponentCode::create_triangles(points, centerline, d.penRoundCaps));
        }
        else {
            // Each piece left by the centerline eraser is its own outline with its own caps
            std::vector<float> arcLengths = BrushComponentCode::centerline_arc_lengths(centerline);
            for(auto& [s0, s1] : d.keptRanges) {
                std::vector<BrushComponentCode::BrushPoint> piece = BrushComponentCode::centerline_range(centerline, arcLengths, s0, s1);
                builder.addPath(BrushComponentCode::create_triangles(piece, piece, d.penRoundCaps));
            }
        }
    }
    catch(...) {
        return false; // Degenerate points; keep the current outline
    }
    SkPath path = builder.detach();
    if(simplify) {
        if(std::optional<SkPath> simplified = Simplify(path))
            path = simplified.value();
    }
    for(const SkPath& eraseOp : d.eraseOps) {
        if(std::optional<SkPath> erased = Op(path, eraseOp, SkPathOp::kDifference_SkPathOp))
            path = erased.value();
    }
    d.meshPath = path;
    return true;
}

bool MeshCanvasComponent::scale_stroke_thickness(float factor) {
    if(!has_pen_source() || !(factor > 0.0f))
        return false;
    d.penWidthScale *= factor;
    return regenerate_from_pen_source();
}

SkPath MeshCanvasComponent::get_smooth_path() const {
    std::scoped_lock lock(smoothPathMutex);
    uint32_t sourceGenID = d.meshPath.getGenerationID();
    if(sourceGenID != smoothPathSourceGenID) {
        smoothPath = BrushComponentCode::smooth_polygon_path(d.meshPath);
        smoothPathSourceGenID = sourceGenID;
    }
    return smoothPath;
}

void MeshCanvasComponent::draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const {
    SkPaint paint;
    paint.setColor4f(SkColor4f{d.color.x(), d.color.y(), d.color.z(), d.color.w()});
    paint.setAntiAlias(drawData.skiaAA);
    canvas->drawPath(get_smooth_path(), paint);
}

namespace {
    // EncxdedCanvas: screen-space stroke drawing. When a stroke is much larger than the view, drawing it with a transformed canvas
    // makes Skia process the whole (huge) outline every frame, and the old accurate path ran CDT over every flattened curve.
    // Instead, the smooth path is mapped to camera space in doubles and only the visible part is kept:
    //  - A curve whose control triangle misses the view is replaced by its chord. The region between a curve and its chord lies
    //    inside that triangle, so the fill inside the view is unchanged.
    //  - Visible curves are split until they're within SCREEN_SPACE_TOLERANCE pixels of the true curve.
    //  - Each contour is then clipped to the view (Sutherland-Hodgman keeps the winding of every point inside the view).
    // Deep inside a stroke this degenerates to a plain rectangle, and a deep-zoom edge to a few nearly straight segments.
    constexpr double SCREEN_SPACE_TOLERANCE = 0.25;
    constexpr int MAX_QUAD_SUBDIVISION_DEPTH = 48;
    constexpr int MAX_UNIFORM_QUAD_SEGMENTS = 1024;
    constexpr double SCREEN_SPACE_MIN_STROKE_TO_VIEW_RATIO = 2.0; // Smaller strokes are left to Skia's normal transformed draw
    constexpr double SCREEN_SPACE_VIEW_MARGIN = 4.0;

    struct ScreenRect {
        Vector2d min;
        Vector2d max;
        bool overlaps(const Vector2d& a, const Vector2d& b, const Vector2d& c) const {
            return std::max({a.x(), b.x(), c.x()}) >= min.x() && std::min({a.x(), b.x(), c.x()}) <= max.x() &&
                   std::max({a.y(), b.y(), c.y()}) >= min.y() && std::min({a.y(), b.y(), c.y()}) <= max.y();
        }
        bool contains(const Vector2d& a, const Vector2d& b, const Vector2d& c) const {
            return std::min({a.x(), b.x(), c.x()}) >= min.x() && std::max({a.x(), b.x(), c.x()}) <= max.x() &&
                   std::min({a.y(), b.y(), c.y()}) >= min.y() && std::max({a.y(), b.y(), c.y()}) <= max.y();
        }
    };

    // Appends the quad's points after p0
    void flatten_quad_in_view(std::vector<Vector2d>& out, const ScreenRect& view, const Vector2d& p0, const Vector2d& p1, const Vector2d& p2, int depth) {
        if(!view.overlaps(p0, p1, p2)) {
            out.emplace_back(p2);
            return;
        }
        double deviation = (p0 - 2.0 * p1 + p2).norm() * 0.25; // Max distance between the curve and its chord
        if(!(deviation > SCREEN_SPACE_TOLERANCE) || depth >= MAX_QUAD_SUBDIVISION_DEPTH) {
            out.emplace_back(p2);
            return;
        }
        if(view.contains(p0, p1, p2)) {
            // Uniform steps: each piece deviates by deviation / segments^2
            int segments = std::min(static_cast<int>(std::ceil(std::sqrt(deviation / SCREEN_SPACE_TOLERANCE))), MAX_UNIFORM_QUAD_SEGMENTS);
            for(int i = 1; i <= segments; i++) {
                double t = static_cast<double>(i) / segments;
                double u = 1.0 - t;
                out.emplace_back(p0 * (u * u) + p1 * (2.0 * u * t) + p2 * (t * t));
            }
            return;
        }
        Vector2d p01 = (p0 + p1) * 0.5;
        Vector2d p12 = (p1 + p2) * 0.5;
        Vector2d mid = (p01 + p12) * 0.5;
        flatten_quad_in_view(out, view, p0, p01, mid, depth + 1);
        flatten_quad_in_view(out, view, mid, p12, p2, depth + 1);
    }

    // One Sutherland-Hodgman pass, keeping the side of the line coordinate[axis] == bound given by keepGreater
    void clip_polygon_axis(std::vector<Vector2d>& poly, std::vector<Vector2d>& scratch, int axis, double bound, bool keepGreater) {
        scratch.clear();
        auto inside = [&](const Vector2d& p) { return keepGreater ? p[axis] >= bound : p[axis] <= bound; };
        for(size_t i = 0; i < poly.size(); i++) {
            const Vector2d& prev = poly[(i + poly.size() - 1) % poly.size()];
            const Vector2d& cur = poly[i];
            bool prevIn = inside(prev);
            bool curIn = inside(cur);
            if(prevIn != curIn) {
                double t = (bound - prev[axis]) / (cur[axis] - prev[axis]);
                Vector2d hit = prev + (cur - prev) * t;
                hit[axis] = bound;
                scratch.emplace_back(hit);
            }
            if(curIn)
                scratch.emplace_back(cur);
        }
        poly.swap(scratch);
    }
}

std::shared_ptr<void> MeshCanvasComponent::get_screen_space_path(const DrawData& drawData, const CoordSpaceHelper& coords, bool force) const {
    try {
        const CoordSpaceHelper& cam = drawData.cam.c;
        // Camera space is the view itself: (0, 0) to viewingArea
        ScreenRect view{Vector2d::Constant(-SCREEN_SPACE_VIEW_MARGIN), drawData.cam.viewingArea.cast<double>() + Vector2d::Constant(SCREEN_SPACE_VIEW_MARGIN)};

        double scale = static_cast<double>(WorldMultiplier(coords.inverseScale) / WorldMultiplier(cam.inverseScale));
        if(!force) {
            SkRect bounds = d.meshPath.getBounds();
            double strokeScreenSize = std::max(bounds.width(), bounds.height()) * scale;
            double viewSize = std::max(view.max.x() - view.min.x(), view.max.y() - view.min.y());
            if(strokeScreenSize < viewSize * SCREEN_SPACE_MIN_STROKE_TO_VIEW_RATIO)
                return nullptr;
        }

        // An object-space point near the view centre is mapped to camera space exactly (fixed point), and everything else is mapped
        // relative to it in doubles, so precision near the view holds at any zoom
        Vector2f anchor = coords.to_space(cam.from_space(((view.min + view.max) * 0.5).cast<float>()));
        Vector2d anchorCam = Rotation2D<double>(-cam.rotation) * ((coords.from_space(anchor) - cam.pos) / cam.inverseScale).cast<double>();
        Matrix2d objToCam = Rotation2D<double>(coords.rotation - cam.rotation).toRotationMatrix() * scale;
        auto to_cam = [&](const SkPoint& p) -> Vector2d {
            return objToCam * Vector2d{static_cast<double>(p.x()) - anchor.x(), static_cast<double>(p.y()) - anchor.y()} + anchorCam;
        };

        SkPath smoothPath = get_smooth_path();
        SkPathBuilder builder(smoothPath.getFillType());
        std::vector<Vector2d> contour;
        std::vector<Vector2d> scratch;
        auto flush_contour = [&] {
            if(contour.size() >= 3) {
                clip_polygon_axis(contour, scratch, 0, view.min.x(), true);
                clip_polygon_axis(contour, scratch, 0, view.max.x(), false);
                clip_polygon_axis(contour, scratch, 1, view.min.y(), true);
                clip_polygon_axis(contour, scratch, 1, view.max.y(), false);
                if(contour.size() >= 3) {
                    builder.moveTo(contour[0].x(), contour[0].y());
                    for(size_t i = 1; i < contour.size(); i++)
                        builder.lineTo(contour[i].x(), contour[i].y());
                    builder.close();
                }
            }
            contour.clear();
        };

        SkPath::Iter iter(smoothPath, true);
        for(;;) {
            std::optional<SkPath::IterRec> rec = iter.next();
            if(!rec.has_value())
                break;
            const SkPoint* p = rec->fPoints.data();
            switch(rec->fVerb) {
                case SkPathVerb::kMove:
                    flush_contour();
                    contour.emplace_back(to_cam(p[0]));
                    break;
                case SkPathVerb::kLine:
                    contour.emplace_back(to_cam(p[1]));
                    break;
                case SkPathVerb::kQuad: {
                    Vector2d start = contour.back();
                    flatten_quad_in_view(contour, view, start, to_cam(p[1]), to_cam(p[2]), 0);
                    break;
                }
                case SkPathVerb::kClose:
                    flush_contour();
                    break;
                default:
                    throw std::runtime_error("[get_screen_space_path] Illegal verb " + std::to_string(static_cast<unsigned>(rec->fVerb)));
            }
        }
        flush_contour();
        return std::make_shared<SkPath>(builder.detach());
    }
    catch(...) {
        return force ? std::make_shared<SkPath>() : nullptr;
    }
}

std::shared_ptr<void> MeshCanvasComponent::get_predraw_data(const DrawData& drawData) const {
    return get_screen_space_path(drawData, compContainer->coords, false);
}

std::shared_ptr<void> MeshCanvasComponent::get_predraw_data_accurate(const DrawData& drawData, const CoordSpaceHelper& coords) const {
    return get_screen_space_path(drawData, coords, true);
}

bool MeshCanvasComponent::accurate_draw(SkCanvas* canvas, const DrawData& drawData, const CoordSpaceHelper& coords, const std::shared_ptr<void>& predrawData) const {
    // No screen-space path means the stroke is small enough on screen for the normal transformed draw
    if(!predrawData)
        return false;
    SkPaint paint;
    paint.setColor4f(SkColor4f{d.color.x(), d.color.y(), d.color.z(), d.color.w()});
    paint.setAntiAlias(drawData.skiaAA);
    canvas->drawPath(*std::static_pointer_cast<SkPath>(predrawData), paint);
    return true;
}

void MeshCanvasComponent::set_data_from(const CanvasComponent& other) {
    auto& otherStroke = static_cast<const MeshCanvasComponent&>(other);
    d = otherStroke.d;
}

void MeshCanvasComponent::initialize_draw_data(DrawingProgram& drawP) {
}

bool MeshCanvasComponent::collides_within_coords_point(const Vector2f& checkAgainst) const {
    bool intersectsAABB = d.meshPath.getBounds().contains(checkAgainst.x(), checkAgainst.y());
    if(!intersectsAABB)
        return false;
    return d.meshPath.contains(checkAgainst.x(), checkAgainst.y());
}

bool MeshCanvasComponent::collides_within_coords_skpath(const SkPath& checkAgainst) const {
    bool intersectsAABB = d.meshPath.getBounds().intersects(checkAgainst.getBounds());
    if(!intersectsAABB)
        return false;
    std::optional<SkPath> pathIntersectCheck = Op(checkAgainst, d.meshPath, SkPathOp::kIntersect_SkPathOp);
    return pathIntersectCheck.has_value() && !pathIntersectCheck.value().isEmpty();
}

bool MeshCanvasComponent::should_draw_extra(const DrawData& drawData, const CoordSpaceHelper& coords) const {
    return true;
}

bool MeshCanvasComponent::can_erase_detail() const {
    return true;
}

std::vector<CanvasComponentContainer*> MeshCanvasComponent::attempt_split(DrawingProgram& drawP) const {
    using namespace Clipper2Lib;

    std::vector<CanvasComponentContainer*> toRet;

    if(has_pen_source()) {
        // Pieces left by the centerline eraser become separate strokes, each keeping the whole pen source and one range
        if(d.keptRanges.size() > 1) {
            for(const auto& range : d.keptRanges) {
                CanvasComponentContainer* newComp = new CanvasComponentContainer(drawP.world.netObjMan, CanvasComponentType::MESH);
                MeshCanvasComponent& mesh = static_cast<MeshCanvasComponent&>(newComp->get_comp());
                mesh.d = d;
                mesh.d.keptRanges = {range};
                newComp->coords = compContainer->coords;
                mesh.regenerate_from_pen_source();
                newComp->normalize_object_coordinates();
                toRet.emplace_back(newComp);
            }
            return toRet;
        }
        if(d.eraseOps.empty())
            return toRet;
    }

    PathsD clippingSubjects;

    BrushComponentCode::skpath_to_clipper2_pathsd(clippingSubjects, d.meshPath);

    if(clippingSubjects.size() <= 1)
        return {};

    ClipperD clipper(4);
    PolyTreeD solutionTree;

    clipper.AddSubject(clippingSubjects);
    clipper.AddClip(clippingSubjects);
    clipper.Execute(ClipType::Union, d.meshPath.getFillType() == SkPathFillType::kEvenOdd ? FillRule::EvenOdd : FillRule::NonZero, solutionTree);

    std::vector<SkPath> splitPaths;

    BrushComponentCode::sort_clipper_polytreed_into_skpaths(splitPaths, solutionTree);

    if(splitPaths.size() <= 1)
        return toRet;

    for(const SkPath& p : splitPaths) {
        CanvasComponentContainer* newComp = new CanvasComponentContainer(drawP.world.netObjMan, CanvasComponentType::MESH);
        MeshCanvasComponent& mesh = static_cast<MeshCanvasComponent&>(newComp->get_comp());
        mesh.d.color = d.color;
        mesh.d.meshPath = p;
        newComp->coords = compContainer->coords;
        mesh.simplify_paths();
        newComp->normalize_object_coordinates();
        toRet.emplace_back(newComp);
    }

    return toRet;
}

void MeshCanvasComponent::normalize_object_coordinates(CoordSpaceHelper& coords) {
    SkRect pathBounds = d.meshPath.getBounds();
    SkPoint center = pathBounds.center();
    constexpr float MAX_COORDINATE = 1000.0f;
    float maxDimScaleFactorDenominator = std::max(pathBounds.width(), pathBounds.height()) * 0.5f;
    if(maxDimScaleFactorDenominator == 0.0f) // Can't normalize, path probably empty or contains one point
        return;
    float maxDimScaleFactor = MAX_COORDINATE / maxDimScaleFactorDenominator; // Scale must be uniform. This scale factor should force the maximum coordinate = MAX_COORDINATE after transformation
    SkMatrix m = SkMatrix::I();
    m.postTranslate(-center.x(), -center.y()).postScale(maxDimScaleFactor, maxDimScaleFactor);
    std::optional<SkPath> tryTransformPath = d.meshPath.tryMakeTransform(m);
    if(tryTransformPath) {
        d.meshPath = tryTransformPath.value();
        // Keep the pen source in the same space as the outline
        for(auto& p : d.penPoints) {
            SkPoint mapped = m.mapPoint(SkPoint::Make(p.pos.x(), p.pos.y()));
            p.pos = {mapped.x(), mapped.y()};
            p.width *= maxDimScaleFactor;
        }
        for(auto& s : d.rawSamples) {
            SkPoint mapped = m.mapPoint(SkPoint::Make(s.pos.x(), s.pos.y()));
            s.pos = {mapped.x(), mapped.y()};
        }
        d.strokeParams.baseWidth *= maxDimScaleFactor;
        d.strokeParams.unitScale *= maxDimScaleFactor;
        for(auto& [s0, s1] : d.keptRanges) {
            s0 *= maxDimScaleFactor;
            s1 *= maxDimScaleFactor;
        }
        for(SkPath& eraseOp : d.eraseOps) {
            if(std::optional<SkPath> transformedOp = eraseOp.tryMakeTransform(m))
                eraseOp = transformedOp.value();
        }
        WorldVec newCenter = WorldVec{center.x(), center.y()} * coords.inverseScale;
        newCenter = rotate_world_coord(newCenter, coords.rotation);
        coords.translate(newCenter);
        coords.scale_about_double(coords.pos, maxDimScaleFactor);
    }
}

CanvasComponentEraseDetailResult MeshCanvasComponent::erase_detail(const SkPath& eraseAgainst) {
    bool intersectsAABB = d.meshPath.getBounds().intersects(eraseAgainst.getBounds());
    if(!intersectsAABB)
        return CanvasComponentEraseDetailResult::NO_CHANGE;

    if(has_pen_source()) {
        // EncxdedCanvas centerline eraser: wherever the eraser touches the ink, that stretch of the centerline is cut out, and the
        // pieces left stay smooth strokes with their own caps
        std::vector<BrushComponentCode::BrushPoint> centerline = scaled_centerline();
        std::vector<float> arcLengths = BrushComponentCode::centerline_arc_lengths(centerline);
        std::vector<std::pair<float, float>> touched = BrushComponentCode::centerline_ranges_touching(centerline, arcLengths, eraseAgainst);
        if(centerline.size() >= 2) // A graze at a single point removes nothing
            std::erase_if(touched, [&](const std::pair<float, float>& r) { return r.second - r.first <= arcLengths.back() * 1e-6f; });
        if(touched.empty())
            return CanvasComponentEraseDetailResult::NO_CHANGE;
        if(centerline.size() < 2) { // A dot
            d.meshPath = SkPath();
            return CanvasComponentEraseDetailResult::REMOVED;
        }

        std::vector<std::pair<float, float>> kept = d.keptRanges;
        if(kept.empty())
            kept.emplace_back(0.0f, arcLengths.back());
        for(auto& [t0, t1] : touched) {
            std::vector<std::pair<float, float>> next;
            for(auto& [k0, k1] : kept) {
                if(t1 <= k0 || t0 >= k1) {
                    next.emplace_back(k0, k1);
                    continue;
                }
                if(t0 > k0)
                    next.emplace_back(k0, t0);
                if(t1 < k1)
                    next.emplace_back(t1, k1);
            }
            kept = std::move(next);
        }
        // Drop crumbs much shorter than the stroke is wide
        std::erase_if(kept, [&](const std::pair<float, float>& r) {
            float width = std::max(BrushComponentCode::centerline_range(centerline, arcLengths, r.first, r.first)[0].width, 1e-6f);
            return r.second - r.first < width * 0.1f;
        });
        if(kept == d.keptRanges)
            return CanvasComponentEraseDetailResult::NO_CHANGE;
        if(kept.empty()) {
            d.meshPath = SkPath();
            return CanvasComponentEraseDetailResult::REMOVED;
        }
        d.keptRanges = kept;
        regenerate_from_pen_source(false); // Simplified when the erase is committed
        return CanvasComponentEraseDetailResult::CHANGED;
    }

    std::optional<SkPath> newPath = Op(d.meshPath, eraseAgainst, SkPathOp::kDifference_SkPathOp);
    if(newPath.has_value()) {
        if(newPath.value().isEmpty()) {
            d.meshPath = newPath.value();
            return CanvasComponentEraseDetailResult::REMOVED;
        }
        else if(!d.meshPath.getBounds().contains(newPath.value().getBounds())) // If bounds of erased path is larger than bounds of original path (can happen when difference op bugs out with big eraser paths), dont change the path
            return CanvasComponentEraseDetailResult::NO_CHANGE;
        else {
            d.meshPath = newPath.value();
            if(has_pen_source())
                d.eraseOps.emplace_back(eraseAgainst);
            return CanvasComponentEraseDetailResult::CHANGED;
        }
    }
    return CanvasComponentEraseDetailResult::NO_CHANGE;
}

void MeshCanvasComponent::simplify_paths() {
    auto newPath = Simplify(d.meshPath);
    if(newPath.has_value())
        d.meshPath = newPath.value();
}

SCollision::AABB<float> MeshCanvasComponent::get_obj_coord_bounds() const {
    return d.meshPath.getBounds();
}
