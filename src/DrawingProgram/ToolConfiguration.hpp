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
#include <array>
#include "../SharedTypes.hpp"
#include "nlohmann/json.hpp"
#include "../GUIStuff/GUIManager.hpp"
#include "../WorldScreenshot.hpp"
#include "Tools/DrawingProgramToolBase.hpp"

class ToolConfiguration {
    public:
        struct BrushToolConfig {
            bool hasRoundCaps = true;
            float relativeWidth = 15.0f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(BrushToolConfig, hasRoundCaps, relativeWidth)
        } brush;

        struct HighlighterToolConfig {
            bool roundEnds = true;
            float relativeWidth = 16.0f;
            float opacity = 0.4f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(HighlighterToolConfig, roundEnds, relativeWidth, opacity)
        } highlighter;

        struct EraserToolConfig {
            float relativeWidth = 15.0f;
            bool eraseDetail = false;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EraserToolConfig, relativeWidth, eraseDetail)
        } eraser;

        struct EllipseDrawToolConfig {
            float relativeWidth = 15.0f;
            unsigned fillStrokeMode = 1;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EllipseDrawToolConfig, relativeWidth, fillStrokeMode)
        } ellipseDraw;

        struct RectDrawToolConfig {
            float relativeWidth = 15.0f;
            float relativeRadiusWidth = 10.0f;
            int fillStrokeMode = 1;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RectDrawToolConfig, relativeWidth, relativeRadiusWidth, fillStrokeMode)
        } rectDraw;

        struct EyeDropperToolConfig {
            bool selectingStrokeColor = true;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(EyeDropperToolConfig, selectingStrokeColor)
        } eyeDropper;

        struct LineDrawToolConfig {
            bool hasRoundCaps = true;
            float relativeWidth = 15.0f;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(LineDrawToolConfig, hasRoundCaps, relativeWidth)
        } lineDraw;

        struct ScreenshotToolConfig {
            int setDimensionSize = 1000;
            bool setDimensionIsX = true;
            WorldScreenshotInfo::ScreenshotType selectedType = WorldScreenshotInfo::ScreenshotType::JPG;
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ScreenshotToolConfig, setDimensionSize, setDimensionIsX, selectedType)
        } screenshot;

        struct GlobalConfig {
            Vector4f foregroundColor{1.0f, 1.0f, 1.0f, 1.0f};
            Vector4f backgroundColor{0.0f, 0.0f, 0.0f, 1.0f};
            float relativeWidth = 15.0f;
            bool useGlobalRelativeWidth = false;
            // Quick-access rows in the top-left tool cluster (editable in Settings > Tools)
            std::array<float, 6> penSizePresets{2.0f, 4.0f, 8.0f, 15.0f, 30.0f, 60.0f};
            std::array<float, 6> highlighterSizePresets{4.0f, 8.0f, 16.0f, 30.0f, 60.0f, 120.0f};
            std::array<Vector4f, 5> quickColors{
                Vector4f{0.96f, 0.36f, 0.55f, 1.0f},
                Vector4f{0.98f, 0.73f, 0.10f, 1.0f},
                Vector4f{0.20f, 0.80f, 0.20f, 1.0f},
                Vector4f{0.15f, 0.58f, 0.98f, 1.0f},
                Vector4f{1.0f, 1.0f, 1.0f, 1.0f}
            };
            NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GlobalConfig, useGlobalRelativeWidth, foregroundColor, backgroundColor, relativeWidth, penSizePresets, highlighterSizePresets, quickColors)
        } globalConf;

        enum class RelativeWidthFailCode {
            SUCCESS,
            TOO_ZOOMED_IN,
            TOO_ZOOMED_OUT
        };

        float& get_stroke_size_relative_width_ref(DrawingProgramToolType toolType);
        const float& get_stroke_size_relative_width_ref(DrawingProgramToolType toolType) const;
        std::array<float, 6>& get_size_presets(DrawingProgramToolType toolType);
        std::pair<std::optional<float>, RelativeWidthFailCode> get_relative_width_from_value(DrawingProgram& drawP, const WorldScalar& camInverseScale, float relativeWidth) const;
        std::pair<std::optional<float>, RelativeWidthFailCode> get_relative_width_stroke_size(DrawingProgram& drawP, const WorldScalar& camInverseScale) const;
        void print_relative_width_fail_message(RelativeWidthFailCode failCode);
        void relative_width_gui(DrawingProgram& drawP, const char* label);

        NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ToolConfiguration, brush, highlighter, eraser, ellipseDraw, rectDraw, eyeDropper, lineDraw, screenshot, globalConf)
};
