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

#pragma once
#include "../CoordSpaceHelper.hpp"
#include <include/core/SkPathBuilder.h>
#include "../InputManager.hpp"
#include <clipper2/clipper.h>

class DrawingProgram;

struct MeshShapeData {
    std::vector<std::vector<Vector2f>> contours;
    template <typename Archive> void serialize(Archive& a) {
        a(contours);
    }
};

namespace BrushComponentCode {
    constexpr float CLIPPER_SIMPLIFY_EPSILON = 0.1;
    struct BrushPoint {
        Vector2f pos;
        float width;
        template <typename Archive> void serialize(Archive& a) {
            a(pos, width);
        }
    };

    // EncxdedCanvas (Phase 3): pen input exactly as it arrived, the source of truth for brush and highlighter strokes.
    // The stroke's points are rebuilt from these with process_raw_samples.
    struct RawSample {
        Vector2f pos;   // Object space
        float pressure; // 0 to 1 (1 for mouse and touch)
        float time;     // Seconds since the stroke started
        template <typename Archive> void serialize(Archive& a) {
            a(pos, pressure, time);
        }
    };

    // The settings a stroke was drawn with. Saved with the stroke, so changing a setting later only affects new strokes.
    struct StrokeParams {
        float baseWidth = 1.0f;  // Width at full pressure, object units
        float unitScale = 1.0f;  // Object units per screen pixel when it was drawn (the stabilizer's fixed distances are in pixels)
        float stabilizer = 0.5f; // GlobalConfig::brushStrokeSmoothingFactor
        float pressureSmoothing = 0.707f;
        bool pressureAffectsWidth = true;
        float minimumSize = 0.0f;
        float pressureCurve = 0.0f;
        template <typename Archive> void serialize(Archive& a) {
            a(baseWidth, unitScale, stabilizer, pressureSmoothing, pressureAffectsWidth, minimumSize, pressureCurve);
        }
    };

    // Width multiplier (0 to 1) for a pen pressure
    float pressure_to_width_factor(float pressure, const StrokeParams& params);
    // Runs the stabilizer and pressure smoothing over the samples, giving the stroke's points. finished applies the end-of-stroke tip fix.
    std::vector<BrushPoint> process_raw_samples(const std::vector<RawSample>& samples, const StrokeParams& params, bool finished);

    // Centerline: the stroke's points joined by a Catmull-Rom curve, subdivided more where it bends
    std::vector<BrushPoint> stroke_centerline(const std::vector<BrushPoint>& points);
    // Arc length from the start of the centerline at each of its points
    std::vector<float> centerline_arc_lengths(const std::vector<BrushPoint>& centerline);
    // The centerline between arc lengths s0 and s1
    std::vector<BrushPoint> centerline_range(const std::vector<BrushPoint>& centerline, const std::vector<float>& arcLengths, float s0, float s1);
    // Arc length intervals where the ink (a disc of the local width around the centerline) touches the region
    std::vector<std::pair<float, float>> centerline_ranges_touching(const std::vector<BrushPoint>& centerline, const std::vector<float>& arcLengths, const SkPath& region);

    struct BrushStrokeGenerationData {
        bool addedTemporaryPoint = false;
        std::vector<BrushComponentCode::BrushPoint> brushPoints;
        Vector2f prevPointUnaltered = {0, 0};
        float penWidth = 1.0f;
        CoordSpaceHelper coords;
    };

    void skpath_to_clipper2_pathsd(Clipper2Lib::PathsD& clipperPath, const SkPath& skPath);
    bool clipper2_polygon_to_skpath_builder(SkPathBuilder& skPathBuilder, const Clipper2Lib::PathD& clipperPath);
    void sort_clipper_polytreed_into_skpaths(std::vector<SkPath>& paths, const Clipper2Lib::PolyTreeD& tree);
    void clipper2_polygons_to_skpath_builder(SkPathBuilder& skPathBuilder, const Clipper2Lib::PathsD& clipperPath);
    std::optional<SkPath> skpath_simplify_only_lines(const SkPath& skPath);

    SkPath brush_stroke_to_skpath(const std::vector<BrushPoint>& brushPoints, bool hasRoundCaps);

    // EncxdedCanvas smooth strokes: render a line-only polygon path as quadratic curves through its edge midpoints,
    // so it stays smooth at any zoom. Returns the input unchanged if it contains anything other than lines.
    SkPath smooth_polygon_path(const SkPath& polygonPath);
    // Converts curves back to line segments, finely enough for the given on-screen scale (pixels per path unit)
    SkPath flatten_path_for_scale(const SkPath& path, float pixelsPerUnit);
    SkPath create_triangles(const std::vector<BrushPoint>& regularPoints, const std::vector<BrushPoint>& smoothedPoints, bool hasRoundCaps);
    std::vector<size_t> get_wedge_indices(const std::vector<BrushPoint>& points);
    std::vector<BrushPoint> smooth_points(const std::vector<BrushPoint>& points, size_t beginIndex, size_t endIndex, unsigned numOfDivisions);
    void smooth_out_points(std::vector<BrushPoint>& brushPoints, float smoothFactor);
    void fix_tip(std::vector<BrushPoint>& brushPoints);
    void mouse_button(DrawingProgram& drawP, BrushStrokeGenerationData& genData, const CoordSpaceHelper& strokeCoordSpace, const InputManager::MouseButtonCallbackArgs& button, float brushSize);
    void mouse_motion(DrawingProgram& drawP, BrushStrokeGenerationData& genData, const Vector2f& motionPos, float brushSize, float brushSmoothing);
    void pen_pressure(DrawingProgram& drawP, BrushStrokeGenerationData& genData, float brushSize);
    // The stabilizer itself: adds a pointer position (in the stroke's space). unitScale converts its pixel distances.
    void add_motion_point(BrushStrokeGenerationData& genData, const Vector2f& pos, float width, float brushSmoothing, float pressureSmoothing, float unitScale);
    bool extensive_point_checking(const std::vector<BrushPoint>& points, const Vector2f& newPoint, float minimumDistance);
    bool extensive_point_checking_back(const std::vector<BrushPoint>& points, const Vector2f& newPoint, float minimumDistance);

}
