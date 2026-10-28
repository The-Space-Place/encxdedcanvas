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
#include "BrushComponentCode.hpp"
#include "CanvasComponent.hpp"
#include "Helpers/SCollision.hpp"
#include <Helpers/Serializers.hpp>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <mutex>

class MeshCanvasComponent : public CanvasComponent {
    public:
        virtual CanvasComponentType get_type() const override;
        virtual void save(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load(cereal::PortableBinaryInputArchive& a) override;
        virtual void save_file(cereal::PortableBinaryOutputArchive& a) const override;
        virtual void load_file(cereal::PortableBinaryInputArchive& a, VersionNumber version) override;
        virtual std::unique_ptr<CanvasComponent> get_data_copy() const override;
        virtual void change_stroke_color(const Vector4f& newStrokeColor) override;
        virtual std::vector<CanvasComponentContainer*> attempt_split(DrawingProgram& drawP) const override;
        virtual std::optional<Vector4f> get_stroke_color() const override;
        virtual void set_data_from(const CanvasComponent& other) override;
        virtual void simplify_paths() override;
        virtual void normalize_object_coordinates(CoordSpaceHelper& coords) override;

        // EncxdedCanvas: multiplies the pen width along the whole stroke, as if drawn with a bigger/smaller pen.
        // Purely multiplicative, so shrinking then growing restores the stroke. No-op without a pen source.
        bool scale_stroke_thickness(float factor);

        struct Data {
            SkPath meshPath;
            Vector4f color;
            // EncxdedCanvas: the pen path the outline was generated from (in the same space as meshPath), so the
            // stroke can be regenerated with a different pen size. Empty for InfiniPaint 0.6+ strokes and their split pieces.
            // For strokes with raw samples it's derived from them (and not saved).
            std::vector<BrushComponentCode::BrushPoint> penPoints;
            // EncxdedCanvas (Phase 3): raw pen input and the settings it was drawn with. Empty for line tool strokes and older files.
            std::vector<BrushComponentCode::RawSample> rawSamples;
            BrushComponentCode::StrokeParams strokeParams;
            bool penRoundCaps = true;
            float penWidthScale = 1.0f; // Thickness changes after drawing, applied on top of the pen widths
            // Arc length intervals of the centerline that are still visible after centerline erasing. Empty means all of it.
            std::vector<std::pair<float, float>> keptRanges;
            std::vector<SkPath> eraseOps; // Outline cuts from the older detail eraser (EncxdedCanvas 1.0 files), re-applied after regenerating
        } d;

        bool has_pen_source() const;
        // Regenerates meshPath from the pen source (plus erase cuts). Returns false if there's no pen source.
        bool regenerate_from_pen_source(bool simplify = true);
        // Rebuilds penPoints from the raw samples, if there are any
        void process_raw_samples();
    private:
        virtual void draw(SkCanvas* canvas, const DrawData& drawData, const std::shared_ptr<void>& predrawData) const override;
        virtual bool accurate_draw(SkCanvas* canvas, const DrawData& drawData, const CoordSpaceHelper& coords, const std::shared_ptr<void>& predrawData) const override;
        virtual std::shared_ptr<void> get_predraw_data(const DrawData& drawData) const override;
        virtual std::shared_ptr<void> get_predraw_data_accurate(const DrawData& drawData, const CoordSpaceHelper& coords) const override;
        // EncxdedCanvas: builds the visible part of the smooth path directly in camera space (see .cpp). Returns nullptr when the
        // stroke is small enough on screen to draw normally, unless force is set.
        std::shared_ptr<void> get_screen_space_path(const DrawData& drawData, const CoordSpaceHelper& coords, bool force) const;
        virtual void initialize_draw_data(DrawingProgram& drawP) override;
        virtual bool collides_within_coords_point(const Vector2f& checkAgainst) const override;
        virtual bool collides_within_coords_skpath(const SkPath& checkAgainst) const override;
        virtual bool can_erase_detail() const override;
        virtual CanvasComponentEraseDetailResult erase_detail(const SkPath& eraseAgainst) override;
        bool should_draw_extra(const DrawData& drawData, const CoordSpaceHelper& coords) const override;
        virtual SCollision::AABB<float> get_obj_coord_bounds() const override;
        SCollision::BVHContainer<float> collisionTree;
        // Centerline with penWidthScale applied
        std::vector<BrushComponentCode::BrushPoint> scaled_centerline() const;

        // EncxdedCanvas smooth strokes: curve version of meshPath used for drawing, rebuilt when meshPath changes.
        // meshPath stays the source of truth for collision, erasing and saving.
        SkPath get_smooth_path() const;
        mutable std::mutex smoothPathMutex;
        mutable SkPath smoothPath;
        mutable uint32_t smoothPathSourceGenID = 0;
};

