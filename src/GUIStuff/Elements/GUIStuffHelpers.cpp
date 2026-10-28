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

#include "Element.hpp"
#include "Helpers/MathExtras.hpp"
#include <chrono>

namespace GUIStuff {
    SCollision::AABB<float> clay_bounding_box_to_aabb(const Clay_BoundingBox& bb) {
        return {{bb.x, bb.y}, {bb.x + bb.width, bb.y + bb.height}};
    }

    SkFont get_setup_skfont() {
        SkFont font;
        font.setLinearMetrics(true);
        font.setHinting(SkFontHinting::kNormal);
        //font.setForceAutoHinting(true);
        font.setSubpixel(true);
        font.setBaselineSnap(true);
        font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
        //paint.setAntiAlias(true);
        return font;
    }

    SkFont UpdateInputData::get_font(float fSize) const {
        SkFont f = get_setup_skfont();
        f.setTypeface(textTypeface);
        f.setSize(fSize);
        return f;
    }

    std::shared_ptr<Theme> get_default_dark_mode() {
        return get_built_in_theme(get_built_in_theme_names()[0]);
    }

    namespace {
        SkColor4f hex_color(uint32_t rgb) {
            return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f};
        }

        struct ThemeColors {
            uint32_t fill1, fill2, back0, back1, back2, front1, front2, accent, onAccent, page, sidebar, card;
        };

        std::shared_ptr<Theme> make_theme(const ThemeColors& c) {
            auto theme = std::make_shared<Theme>();
            theme->fillColor1 = hex_color(c.fill1);
            theme->fillColor2 = hex_color(c.fill2);
            theme->backColor0 = hex_color(c.back0);
            theme->backColor1 = hex_color(c.back1);
            theme->backColor2 = hex_color(c.back2);
            theme->frontColor1 = hex_color(c.front1);
            theme->frontColor2 = hex_color(c.front2);
            theme->accentColor = hex_color(c.accent);
            theme->onAccentColor = hex_color(c.onAccent);
            theme->pageColor = hex_color(c.page);
            theme->sidebarColor = hex_color(c.sidebar);
            theme->cardColor = hex_color(c.card);
            theme->composeColor = {0.55f, 1.0f, 0.75f};
            return theme;
        }

        // Matches planning-files/themes_no_bg: Navy/Yellow (Ahmni), Charcoal/Blue, Teal, Light/Blue, Light/Navy
        const std::vector<std::pair<std::string, ThemeColors>>& built_in_theme_colors() {
            static const std::vector<std::pair<std::string, ThemeColors>> themes = {
                {"Navy / Yellow",   {0x9EBDFF, 0x7387AD, 0x12171F, 0x2E3645, 0x404A5C, 0xE6EBF2, 0x9EA8BA, 0xF2C94C, 0x1B2330, 0x243346, 0x1B232E, 0x2B394B}},
                {"Charcoal / Blue", {0x8FA6EE, 0x4B5470, 0x141519, 0x2A2B32, 0x3A3B44, 0xE8E8EC, 0x9A9BA5, 0x5A78D6, 0xFFFFFF, 0x24252B, 0x1B1C21, 0x2F3038}},
                {"Teal",            {0x7CC3B7, 0x3D6B66, 0x0D1C20, 0x1D383F, 0x29474F, 0xDFECEB, 0x8FB0AE, 0x3E9184, 0xE9F5F3, 0x173037, 0x11252A, 0x1F3D45}},
                {"Light / Blue",    {0x1F6FE5, 0xB7C9EE, 0xF5F6FA, 0xFFFFFF, 0xDFE3EB, 0x1C1F26, 0x6A717F, 0x1F6FE5, 0xFFFFFF, 0xEEF1F8, 0xE1E5EE, 0xFFFFFF}},
                {"Light / Navy",    {0x2A4877, 0xB9C3D4, 0xF7F7F8, 0xFFFFFF, 0xDCDDE2, 0x1D2129, 0x6B707B, 0x2A4877, 0xFFFFFF, 0xF1F1F3, 0xE4E5E9, 0xFFFFFF}}
            };
            return themes;
        }
    }

    const std::vector<std::string>& get_built_in_theme_names() {
        static const std::vector<std::string> names = [] {
            std::vector<std::string> toRet;
            for(auto& [name, colors] : built_in_theme_colors())
                toRet.emplace_back(name);
            return toRet;
        }();
        return names;
    }

    std::shared_ptr<Theme> get_built_in_theme(const std::string& name) {
        auto& themes = built_in_theme_colors();
        if(name == "Default")
            return make_theme(themes[0].second);
        for(auto& [themeName, colors] : themes) {
            if(themeName == name)
                return make_theme(colors);
        }
        return nullptr;
    }
    
    void SelectionHelper::update(bool isHovering, bool isLeftClick, bool isLeftHeld, const Vector2f& cursorPos) {
        hovered = isHovering;
    
        clicked = false;
        tapped = false;
        justUnselected = false;
        bool oldSelected = selected;
    
        if(isLeftClick) {
            if(hovered) {
                held = true;
                clicked = true;
                selected = true;
                timeClicked = std::chrono::steady_clock::now();
                clickPos = cursorPos;
            }
            else
                selected = false;
        }
        else if(!isLeftHeld) {
            if(held && hovered && (std::chrono::steady_clock::now() - timeClicked) < std::chrono::milliseconds(250) && vec_distance_sqrd(clickPos, cursorPos) < (25 * 25))
                tapped = true;
            held = false;
        }

        if(!selected && oldSelected)
            justUnselected = true;
    }
}
