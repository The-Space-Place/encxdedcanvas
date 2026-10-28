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

#include "BrushComponentCode.hpp"
#include <Helpers/ConvertVec.hpp>
#include <Helpers/MathExtras.hpp>
#include <clipper2/clipper.h>
#include <include/core/SkPathTypes.h>
#include <algorithm>
#include <cmath>
#include "../TimePoint.hpp"
#include "../DrawingProgram/DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"

#define DEFAULT_SMOOTHNESS 3
#define MINIMUM_DISTANCE_FROM_FIRST_POINT 3.0f
#define MINIMUM_DISTANCE_TO_NEXT_POINT 0.002f

namespace BrushComponentCode {

void skpath_to_clipper2_pathsd(Clipper2Lib::PathsD& clipperPath, const SkPath& skPath) {
    SkPath::Iter iter(skPath, true);

    for(;;) {
        std::optional<SkPath::IterRec> rec = iter.next();
        if(!rec.has_value())
            break;

        switch(rec->fVerb) {
            case SkPathVerb::kClose:
                break;
            case SkPathVerb::kLine:
                clipperPath.back().emplace_back(rec->fPoints[1].x(), rec->fPoints[1].y());
                break;
            case SkPathVerb::kMove:
                clipperPath.emplace_back();
                clipperPath.back().emplace_back(rec->fPoints[0].x(), rec->fPoints[0].y());
                break;
            default:
                throw std::runtime_error("[skpath_to_clipper2_pathsd] Illegal verb " + std::to_string(static_cast<unsigned>(rec->fVerb)));
                break;
        }
    }
}

bool clipper2_polygon_to_skpath_builder(SkPathBuilder& skPathBuilder, const Clipper2Lib::PathD& clipperPath) {
    if(clipperPath.empty())
        return false;
    skPathBuilder.moveTo(clipperPath.front().x, clipperPath.front().y);
    for(size_t i = 1; i < clipperPath.size(); i++)
        skPathBuilder.lineTo(clipperPath[i].x, clipperPath[i].y);
    skPathBuilder.close();
    return true;
}

void sort_clipper_polytreed_into_skpaths_CHILD(std::vector<SkPath> &paths, const Clipper2Lib::PolyTreeD &tree) {
    using namespace Clipper2Lib;
    SkPathBuilder newPath;
    if(!clipper2_polygon_to_skpath_builder(newPath, tree.Polygon()))
        return;
    for(auto& treeChild : tree) {
        clipper2_polygon_to_skpath_builder(newPath, treeChild->Polygon());
        for(auto& childChild : *treeChild)
            sort_clipper_polytreed_into_skpaths_CHILD(paths, *childChild);
    }
    paths.emplace_back(newPath.detach());
}

void sort_clipper_polytreed_into_skpaths(std::vector<SkPath>& paths, const Clipper2Lib::PolyTreeD& tree) {
    using namespace Clipper2Lib;
    for(auto& treeChild : tree)
        sort_clipper_polytreed_into_skpaths_CHILD(paths, *treeChild);
}

void clipper2_polygons_to_skpath_builder(SkPathBuilder& skPathBuilder, const Clipper2Lib::PathsD& clipperPath) {
    for(auto& p : clipperPath)
        clipper2_polygon_to_skpath_builder(skPathBuilder, p);
}

std::optional<SkPath> skpath_simplify_only_lines(const SkPath& skPath) {
    using namespace Clipper2Lib;
    PathsD clippingSubjects;
    skpath_to_clipper2_pathsd(clippingSubjects, skPath);
    ClipperD clipper;
    PathsD solutionPaths;

    clipper.AddSubject(clippingSubjects);
    clipper.AddClip(clippingSubjects);
    if(!clipper.Execute(ClipType::Union, skPath.getFillType() == SkPathFillType::kEvenOdd ? FillRule::EvenOdd : FillRule::NonZero, solutionPaths))
        return std::nullopt;

    SimplifyPaths(solutionPaths, CLIPPER_SIMPLIFY_EPSILON);

    SkPathBuilder newPath;
    newPath.setFillType(skPath.getFillType());
    clipper2_polygons_to_skpath_builder(newPath, solutionPaths);
    return newPath.detach();
}

SkPath brush_stroke_to_skpath(const std::vector<BrushPoint>& brushPoints, bool hasRoundCaps) {
    if(brushPoints.empty())
        return SkPath();
    return create_triangles(brushPoints, stroke_centerline(brushPoints), hasRoundCaps);
}

SkPath smooth_polygon_path(const SkPath& polygonPath) {
    SkPathBuilder builder;
    builder.setFillType(polygonPath.getFillType());

    std::vector<SkPoint> contour;
    auto flush_contour = [&] {
        // Drop repeated points; they'd produce degenerate curves
        std::vector<SkPoint> deduped;
        for(const SkPoint& p : contour) {
            if(deduped.empty() || SkPoint::Distance(deduped.back(), p) >= 1e-4f)
                deduped.emplace_back(p);
        }
        contour = std::move(deduped);
        if(contour.size() >= 2 && SkPoint::Distance(contour.front(), contour.back()) < 1e-4f)
            contour.pop_back();
        if(contour.size() >= 3) {
            size_t n = contour.size();
            auto mid = [&](size_t i) { return (contour[i % n] + contour[(i + 1) % n]) * 0.5f; };
            builder.moveTo(mid(n - 1));
            for(size_t i = 0; i < n; i++)
                builder.quadTo(contour[i], mid(i));
            builder.close();
        }
        else if(!contour.empty())
            builder.addPolygon({contour.data(), contour.size()}, true);
        contour.clear();
    };

    SkPath::Iter iter(polygonPath, true);
    for(;;) {
        std::optional<SkPath::IterRec> rec = iter.next();
        if(!rec.has_value())
            break;
        switch(rec->fVerb) {
            case SkPathVerb::kMove:
                flush_contour();
                contour.emplace_back(rec->fPoints[0]);
                break;
            case SkPathVerb::kLine:
                contour.emplace_back(rec->fPoints[1]);
                break;
            case SkPathVerb::kClose:
                flush_contour();
                break;
            default:
                return polygonPath; // Already has curves
        }
    }
    flush_contour();
    return builder.detach();
}

SkPath flatten_path_for_scale(const SkPath& path, float pixelsPerUnit) {
    SkPathBuilder builder;
    builder.setFillType(path.getFillType());
    SkPath::Iter iter(path, true);
    for(;;) {
        std::optional<SkPath::IterRec> rec = iter.next();
        if(!rec.has_value())
            break;
        const SkPoint* p = rec->fPoints.data();
        switch(rec->fVerb) {
            case SkPathVerb::kMove:
                builder.moveTo(p[0]);
                break;
            case SkPathVerb::kLine:
                builder.lineTo(p[1]);
                break;
            case SkPathVerb::kQuad: {
                float lengthPx = (SkPoint::Distance(p[0], p[1]) + SkPoint::Distance(p[1], p[2])) * pixelsPerUnit;
                lengthPx = std::isfinite(lengthPx) ? std::min(lengthPx, 1e6f) : 1e6f;
                int segments = std::clamp(static_cast<int>(std::ceil(std::sqrt(lengthPx * 0.5f))), 1, 64);
                for(int i = 1; i <= segments; i++) {
                    float t = static_cast<float>(i) / segments;
                    float u = 1.0f - t;
                    builder.lineTo(p[0] * (u * u) + p[1] * (2.0f * u * t) + p[2] * (t * t));
                }
                break;
            }
            case SkPathVerb::kClose:
                builder.close();
                break;
            default:
                return path; // Only lines and quads are produced by smooth_polygon_path
        }
    }
    return builder.detach();
}

std::vector<size_t> get_wedge_indices(const std::vector<BrushPoint>& points) {
    if(points.size() < 2)
        return {};
    std::vector<size_t> toRet;
    toRet.emplace_back(0);
    for(size_t i = 1; i < points.size() - 1; i++) {
        Vector2f v1Norm = (points[i - 1].pos - points[i].pos).normalized();
        Vector2f v2Norm = (points[i + 1].pos - points[i].pos).normalized();
        if(v1Norm.dot(v2Norm) > -0.6f)
            toRet.emplace_back(i);
    }
    toRet.emplace_back(points.size() - 1);
    return toRet;
}

std::vector<BrushPoint> smooth_points(const std::vector<BrushPoint>& points, size_t beginIndex, size_t endIndex, unsigned numOfDivisions) {
    size_t pointsSize = endIndex - beginIndex + 1;
    if(pointsSize < 2) 
        return {points[beginIndex], points[endIndex]};

    std::vector<BrushPoint> toRet;

    BrushPoint p0 = points[beginIndex];
    p0.pos = p0.pos + p0.width * (p0.pos - points[beginIndex + 1].pos).normalized();
    BrushPoint p1 = points[beginIndex];
    BrushPoint p2 = points[beginIndex + 1];
    BrushPoint p3;

    for(size_t i = beginIndex; i < endIndex; i++) {

        if(i == endIndex - 1) {
            p3 = points[i + 1];
            p3.pos = p3.pos + p3.width * (p3.pos - points[i].pos).normalized();
        }
        else
            p3 = points[i + 2];

        toRet.emplace_back(p1);

        float tMove = 1.0 / static_cast<float>(numOfDivisions);
        for(unsigned d = 1; d < numOfDivisions; d++) {
            float t = tMove * static_cast<float>(d);
            BrushPoint pToAdd;
            pToAdd.pos = catmull_rom(p0.pos, p1.pos, p2.pos, p3.pos, t);
            pToAdd.width = std::lerp(p1.width, p2.width, t);
            if(!(std::isnan(pToAdd.pos.x()) || std::isnan(pToAdd.pos.y()))) // Don't add NAN values
                toRet.emplace_back(pToAdd);
        }
        p0 = p1;
        p1 = p2;
        p2 = p3;
    }
    toRet.emplace_back(points[endIndex]);

    return toRet;
}

SkPath create_triangles(const std::vector<BrushPoint>& regularPoints, const std::vector<BrushPoint>& smoothedPoints, bool hasRoundCaps) {
    SkPathBuilder pathBuilder;
    const int ARC_SMOOTHNESS = 10;
    const int CIRCLE_SMOOTHNESS = 20;
    const std::vector<BrushPoint>& pointsN = regularPoints;
    std::vector<SkPoint> topPoints;
    std::vector<SkPoint> bottomPoints;

    if(pointsN.size() == 1 && hasRoundCaps) {
        std::vector<Vector2f> circlePoints = gen_circle_points(pointsN.front().pos, pointsN.front().width / 2.0f, CIRCLE_SMOOTHNESS);
        for(size_t i = 0; i < circlePoints.size(); i++) {
            topPoints.emplace_back(convert_vec2<SkPoint>(circlePoints[i]));
        }
    }

    if(pointsN.size() < 2) {
        if(!topPoints.empty()) {
            pathBuilder.addPolygon({topPoints.data(), topPoints.size()}, true);
            return pathBuilder.detach();
        }
        return SkPath();
    }

    auto& points = smoothedPoints;

    std::vector<size_t> wedgeIndices = get_wedge_indices(points);
    for(size_t i = 0; i < wedgeIndices.size() - 1; i++) {

        size_t pointsBegin = wedgeIndices[i];
        size_t pointsEnd = wedgeIndices[i + 1];

        std::array<Vector2f, 3> vertArray;
        size_t newIndex = 0;

        Vector2f perpPrev = perpendicular_vec2((points[pointsBegin + 1].pos - points[pointsBegin].pos).eval());
        perpPrev.normalize();

        vertArray[0] = points[pointsBegin].pos + perpPrev * points[pointsBegin].width * 0.5f;
        topPoints.emplace_back(convert_vec2<SkPoint>(vertArray[0]));
        vertArray[1] = points[pointsBegin].pos - perpPrev * points[pointsBegin].width * 0.5f;
        bottomPoints.emplace_back(convert_vec2<SkPoint>(vertArray[1]));
        newIndex = 2;

        if(hasRoundCaps && pointsBegin == 0) {
            Vector2f arcDirStart = (convert_vec2<Vector2f>(vertArray[0]) - points[0].pos).normalized();
            Vector2f arcDirEnd = (convert_vec2<Vector2f>(vertArray[1]) - points[0].pos).normalized();
            Vector2f arcDirCenter = (points[0].pos - points[1].pos).normalized();
            std::vector<Vector2f> arcPoints = arc_vec<float>(points[0].pos, arcDirStart, arcDirEnd, arcDirCenter, points[0].width * 0.5f, ARC_SMOOTHNESS);
            arcPoints.emplace(arcPoints.begin(), convert_vec2<Vector2f>(vertArray[0]));
            arcPoints.emplace_back(convert_vec2<Vector2f>(vertArray[1]));
            for(size_t i = 0; i < arcPoints.size() - 1; i++) {
                bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
            }
            bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
        }

        for(size_t j = pointsBegin + 1; j < pointsEnd; j++) {
            if(j >= pointsEnd)
                j = pointsEnd - 1;

            Vector2f v1 = points[j - 1].pos - points[j].pos;
            Vector2f v2 = points[j].pos - points[j + 1].pos;
            Vector2f perp = perpendicular_vec2((v1 + v2).eval()).normalized();
            if(std::isnan(perp.dot(perpPrev))) {
                std::cout << "NAN FOUND" << std::endl;
                std::cout << v1 << std::endl;
                std::cout << v2 << std::endl;
                std::cout << perp << std::endl;
                std::cout << perpPrev << std::endl;
                throw 1;
            }
            if(perp.dot(perpPrev) < 0.0)
                perp = -perp;

            vertArray[newIndex] = points[j].pos + perp * 0.5f * points[j].width;
            topPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
            newIndex = (newIndex + 1) % 3;
            
            vertArray[newIndex] = points[j].pos - perp * 0.5f * points[j].width;
            bottomPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
            newIndex = (newIndex + 1) % 3;

            perpPrev = perp;
        }

        Vector2f rectDir = points[pointsEnd - 1].pos - points[pointsEnd].pos;
        rectDir.normalize();
        Vector2f perp = perpendicular_vec2(rectDir);
        if(perp.dot(perpPrev) < 0.0)
            perp = -perp;
        vertArray[newIndex] = points[pointsEnd].pos + perp * points[pointsEnd].width * 0.5;
        topPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
        newIndex = (newIndex + 1) % 3;

        vertArray[newIndex] = points[pointsEnd].pos - perp * points[pointsEnd].width * 0.5;
        bottomPoints.emplace_back(convert_vec2<SkPoint>(vertArray[newIndex]));
        newIndex = (newIndex + 1) % 3;

        // Render wedge
        if(pointsEnd != points.size() - 1) {
            // Find 4 points involved in intersection
            Vector2f currentP1 = points[pointsEnd].pos - perp * points[pointsEnd].width * 0.5;
            Vector2f currentP2 = points[pointsEnd].pos + perp * points[pointsEnd].width * 0.5;
            Vector2f nextRectDir = points[pointsEnd + 1].pos - points[pointsEnd].pos;
            nextRectDir.normalize();
            Vector2f nextPerp = perpendicular_vec2(nextRectDir);
            Vector2f nextP1 = points[pointsEnd].pos - nextPerp * points[pointsEnd].width * 0.5;
            Vector2f nextP2 = points[pointsEnd].pos + nextPerp * points[pointsEnd].width * 0.5;

            // Find center of wedge
            Vector2f wedgeCenter = line_line_intersection(currentP1, currentP2, nextP1, nextP2);

            Vector2f wedgeP1 = ((currentP1 - currentP2).dot(nextRectDir)) <= 0.0 ? currentP1 : currentP2;
            Vector2f wedgeP2 = ((nextP1 - nextP2).dot(rectDir)) <= 0.0 ? nextP1 : nextP2;
            bool isTopWedge = wedgeP1 == convert_vec2<Vector2f>(topPoints.back());

            float arcRadius;

            std::vector<Vector2f> arcPoints;

            if(wedgeCenter.x() == std::numeric_limits<float>::max()) {
                wedgeCenter = (currentP1 + currentP2) * 0.5;
                wedgeP1 = currentP1;
                wedgeP2 = currentP2;
                arcRadius = points[pointsEnd].width * 0.5;
                arcPoints = arc_vec(wedgeCenter, (wedgeP1 - wedgeCenter).normalized(), (wedgeP2 - wedgeCenter).normalized(), (-rectDir).eval(), arcRadius, ARC_SMOOTHNESS);
            }
            else {
                arcRadius = (wedgeCenter - wedgeP1).norm();
                arcPoints = arc_vec(wedgeCenter, (wedgeP1 - wedgeCenter).normalized(), (wedgeP2 - wedgeCenter).normalized(), lerp_vec((-rectDir).eval(), (-nextRectDir).eval(), 0.5).normalized(), arcRadius, ARC_SMOOTHNESS);
            }

            arcPoints.emplace(arcPoints.begin(), wedgeP1);
            arcPoints.emplace_back(wedgeP2);
            for(size_t i = 0; i < arcPoints.size() - 1; i++) {
                if(isTopWedge)
                    topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
                else
                    bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
            }
            if(isTopWedge)
                topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
            else
                bottomPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
        }
        else if(hasRoundCaps) { // It's the last point, cap it off
            Vector2f arcDirStart = (convert_vec2<Vector2f>(vertArray[(newIndex + 1) % 3]) - points.back().pos).normalized();
            Vector2f arcDirEnd = (convert_vec2<Vector2f>(vertArray[(newIndex + 2) % 3]) - points.back().pos).normalized();
            Vector2f arcDirCenter = -rectDir;
            std::vector<Vector2f> arcPoints = arc_vec<float>(points.back().pos, arcDirStart, arcDirEnd, arcDirCenter, points.back().width * 0.5f, ARC_SMOOTHNESS);
            arcPoints.emplace(arcPoints.begin(), convert_vec2<Vector2f>(vertArray[(newIndex + 1) % 3]));
            arcPoints.emplace_back(convert_vec2<Vector2f>(vertArray[(newIndex + 2) % 3]));
            for(size_t i = 0; i < arcPoints.size() - 1; i++) {
                topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints[i]));
            }
            topPoints.emplace_back(convert_vec2<SkPoint>(arcPoints.back()));
        }
    }

    if(!topPoints.empty()) {
        topPoints.insert(topPoints.begin(), bottomPoints.rbegin(), bottomPoints.rend());
        pathBuilder.addPolygon({topPoints.data(), topPoints.size()}, true);
    }

    return pathBuilder.detach();
}

void smooth_out_points(std::vector<BrushPoint>& brushPoints, float smoothFactor) {
    if(brushPoints.size() >= 2) {
        brushPoints.back().width = std::max(brushPoints[brushPoints.size() - 1].width, brushPoints[brushPoints.size() - 2].width * smoothFactor);
        for(size_t i = brushPoints.size() - 1; i > 0; i--) {
            if(brushPoints[i].width * smoothFactor > brushPoints[i - 1].width)
                brushPoints[i - 1].width = brushPoints[i].width * smoothFactor;
            else
                break;
        }
    }
}

void fix_tip(std::vector<BrushPoint>& brushPoints) {
    if(brushPoints.size() >= 2)
        brushPoints[brushPoints.size() - 2].width = brushPoints[brushPoints.size() - 1].width = std::max(brushPoints[brushPoints.size() - 1].width, brushPoints[brushPoints.size() - 2].width);
}

void mouse_button(DrawingProgram& drawP, BrushStrokeGenerationData& genData, const CoordSpaceHelper& strokeCoordSpace, const InputManager::MouseButtonCallbackArgs& button, float brushSize) {
    if(button.deviceType == InputManager::MouseDeviceType::PEN && drawP.world.main.conf.tabletOptions.pressureAffectsBrushWidth) {
        genData.penWidth = drawP.world.main.input.pen.pressure;
        if(genData.penWidth != 0.0f) {
            float brushMinSize = drawP.world.main.conf.tabletOptions.brushMinimumSize;
            genData.penWidth = brushMinSize + genData.penWidth * (1.0f - brushMinSize);
        }
    }
    else
        genData.penWidth = 1.0f;

    float width = brushSize * genData.penWidth;
    genData.coords = strokeCoordSpace;

    genData.brushPoints.clear();
    BrushComponentCode::BrushPoint p;
    p.pos = button.pos;
    p.width = width;
    genData.prevPointUnaltered = p.pos;
    genData.brushPoints.emplace_back(p);
    genData.addedTemporaryPoint = false;
}

void mouse_motion(DrawingProgram& drawP, BrushStrokeGenerationData& genData, const Vector2f& motionPos, float brushSize, float brushSmoothing) {
    add_motion_point(genData, genData.coords.to_space(drawP.world.drawData.cam.c.from_space(motionPos)), brushSize * genData.penWidth, brushSmoothing, drawP.world.main.conf.tabletOptions.brushPressureSmoothingFactor, 1.0f);
}

void add_motion_point(BrushStrokeGenerationData& genData, const Vector2f& pos, float width, float brushSmoothing, float pressureSmoothing, float unitScale) {
    BrushComponentCode::BrushPoint p;
    p.pos = pos;
    p.width = width;
    float minimumDistanceToNext = MINIMUM_DISTANCE_TO_NEXT_POINT * unitScale;

    // Temporary point is a point that follows the cursor until it is placed
    if(!genData.addedTemporaryPoint) {
        // Temporary point is close enough to be placed
        if(extensive_point_checking(genData.brushPoints, p.pos, genData.brushPoints.size() == 1 ? MINIMUM_DISTANCE_FROM_FIRST_POINT * unitScale : minimumDistanceToNext)) {
            genData.brushPoints.emplace_back(p);
            genData.addedTemporaryPoint = true;
        }
        else
            // Temporary point not close enough yet, so change the width of the previous point based on the current pen width until it can be placed
            genData.brushPoints.back().width = std::max(genData.brushPoints.back().width, p.width);
    }

    if(genData.addedTemporaryPoint) {
        const BrushComponentCode::BrushPoint& prevP = genData.brushPoints[genData.brushPoints.size() - 2];
        float distToPrev = (p.pos - prevP.pos).norm();
        // If the cursor isnt too close to the previous points, move the temporary point to the cursor and change the width of the temporary point to the current pen width
        if(extensive_point_checking_back(genData.brushPoints, p.pos, minimumDistanceToNext)) {
            genData.brushPoints.back().pos = p.pos;
            genData.brushPoints.back().width = std::max(genData.brushPoints.back().width, p.width);
        }

        float maxWidthToCompareTo = std::max(genData.brushPoints.back().width, p.width);
        maxWidthToCompareTo = std::max(maxWidthToCompareTo, 5.0f * unitScale);
        if(genData.brushPoints.size() >= 2)
            maxWidthToCompareTo = std::max(genData.brushPoints[genData.brushPoints.size() - 2].width, maxWidthToCompareTo);
        if(genData.brushPoints.size() >= 3)
            maxWidthToCompareTo = std::max(genData.brushPoints[genData.brushPoints.size() - 3].width, maxWidthToCompareTo);

        // If the temporary point (cursor) is far enough from the point before it, make the temporary point permanent
        if(distToPrev >= maxWidthToCompareTo * brushSmoothing) {
            genData.brushPoints.back().pos = p.pos;
            genData.brushPoints.back().width = std::max(genData.brushPoints.back().width, p.width);
            genData.addedTemporaryPoint = false;

            // Midway interpolation moves the point before the last one to a position that leads to a smoother line
            if(genData.brushPoints.size() != 2) // Don't interpolate the first point
                genData.brushPoints[genData.brushPoints.size() - 2].pos = (genData.prevPointUnaltered + p.pos) * 0.5;
            genData.prevPointUnaltered = p.pos;
        }
    }

    smooth_out_points(genData.brushPoints, pressureSmoothing);
}

void pen_pressure(DrawingProgram& drawP, BrushStrokeGenerationData& genData, float brushSize) {
    if(drawP.world.main.conf.tabletOptions.pressureAffectsBrushWidth) {
        if(genData.penWidth != 0.0f) {
            float brushMinSize = drawP.world.main.conf.tabletOptions.brushMinimumSize;
            genData.penWidth = brushMinSize + genData.penWidth * (1.0f - brushMinSize);
            float width = brushSize * genData.penWidth;
            genData.brushPoints.back().width = std::max(genData.brushPoints.back().width, width);
            smooth_out_points(genData.brushPoints, drawP.world.main.conf.tabletOptions.brushPressureSmoothingFactor);
        }
    }
}

bool extensive_point_checking(const std::vector<BrushPoint>& points, const Vector2f& newPoint, float minimumDistance) {
    if(points.size() >= 1 && (newPoint - points[points.size() - 1].pos).norm() < minimumDistance)
        return false;
    if(points.size() >= 2 && (newPoint - points[points.size() - 2].pos).norm() < minimumDistance)
        return false;
    if(points.size() >= 3 && (newPoint - points[points.size() - 3].pos).norm() < minimumDistance)
        return false;
    return true;
}

bool extensive_point_checking_back(const std::vector<BrushPoint>& points, const Vector2f& newPoint, float minimumDistance) {
    if(points.size() >= 2 && (newPoint - points[points.size() - 2].pos).norm() < minimumDistance)
        return false;
    if(points.size() >= 3 && (newPoint - points[points.size() - 3].pos).norm() < minimumDistance)
        return false;
    if(points.size() >= 4 && (newPoint - points[points.size() - 4].pos).norm() < minimumDistance)
        return false;
    return true;
}

float pressure_to_width_factor(float pressure, const StrokeParams& params) {
    if(!params.pressureAffectsWidth)
        return 1.0f;
    if(pressure == 0.0f) // Same as the live input path: no width until there's pressure
        return 0.0f;
    float curved = std::pow(std::clamp(pressure, 0.0f, 1.0f), std::exp2(-params.pressureCurve)); // Soft (positive) widens light strokes
    return params.minimumSize + curved * (1.0f - params.minimumSize);
}

std::vector<BrushPoint> process_raw_samples(const std::vector<RawSample>& samples, const StrokeParams& params, bool finished) {
    if(samples.empty())
        return {};
    BrushStrokeGenerationData g;
    g.penWidth = pressure_to_width_factor(samples[0].pressure, params);
    g.brushPoints.push_back({samples[0].pos, params.baseWidth * g.penWidth});
    g.prevPointUnaltered = samples[0].pos;
    for(size_t i = 1; i < samples.size(); i++) {
        g.penWidth = pressure_to_width_factor(samples[i].pressure, params);
        float width = params.baseWidth * g.penWidth;
        if(samples[i].pos == samples[i - 1].pos) { // Pressure change only
            if(g.penWidth != 0.0f) {
                g.brushPoints.back().width = std::max(g.brushPoints.back().width, width);
                smooth_out_points(g.brushPoints, params.pressureSmoothing);
            }
        }
        else
            add_motion_point(g, samples[i].pos, width, params.stabilizer, params.pressureSmoothing, params.unitScale);
    }
    if(finished)
        fix_tip(g.brushPoints);
    return g.brushPoints;
}

std::vector<BrushPoint> stroke_centerline(const std::vector<BrushPoint>& points) {
    constexpr unsigned MIN_DIVISIONS = DEFAULT_SMOOTHNESS;
    constexpr unsigned MAX_DIVISIONS = 32;
    constexpr float RADIANS_PER_DIVISION = 0.08f; // About 4.5 degrees of turning per piece
    if(points.size() < 2)
        return points;

    std::vector<BrushPoint> toRet;
    // The ends are extended past the first and last points, as in smooth_points
    auto point_at = [&](ptrdiff_t i) -> Vector2f {
        if(i < 0)
            return points[0].pos + points[0].width * (points[0].pos - points[1].pos).normalized();
        if(i >= static_cast<ptrdiff_t>(points.size())) {
            const BrushPoint& last = points.back();
            return last.pos + last.width * (last.pos - points[points.size() - 2].pos).normalized();
        }
        return points[i].pos;
    };
    for(size_t i = 0; i + 1 < points.size(); i++) {
        Vector2f p0 = point_at(static_cast<ptrdiff_t>(i) - 1);
        const Vector2f& p1 = points[i].pos;
        const Vector2f& p2 = points[i + 1].pos;
        Vector2f p3 = point_at(static_cast<ptrdiff_t>(i) + 2);
        toRet.emplace_back(points[i]);

        // Turning between the tangents at both ends decides how finely this piece is cut
        Vector2f t1 = (p2 - p0).normalized();
        Vector2f t2 = (p3 - p1).normalized();
        float turn = std::acos(std::clamp(t1.dot(t2), -1.0f, 1.0f));
        unsigned divisions = MIN_DIVISIONS;
        if(std::isfinite(turn))
            divisions = std::clamp(MIN_DIVISIONS + static_cast<unsigned>(std::ceil(turn / RADIANS_PER_DIVISION)), MIN_DIVISIONS, MAX_DIVISIONS);

        for(unsigned d = 1; d < divisions; d++) {
            float t = static_cast<float>(d) / static_cast<float>(divisions);
            BrushPoint p;
            p.pos = catmull_rom(p0, p1, p2, p3, t);
            p.width = std::lerp(points[i].width, points[i + 1].width, t);
            if(std::isfinite(p.pos.x()) && std::isfinite(p.pos.y())) // Don't add NAN values
                toRet.emplace_back(p);
        }
    }
    toRet.emplace_back(points.back());
    return toRet;
}

std::vector<float> centerline_arc_lengths(const std::vector<BrushPoint>& centerline) {
    std::vector<float> s(centerline.size(), 0.0f);
    for(size_t i = 1; i < centerline.size(); i++)
        s[i] = s[i - 1] + (centerline[i].pos - centerline[i - 1].pos).norm();
    return s;
}

namespace {
    BrushPoint centerline_point_at(const std::vector<BrushPoint>& centerline, const std::vector<float>& arcLengths, float s) {
        if(s <= arcLengths.front())
            return centerline.front();
        if(s >= arcLengths.back())
            return centerline.back();
        size_t i = static_cast<size_t>(std::upper_bound(arcLengths.begin(), arcLengths.end(), s) - arcLengths.begin());
        float segment = arcLengths[i] - arcLengths[i - 1];
        float t = segment > 0.0f ? (s - arcLengths[i - 1]) / segment : 0.0f;
        BrushPoint p;
        p.pos = lerp_vec(centerline[i - 1].pos, centerline[i].pos, t);
        p.width = std::lerp(centerline[i - 1].width, centerline[i].width, t);
        return p;
    }

    float distance_squared_to_segment(const Vector2f& p, const Vector2f& a, const Vector2f& b) {
        Vector2f ab = b - a;
        float len2 = ab.squaredNorm();
        float t = len2 > 0.0f ? std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
        return (a + ab * t - p).squaredNorm();
    }
}

std::vector<BrushPoint> centerline_range(const std::vector<BrushPoint>& centerline, const std::vector<float>& arcLengths, float s0, float s1) {
    std::vector<BrushPoint> toRet;
    if(centerline.empty())
        return toRet;
    auto add = [&](const BrushPoint& p) {
        // Repeated points would make degenerate joins
        if(toRet.empty() || (p.pos - toRet.back().pos).squaredNorm() > 1e-12f)
            toRet.emplace_back(p);
    };
    add(centerline_point_at(centerline, arcLengths, s0));
    for(size_t i = 0; i < centerline.size(); i++) {
        if(arcLengths[i] > s0 && arcLengths[i] < s1)
            add(centerline[i]);
    }
    add(centerline_point_at(centerline, arcLengths, s1));
    return toRet;
}

std::vector<std::pair<float, float>> centerline_ranges_touching(const std::vector<BrushPoint>& centerline, const std::vector<float>& arcLengths, const SkPath& region) {
    std::vector<std::pair<float, float>> toRet;
    if(centerline.empty())
        return toRet;

    std::vector<std::pair<Vector2f, Vector2f>> edges;
    {
        SkPath::Iter iter(region, true);
        Vector2f contourStart{0.0f, 0.0f};
        Vector2f last{0.0f, 0.0f};
        for(;;) {
            std::optional<SkPath::IterRec> rec = iter.next();
            if(!rec.has_value())
                break;
            const SkPoint* p = rec->fPoints.data();
            switch(rec->fVerb) {
                case SkPathVerb::kMove:
                    contourStart = last = {p[0].x(), p[0].y()};
                    break;
                case SkPathVerb::kLine:
                    edges.emplace_back(last, Vector2f{p[1].x(), p[1].y()});
                    last = {p[1].x(), p[1].y()};
                    break;
                case SkPathVerb::kQuad:
                case SkPathVerb::kConic:
                    edges.emplace_back(last, Vector2f{p[2].x(), p[2].y()});
                    last = {p[2].x(), p[2].y()};
                    break;
                case SkPathVerb::kCubic:
                    edges.emplace_back(last, Vector2f{p[3].x(), p[3].y()});
                    last = {p[3].x(), p[3].y()};
                    break;
                case SkPathVerb::kClose:
                    edges.emplace_back(last, contourStart);
                    last = contourStart;
                    break;
            }
        }
    }
    SkRect bounds = region.getBounds();
    auto touches = [&](const BrushPoint& p) {
        float r = p.width * 0.5f;
        if(p.pos.x() < bounds.left() - r || p.pos.x() > bounds.right() + r || p.pos.y() < bounds.top() - r || p.pos.y() > bounds.bottom() + r)
            return false;
        if(region.contains(p.pos.x(), p.pos.y()))
            return true;
        float r2 = r * r;
        for(auto& [a, b] : edges) {
            if(distance_squared_to_segment(p.pos, a, b) <= r2)
                return true;
        }
        return false;
    };

    if(centerline.size() == 1) {
        if(touches(centerline[0]))
            toRet.emplace_back(0.0f, 0.0f);
        return toRet;
    }

    // Walk the centerline in steps of a quarter of the local width, then find each boundary by bisection
    auto boundary = [&](float inside, float outside) {
        for(int k = 0; k < 8; k++) {
            float mid = (inside + outside) * 0.5f;
            if(touches(centerline_point_at(centerline, arcLengths, mid)))
                inside = mid;
            else
                outside = mid;
        }
        return inside;
    };
    float total = arcLengths.back();
    bool prevTouching = touches(centerline.front());
    float prevS = 0.0f;
    float start = 0.0f;
    for(size_t i = 1; i < centerline.size(); i++) {
        float segment = arcLengths[i] - arcLengths[i - 1];
        float step = std::max(std::min(centerline[i - 1].width, centerline[i].width) * 0.25f, total * 1e-4f);
        int steps = std::clamp(static_cast<int>(std::ceil(segment / std::max(step, 1e-9f))), 1, 4096);
        for(int k = 1; k <= steps; k++) {
            float s = arcLengths[i - 1] + segment * k / steps;
            bool touching = touches(centerline_point_at(centerline, arcLengths, s));
            if(touching && !prevTouching)
                start = boundary(s, prevS);
            else if(!touching && prevTouching)
                toRet.emplace_back(start, boundary(prevS, s));
            prevTouching = touching;
            prevS = s;
        }
    }
    if(prevTouching)
        toRet.emplace_back(start, total);
    return toRet;
}

}
