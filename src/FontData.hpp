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
#include <include/core/SkFontMgr.h>
#include <include/core/SkTypeface.h>
#include <include/core/SkFont.h>
#include <unordered_map>
#include <Eigen/Dense>
#include <modules/skparagraph/include/FontCollection.h>
#include <modules/skparagraph/include/TypefaceFontProvider.h>

using namespace Eigen;

struct FontData {
    FontData();
    sk_sp<SkFontMgr> defaultFontMgr;
    sk_sp<SkFontMgr> localFontMgr;
    sk_sp<skia::textlayout::FontCollection> collection;
    // EncxdedCanvas: real weight instances of variable fonts (Skia's data font manager registers
    // named instances that all render at the default weight). Takes priority in the collection.
    sk_sp<skia::textlayout::TypefaceFontProvider> variableWeightFonts;
    std::unordered_map<std::string, sk_sp<SkTypeface>> map;
    bool fallbackOnEmoji = false;
    const std::vector<SkString>& get_default_font_families() const;
    void push_default_font_families(std::vector<SkString>& fontFamilies) const;
    ~FontData();
};

Vector2f get_str_font_bounds(const SkFont& font, const std::string& str);
