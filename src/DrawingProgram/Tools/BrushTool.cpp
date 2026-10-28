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

#include "BrushTool.hpp"
#include <Helpers/ConvertVec.hpp>
#include "../../GUIStuff/GUIManager.hpp"
#include "../DrawingProgram.hpp"
#include "../../MainProgram.hpp"
#include "DrawingProgramToolBase.hpp"
#include "../../CanvasComponents/MeshCanvasComponent.hpp"
#include "Helpers/Networking/NetLibrary.hpp"
#include "Helpers/NetworkingObjects/NetObjTemporaryPtr.decl.hpp"
#include "../../CanvasComponents/CanvasComponentContainer.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/CheckBoxHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/NumberSliderHelpers.hpp"
#include <include/pathops/SkPathOps.h>

BrushTool::BrushTool(DrawingProgram& initDrawP, bool initIsHighlighter):
    DrawingProgramToolBase(initDrawP),
    isHighlighter(initIsHighlighter)
{}

DrawingProgramToolType BrushTool::get_type() {
    return isHighlighter ? DrawingProgramToolType::HIGHLIGHTER : DrawingProgramToolType::BRUSH;
}

bool BrushTool::has_round_caps() {
    auto& toolConfig = drawP.world.main.toolConfig;
    return isHighlighter ? toolConfig.highlighter.roundEnds : toolConfig.brush.hasRoundCaps;
}

void BrushTool::switch_tool(DrawingProgramToolType newTool) {
    commit_stroke();
}

void BrushTool::erase_component(CanvasComponentContainer::ObjInfo* erasedComp) {
    if(objInfoBeingEdited == erasedComp) {
        objInfoBeingEdited = nullptr;
        commitUpdate = false;
    }
}

void BrushTool::input_mouse_button_on_canvas_callback(const InputManager::MouseButtonCallbackArgs& button) {
    if(button.button == InputManager::MouseButton::LEFT) {
        auto& toolConfig = drawP.world.main.toolConfig;
        if(button.down && drawP.layerMan.is_a_layer_being_edited() && !objInfoBeingEdited) {
            auto relativeWidthResult = drawP.world.main.toolConfig.get_relative_width_stroke_size(drawP, drawP.world.drawData.cam.c.inverseScale);
            if(!relativeWidthResult.first.has_value()) {
                drawP.world.main.toolConfig.print_relative_width_fail_message(relativeWidthResult.second);
                return;
            }

            CanvasComponentContainer* newMeshContainer = new CanvasComponentContainer(drawP.world.netObjMan, CanvasComponentType::MESH);
            MeshCanvasComponent& newMesh = static_cast<MeshCanvasComponent&>(newMeshContainer->get_comp());

            newMesh.d.color = toolConfig.globalConf.foregroundColor;
            if(isHighlighter)
                newMesh.d.color.w() *= toolConfig.highlighter.opacity;
            newMeshContainer->coords = drawP.world.drawData.cam.c;

            // EncxdedCanvas: the stroke is rebuilt from raw samples with these settings, which are locked into it
            auto& conf = drawP.world.main.conf;
            strokeParams = BrushComponentCode::StrokeParams{
                .baseWidth = relativeWidthResult.first.value(),
                .unitScale = 1.0f, // The stroke's space is the camera's (screen pixels) until it's normalized
                .stabilizer = conf.brushStrokeSmoothingFactor,
                .pressureSmoothing = conf.tabletOptions.brushPressureSmoothingFactor,
                .pressureAffectsWidth = !isHighlighter && conf.tabletOptions.pressureAffectsBrushWidth, // Highlighter: constant width
                .minimumSize = conf.tabletOptions.brushMinimumSize,
                .pressureCurve = conf.tabletOptions.brushPressureCurve
            };
            currentPressure = button.deviceType == InputManager::MouseDeviceType::PEN ? drawP.world.main.input.pen.pressure : 1.0f;
            strokeStart = std::chrono::steady_clock::now();
            rawSamples.clear();
            rawSamples.push_back({button.pos, currentPressure, 0.0f});

            objInfoBeingEdited = drawP.layerMan.add_component_to_layer_being_edited(newMeshContainer);
            commit_data(false);
        }
        else if(!button.down && objInfoBeingEdited)
            commit_stroke();
    }
}

void BrushTool::cancel_finger_touch_callback(const FingerInput::TouchCallbackArgs& touch) {
    if(objInfoBeingEdited) {
        NetworkingObjects::NetObjOwnerPtr<CanvasComponentContainer>& containerPtr = objInfoBeingEdited->obj;
        auto& components = containerPtr->parentLayer->get_layer().components;
        components->erase(components, containerPtr->objInfo);
        objInfoBeingEdited = nullptr;
        commitUpdate = false;
    }
}

void BrushTool::commit_data(bool final) {
    if(objInfoBeingEdited) {
        NetworkingObjects::NetObjOwnerPtr<CanvasComponentContainer>& containerPtr = objInfoBeingEdited->obj;
        MeshCanvasComponent& newMesh = static_cast<MeshCanvasComponent&>(containerPtr->get_comp());
        newMesh.d.rawSamples = rawSamples;
        newMesh.d.strokeParams = strokeParams;
        newMesh.d.penPoints = BrushComponentCode::process_raw_samples(rawSamples, strokeParams, final);
        newMesh.d.penRoundCaps = has_round_caps();
        newMesh.d.meshPath = BrushComponentCode::brush_stroke_to_skpath(newMesh.d.penPoints, newMesh.d.penRoundCaps);
        if(final) {
            containerPtr->get_comp().simplify_paths();
            containerPtr->normalize_object_coordinates();
        }
        containerPtr->commit_update(drawP);
        if(final) {
            drawP.world.send_reliable_multi_command_to_all([&]() {
                drawP.send_transforms_for({objInfoBeingEdited});
                containerPtr->send_comp_update(drawP, true);
            });
        }
        else
            containerPtr->send_comp_update(drawP, final);
    }
    commitUpdate = false;
}

void BrushTool::input_mouse_motion_callback(const InputManager::MouseMotionCallbackArgs& motion) {
    if(objInfoBeingEdited) {
        NetworkingObjects::NetObjOwnerPtr<CanvasComponentContainer>& containerPtr = objInfoBeingEdited->obj;
        Vector2f pos = containerPtr->coords.to_space(drawP.world.drawData.cam.c.from_space(motion.pos));
        if(pos != rawSamples.back().pos) { // A repeated position would read as a pressure-only sample
            rawSamples.push_back({pos, currentPressure, seconds_since_stroke_start()});
            commitUpdate = true;
        }
    }
}

void BrushTool::input_pen_axis_callback(const InputManager::PenAxisCallbackArgs& axis) {
    if(axis.axis == SDL_PEN_AXIS_PRESSURE) {
        currentPressure = axis.value;
        if(objInfoBeingEdited && !isHighlighter && strokeParams.pressureAffectsWidth) {
            rawSamples.push_back({rawSamples.back().pos, currentPressure, seconds_since_stroke_start()});
            commitUpdate = true;
        }
    }
}

float BrushTool::seconds_since_stroke_start() const {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - strokeStart).count();
}

void BrushTool::tool_update() {
    if(!drawP.world.main.g.gui.mouse_pointer_obstructed())
        drawP.world.main.input.hideCursor = true;

    if(commitUpdate && objInfoBeingEdited)
        commit_data(false);
}

void BrushTool::commit_stroke() {
    if(objInfoBeingEdited) {
        NetworkingObjects::NetObjOwnerPtr<CanvasComponentContainer>& containerPtr = objInfoBeingEdited->obj;
        commit_data(true); // Final processing includes the tip fix
        if(containerPtr->get_world_bounds().has_value())
            drawP.layerMan.add_undo_place_component(objInfoBeingEdited);
        else {
            auto& components = containerPtr->parentLayer->get_layer().components;
            components->erase(components, containerPtr->objInfo);
        }
        objInfoBeingEdited = nullptr;
    }
}

void BrushTool::gui_phone_toolbox(CanvasEditorScreen& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;

    auto& toolConfig = drawP.world.main.toolConfig;
    if(isHighlighter) {
        gui.new_id("highlighter tool", [&] {
            checkbox_boolean_field(gui, "hasroundcaps", "Round Caps", &toolConfig.highlighter.roundEnds);
            slider_scalar_field(gui, "highlighter opacity", "Opacity", &toolConfig.highlighter.opacity, 0.05f, 1.0f, {.decimalPrecision = 2});
            toolConfig.relative_width_gui(drawP, "Size");
        });
        return;
    }
    gui.new_id("brush tool", [&] {
        checkbox_boolean_field(gui, "hasroundcaps", "Round Caps", &toolConfig.brush.hasRoundCaps);
        toolConfig.relative_width_gui(drawP, "Size");
    });
}

void BrushTool::right_click_popup_gui(Vector2f popupPos) {
    drawP.paint_popup(popupPos);
}

bool BrushTool::prevent_undo_or_redo() {
    return objInfoBeingEdited;
}

void BrushTool::draw(SkCanvas* canvas, const DrawData& drawData) {
    if(!drawP.world.main.input.isTouchDevice && !drawData.main->g.gui.mouse_pointer_obstructed() && drawData.main->visual_mouse_focus()) {
        auto relativeWidthResult = drawP.world.main.toolConfig.get_relative_width_stroke_size(drawP, drawP.world.drawData.cam.c.inverseScale);
        if(relativeWidthResult.first.has_value()) {
            float width = relativeWidthResult.first.value();
            if(objInfoBeingEdited)
                width *= BrushComponentCode::pressure_to_width_factor(currentPressure, strokeParams) * 0.5f;
            else
                width *= 0.5f;
            width += 1.0f;
            Vector2f pos = drawData.main->input.mouse.pos;
            SkPaint linePaint;
            linePaint.setAntiAlias(drawData.skiaAA);
            linePaint.setColor4f({1.0f, 1.0f, 1.0f, 1.0f});
            linePaint.setStyle(SkPaint::kStroke_Style);
            linePaint.setStrokeCap(SkPaint::kRound_Cap);
            linePaint.setStrokeWidth(0.0f);
            SkPath circ = SkPath::Circle(pos.x(), pos.y(), width);
            canvas->drawPath(circ, linePaint);
            linePaint.setColor4f({0.0f, 0.0f, 0.0f, 1.0f});
            circ = SkPath::Circle(pos.x(), pos.y(), width - 1.0f);
            canvas->drawPath(circ, linePaint);
        }
    }
}
