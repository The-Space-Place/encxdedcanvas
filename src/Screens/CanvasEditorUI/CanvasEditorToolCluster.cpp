// EncxdedCanvas: Ahmni-style tool cluster in the top-left corner of the canvas editor.
// Rows: tools (with overflow dropdown), stroke size presets, quick colors.
#include "../CanvasEditorScreen.hpp"
#include "../../MainProgram.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/Elements/LayoutElement.hpp"
#include "../../World.hpp"
#include "../../ClientData.hpp"
#include "../../ScaleUpCanvas.hpp"
#include <cmath>
#include <format>

using namespace GUIStuff;
using namespace ElementHelpers;

namespace {
    constexpr float CLUSTER_CELL_SIZE = 40.0f;
    constexpr uint16_t CLUSTER_ROW_GAP = 4;
}

void CanvasEditorScreen::tool_cluster() {
    auto& gui = main.g.gui;
    auto& io = gui.io;

    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .padding = {.left = io.theme->padding1, .right = io.theme->padding1, .top = 0, .bottom = io.theme->padding1}, // Top bar above already pads
            .childGap = io.theme->childGap1,
            .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_TOP},
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        }
    }) {
        gui.element<LayoutElement>("tool cluster", [&](LayoutElement*, const Clay_ElementId& lId) {
            CLAY(lId, {
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)},
                    .childGap = CLUSTER_ROW_GAP,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                }
            }) {
                tool_cluster_zoom_row();
                tool_cluster_tool_row();
                if(toolOverflowOpen)
                    tool_cluster_overflow_row();
                tool_cluster_size_row();
                tool_cluster_color_row();
            }
        });

        // Tool settings / color picker popups open beside the cluster
        switch(settingsMenuPopup) {
            case SettingsMenuPopup::NONE:
                break;
            case SettingsMenuPopup::SETTINGS:
                if(colorPickerPtr) {
                    color_settings_popup(colorPickerPtr, colorPickerData, [&] {
                        set_color_picker_data(nullptr, ColorSelectorData());
                    });
                }
                else {
                    reset_color_picker_popup_data();
                    tool_settings_popup();
                }
                break;
            case SettingsMenuPopup::COLOR_CHANGE:
                color_settings_popup(colorPickerPtr, colorPickerData, nullptr);
                break;
        }
    }
}

void CanvasEditorScreen::tool_cluster_row(const char* id, const std::function<void()>& content) {
    auto& gui = main.g.gui;
    auto& io = gui.io;
    gui.element<LayoutElement>(id, [&](LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIXED(CLUSTER_CELL_SIZE)},
                .padding = CLAY_PADDING_ALL(2),
                .childGap = 2,
                .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            },
            .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor1),
            .cornerRadius = CLAY_CORNER_RADIUS(io.theme->windowCorners1)
        }) {
            content();
        }
    });
}

namespace {
    // 100% is the scale a new canvas opens at. The canvas periodically rescales itself by CANVAS_SCALE_UP_STEP
    // (tracked by the grid size), so that's factored in. Everything is done in log2 since the scales are huge.
    double zoom_100_percent_log2(World& w) {
        static const double LOG2_DEFAULT_INVERSE_SCALE = std::log2(1000000.0); // DrawCamera's initial inverseScale
        static const double LOG2_SCALE_UP_STEP = static_cast<double>(FixedPoint::log2(CANVAS_SCALE_UP_STEP));
        uint32_t gridSize = w.ownClientData ? w.ownClientData->get_grid_size() : 0;
        return LOG2_DEFAULT_INVERSE_SCALE + gridSize * LOG2_SCALE_UP_STEP;
    }

    double get_zoom_percent_log2(World& w) {
        return zoom_100_percent_log2(w) - static_cast<double>(FixedPoint::log2(w.drawData.cam.c.inverseScale)) + std::log2(100.0);
    }

    std::string format_zoom_percent(double percentLog2) {
        if(percentLog2 > std::log2(1e6) || percentLog2 < std::log2(0.01))
            return std::format("2^{:.0f}", percentLog2 - std::log2(100.0)); // Far beyond normal range, show the power instead
        double p = std::exp2(percentLog2);
        if(p >= 10.0)
            return std::format("{:.0f}%", p);
        if(p >= 1.0)
            return std::format("{:.1f}%", p);
        return std::format("{:.2f}%", p);
    }

    void set_zoom_percent(World& w, double percent) {
        auto& cam = w.drawData.cam;
        Vector2f halfWindow = w.main.window.size.cast<float>() * 0.5f;
        WorldVec center = cam.c.pos + cam.c.dir_from_space(halfWindow);
        CoordSpaceHelper newC = cam.c;
        newC.inverseScale = FixedPoint::exp2(WorldScalar(zoom_100_percent_log2(w) - std::log2(percent / 100.0)));
        newC.pos = center - newC.dir_from_space(halfWindow);
        cam.smooth_move_to(w, newC, w.main.window.size.cast<float>());
    }
}

void CanvasEditorScreen::tool_cluster_update() {
    // Camera gestures don't relayout the GUI, so refresh when the displayed zoom text changes
    std::string zoomLabel = format_zoom_percent(get_zoom_percent_log2(*world));
    if(zoomLabel != lastZoomLabel) {
        lastZoomLabel = zoomLabel;
        main.g.gui.set_to_layout();
    }
}

void CanvasEditorScreen::tool_cluster_zoom_row() {
    auto& gui = main.g.gui;
    auto& touchOptions = main.conf.touchOptions;
    constexpr float CELL = CLUSTER_CELL_SIZE - 4.0f;

    tool_cluster_row("cluster zoom row", [&] {
        // Straighten the view (undo any rotation)
        svg_icon_button(gui, "reset rotation", "data/icons/RemixIcon/refresh-line.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .size = CELL,
            .onClick = [&] {
                auto& cam = world->drawData.cam;
                cam.c.rotate_about(cam.c.from_space(main.window.size.cast<float>() * 0.5f), -cam.c.rotation);
                cam.lastRotationTime.update_time_point();
            }
        });
        // Current zoom; tapping returns to 100%
        text_button_sized(gui, "current zoom", format_zoom_percent(get_zoom_percent_log2(*world)), CLAY_SIZING_FIXED(CELL), CLAY_SIZING_FIXED(CELL), {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .bold = true,
            .textScale = 0.75f,
            .onClick = [&] { set_zoom_percent(*world, 100.0); }
        });
        svg_icon_button(gui, "rotation lock", touchOptions.twoFingerRotation ? "data/icons/lockopen.svg" : "data/icons/lockclosed.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .isSelected = !touchOptions.twoFingerRotation,
            .size = CELL,
            .onClick = [&] {
                touchOptions.twoFingerRotation = !touchOptions.twoFingerRotation;
            }
        });
        for(double preset : {50.0, 100.0, 200.0}) {
            gui.new_id(static_cast<int64_t>(preset), [&] {
                text_button_sized(gui, "zoom preset", std::format("{:.0f}%", preset), CLAY_SIZING_FIXED(CELL), CLAY_SIZING_FIXED(CELL), {
                    .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                    .bold = true,
                    .textScale = 0.75f,
                    .onClick = [&, preset] { set_zoom_percent(*world, preset); }
                });
            });
        }
    });
}

void CanvasEditorScreen::tool_cluster_tool_button(const char* id, const std::string& svgPath, DrawingProgramToolType toolType) {
    auto& gui = main.g.gui;
    auto& drawP = world->drawProg;
    svg_icon_button(gui, id, svgPath, {
        .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
        .isSelected = drawP.drawTool->get_type() == toolType,
        .size = CLUSTER_CELL_SIZE - 4.0f,
        .onClick = [&, toolType] {
            if(toolType == DrawingProgramToolType::EDIT)
                set_settings_menu_popup_mode_with_color_picker_data(SettingsMenuPopup::SETTINGS);
            drawP.switch_to_tool(toolType);
        }
    });
}

void CanvasEditorScreen::tool_cluster_tool_row() {
    auto& gui = main.g.gui;
    tool_cluster_row("cluster tool row", [&] {
        svg_icon_button(gui, "tool overflow", toolOverflowOpen ? "data/icons/droparrowclose.svg" : "data/icons/droparrow.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .isSelected = toolOverflowOpen,
            .size = CLUSTER_CELL_SIZE - 4.0f,
            .onClick = [&] {
                toolOverflowOpen = !toolOverflowOpen;
            }
        });
        tool_cluster_tool_button("cluster lasso", "data/icons/lassoselect.svg", DrawingProgramToolType::LASSOSELECT);
        tool_cluster_tool_button("cluster eraser", "data/icons/eraser.svg", DrawingProgramToolType::ERASER);
        tool_cluster_tool_button("cluster brush", "data/icons/pencil.svg", DrawingProgramToolType::BRUSH);
        tool_cluster_tool_button("cluster highlighter", "data/icons/highlighter.svg", DrawingProgramToolType::HIGHLIGHTER);
        svg_icon_button(gui, "cluster tool settings", "data/icons/RemixIcon/settings-3-line.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .isSelected = settingsMenuPopup == SettingsMenuPopup::SETTINGS,
            .size = CLUSTER_CELL_SIZE - 4.0f,
            .onClick = [&] {
                set_settings_menu_popup_mode_with_color_picker_data((settingsMenuPopup == SettingsMenuPopup::SETTINGS) ? SettingsMenuPopup::NONE : SettingsMenuPopup::SETTINGS);
            }
        });
    });
}

void CanvasEditorScreen::tool_cluster_overflow_row() {
    tool_cluster_row("cluster overflow row", [&] {
        tool_cluster_tool_button("overflow line", "data/icons/line.svg", DrawingProgramToolType::LINE);
        tool_cluster_tool_button("overflow text", "data/icons/text.svg", DrawingProgramToolType::TEXTBOX);
        tool_cluster_tool_button("overflow ellipse", "data/icons/circle.svg", DrawingProgramToolType::ELLIPSE);
        tool_cluster_tool_button("overflow rect", "data/icons/rectangle.svg", DrawingProgramToolType::RECTANGLE);
        tool_cluster_tool_button("overflow rectselect", "data/icons/rectselect.svg", DrawingProgramToolType::RECTSELECT);
        tool_cluster_tool_button("overflow edit", "data/icons/cursor.svg", DrawingProgramToolType::EDIT);
        tool_cluster_tool_button("overflow eyedropper", "data/icons/eyedropper.svg", DrawingProgramToolType::EYEDROPPER);
        tool_cluster_tool_button("overflow zoom", "data/icons/zoom.svg", DrawingProgramToolType::ZOOM);
        tool_cluster_tool_button("overflow pan", "data/icons/hand.svg", DrawingProgramToolType::PAN);
    });
}

void CanvasEditorScreen::tool_cluster_size_row() {
    auto& gui = main.g.gui;
    auto& toolConf = main.toolConfig;
    DrawingProgramToolType toolType = world->drawProg.drawTool->get_type();
    float& currentWidth = toolConf.get_stroke_size_relative_width_ref(toolType);
    auto& presets = toolConf.get_size_presets(toolType);

    // Only stroke-drawing tools use the presets. For the rest the row stays, but empty, so the color row never moves.
    bool toolUsesSize = toolType == DrawingProgramToolType::BRUSH || toolType == DrawingProgramToolType::HIGHLIGHTER || toolType == DrawingProgramToolType::LINE ||
                        toolType == DrawingProgramToolType::ELLIPSE || toolType == DrawingProgramToolType::RECTANGLE;

    tool_cluster_row("cluster size row", [&] {
        if(!toolUsesSize) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(presets.size() * (CLUSTER_CELL_SIZE - 4.0f) + (presets.size() - 1) * 2.0f)}}}) {}
            return;
        }
        for(size_t i = 0; i < presets.size(); i++) {
            gui.new_id(i, [&] {
                float preset = presets[i];
                std::string label = std::format("{:g}", preset);
                text_button_sized(gui, "size preset", label, CLAY_SIZING_FIXED(CLUSTER_CELL_SIZE - 4.0f), CLAY_SIZING_FIXED(CLUSTER_CELL_SIZE - 4.0f), {
                    .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                    .isSelected = currentWidth == preset,
                    .bold = true,
                    .onClick = [&currentWidth, preset] {
                        currentWidth = preset;
                    }
                });
            });
        }
    });
}

void CanvasEditorScreen::tool_cluster_color_row() {
    auto& gui = main.g.gui;
    auto& globalConf = main.toolConfig.globalConf;
    Vector4f& fg = globalConf.foregroundColor;

    tool_cluster_row("cluster color row", [&] {
        // Opens the full color picker. If the current color is one of the swatches, edits that swatch too.
        svg_icon_button(gui, "cluster color picker", "data/icons/droparrow.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .isSelected = settingsMenuPopup == SettingsMenuPopup::COLOR_CHANGE,
            .size = CLUSTER_CELL_SIZE - 4.0f,
            .onClick = [&] {
                if(settingsMenuPopup == SettingsMenuPopup::COLOR_CHANGE) {
                    set_settings_menu_popup_mode_with_color_picker_data(SettingsMenuPopup::NONE);
                    return;
                }
                Vector4f* swatch = nullptr;
                for(auto& c : globalConf.quickColors)
                    if(c == fg) swatch = &c;
                if(swatch) {
                    set_settings_menu_popup_mode_with_color_picker_data(SettingsMenuPopup::COLOR_CHANGE, swatch, ColorSelectorData{
                        .onChange = [&, swatch] { main.toolConfig.globalConf.foregroundColor = *swatch; }
                    });
                }
                else
                    set_settings_menu_popup_mode_with_color_picker_data(SettingsMenuPopup::COLOR_CHANGE, &fg);
            }
        });

        for(size_t i = 0; i < globalConf.quickColors.size(); i++) {
            gui.new_id(i, [&] {
                Vector4f* swatch = &globalConf.quickColors[i];
                color_button(gui, "quick color", swatch, {
                    .drawType = SelectableButton::DrawType::SWATCH,
                    .isSelected = *swatch == fg,
                    .hasAlpha = true,
                    .size = CLUSTER_CELL_SIZE - 4.0f,
                    .onClick = [&, swatch] {
                        main.toolConfig.globalConf.foregroundColor = *swatch;
                        // Keep an open picker pointed at the newly selected swatch
                        if(settingsMenuPopup == SettingsMenuPopup::COLOR_CHANGE) {
                            set_settings_menu_popup_mode_with_color_picker_data(SettingsMenuPopup::COLOR_CHANGE, swatch, ColorSelectorData{
                                .onChange = [&, swatch] { main.toolConfig.globalConf.foregroundColor = *swatch; }
                            });
                        }
                    }
                });
            });
        }
    });
}
