// EncxdedCanvas: single background grid (lines or dots) drawn behind the canvas contents.
//
// Canvas-anchored grids keep their on-screen cell size between MIN_CELL_PX and 4x that: as you zoom out, the
// fine grid fades away and the 4x coarser grid remains, and the same repeats at every zoom level. The world-space
// lattice is computed exactly (WorldScalar) so lines don't jump when the level changes.
#include "BackgroundGrid.hpp"
#include "World.hpp"
#include "MainProgram.hpp"
#include "ClientData.hpp"
#include "ScaleUpCanvas.hpp"
#include "DrawData.hpp"
#include <include/effects/SkRuntimeEffect.h>
#include <cmath>
#include <numbers>
#include <iostream>

namespace {

const char* backgroundGridShaderCode = R"V(
uniform vec4 gridColor;
uniform float2 fineCell;
uniform float2 coarseCell;
uniform float2 origin;
uniform float fineAlpha;
uniform float width;
uniform float isDots;

float grid_val(float2 p, float2 cell) {
    float2 m = mod(p - origin, cell);
    float2 d = min(m, cell - m); // Distance to the nearest lattice line on each axis
    if(isDots > 0.5)
        return 1.0 - smoothstep(width, width + 1.0, length(d));
    float lx = 1.0 - smoothstep(width * 0.5, width * 0.5 + 1.0, d.x);
    float ly = 1.0 - smoothstep(width * 0.5, width * 0.5 + 1.0, d.y);
    return max(lx, ly);
}

vec4 main(float2 fragCoord) {
    float v = max(grid_val(fragCoord, fineCell) * fineAlpha, grid_val(fragCoord, coarseCell));
    return vec4(gridColor.rgb, 1.0) * gridColor.a * v;
})V";

sk_sp<SkRuntimeEffect> backgroundGridEffect;

constexpr float MIN_CELL_PX = 10.0f;

// Screen position (relative to the camera's origin, in unrotated camera space) of a lattice line along one axis
float lattice_origin(const WorldScalar& camPos, const WorldScalar& cellWorld, const WorldScalar& inverseScale) {
    WorldScalar frac = camPos % cellWorld;
    return -static_cast<float>(frac / inverseScale);
}

}

void draw_background_grid(World& world, SkCanvas* canvas, const DrawData& drawData) {
    auto& opts = world.main.conf.backgroundGrid;
    using Opts = GlobalConfig::BackgroundGridOptions;
    if(opts.mode == Opts::Mode::OFF || opts.opacity <= 0.0f)
        return;

    if(!backgroundGridEffect) {
        auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(backgroundGridShaderCode));
        if(!err.isEmpty()) {
            std::cout << "[draw_background_grid] Shader construction error\n" << err.c_str() << std::endl;
            return;
        }
        backgroundGridEffect = effect;
    }

    const auto& cam = drawData.cam.c;
    float ratio = std::clamp(opts.boxRatio, 0.1f, 10.0f);
    float spacing = std::max(opts.spacing, 2.0f);
    bool isDots = opts.mode == Opts::Mode::DOTS;
    // For dots, width is the dot radius
    float width = isDots ? std::max(opts.width, 0.5f) : std::max(opts.width, 0.25f);

    Vector2f fineCell, coarseCell, origin;
    float fineAlpha = 1.0f;
    Vector2f windowCenter = drawData.cam.viewingArea * 0.5f;

    canvas->save();
    if(opts.anchor == Opts::Anchor::CANVAS) {
        // World size of one cell at 100% zoom (DrawCamera's initial inverseScale, adjusted for canvas scale-ups)
        uint32_t gridSize = world.ownClientData ? world.ownClientData->get_grid_size() : 0;
        WorldScalar baseCellWorld = WorldScalar(static_cast<double>(spacing)) * WorldScalar(1000000) * FixedPoint::pow_int(CANVAS_SCALE_UP_STEP, gridSize);

        // On-screen size of that cell right now, then pick the power-of-4 level that keeps it in [MIN, 4*MIN)
        double baseCellPxLog2 = static_cast<double>(FixedPoint::log2(baseCellWorld)) - static_cast<double>(FixedPoint::log2(cam.inverseScale));
        double levelsLog4 = (baseCellPxLog2 - std::log2(MIN_CELL_PX)) / 2.0;
        int64_t level = static_cast<int64_t>(std::floor(levelsLog4));
        fineAlpha = static_cast<float>(levelsLog4 - std::floor(levelsLog4)); // 0 when the fine grid is at its smallest

        // Coarse lattice = base cell * 4^(1 - level), computed exactly
        WorldScalar coarseWorld = (level >= 1) ? baseCellWorld / FixedPoint::pow_int(WorldScalar(4), static_cast<uint64_t>(level - 1))
                                               : baseCellWorld * FixedPoint::pow_int(WorldScalar(4), static_cast<uint64_t>(1 - level));
        WorldScalar coarseWorldX = coarseWorld.multiply_double(ratio);

        coarseCell = {static_cast<float>(coarseWorldX / cam.inverseScale), static_cast<float>(coarseWorld / cam.inverseScale)};
        fineCell = coarseCell * 0.25f;
        origin = {lattice_origin(cam.pos.x(), coarseWorldX, cam.inverseScale), lattice_origin(cam.pos.y(), coarseWorld, cam.inverseScale)};

        canvas->rotate(-cam.rotation * 180.0 / std::numbers::pi);
    }
    else {
        fineCell = coarseCell = Vector2f{spacing * ratio, spacing};
        origin = windowCenter;
        if(opts.anchor == Opts::Anchor::SCREEN_ROTATING)
            canvas->rotate(-cam.rotation * 180.0 / std::numbers::pi, windowCenter.x(), windowCenter.y());
    }

    SkColor4f c = world.canvasTheme.get_tool_front_color();
    c.fA = std::clamp(opts.opacity, 0.0f, 1.0f);

    SkRuntimeShaderBuilder builder(backgroundGridEffect);
    builder.uniform("gridColor") = c;
    builder.uniform("fineCell").set(fineCell.data(), 2);
    builder.uniform("coarseCell").set(coarseCell.data(), 2);
    builder.uniform("origin").set(origin.data(), 2);
    builder.uniform("fineAlpha") = fineAlpha;
    builder.uniform("width") = width;
    builder.uniform("isDots") = isDots ? 1.0f : 0.0f;

    SkPaint p;
    p.setShader(builder.makeShader());
    canvas->drawPaint(p);
    canvas->restore();
}
