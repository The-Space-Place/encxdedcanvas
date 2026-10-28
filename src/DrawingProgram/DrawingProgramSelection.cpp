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

#include "DrawingProgramSelection.hpp"
#include "DrawingProgram.hpp"
#include "../World.hpp"
#include "../MainProgram.hpp"
#include "../InputManager.hpp"
#include <Helpers/Parallel.hpp>
#include <Helpers/Logger.hpp>
#include "EditCanvasComponentWorldUndoAction.hpp"
#include "../CanvasComponents/MeshCanvasComponent.hpp"
#include <numbers>

#ifdef USE_SKIA_BACKEND_GRAPHITE
    #include <include/gpu/graphite/Surface.h>
#elif USE_SKIA_BACKEND_GANESH
    #include <include/gpu/ganesh/GrDirectContext.h>
    #include <include/gpu/ganesh/SkSurfaceGanesh.h>
#endif
#include <include/effects/SkImageFilters.h>
#include <include/core/SkPathBuilder.h>

#define ROTATION_POINT_RADIUS_MULTIPLIER 0.7f
#define ROTATION_POINTS_DISTANCE 20.0f

#ifdef __EMSCRIPTEN__
    #include <EmscriptenHelpers/emscripten_browser_clipboard.h>
#endif

#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/ElementHelpers/RadioButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"
#include "../GUIStuff/Elements/NumberSlider.hpp"

DrawingProgramSelection::DrawingProgramSelection(DrawingProgram& initDrawP):
    drawP(initDrawP)
{}

void DrawingProgramSelection::phone_selection_gui(CanvasEditorScreen& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;

    gui.new_id("general selection gui", [&] {
        text_label(gui, "Select from:");
        radio_button_selector(gui, "layer selector", &drawP.controls.layerSelector, {
            {"Layer being edited", DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED},
            {"All visible layers", DrawingProgramLayerManager::LayerSelector::ALL_VISIBLE_LAYERS}
        });
    });
}

Vector4f* DrawingProgramSelection::color_picker_color(Vector4f* oldColor) {
    if(is_something_selected()) {
        if(oldColor == &strokeColorChangeData.newColor)
            return oldColor;
    }
    return nullptr;
}

void DrawingProgramSelection::phone_selection_bottom_toolbar(CanvasEditorScreen& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;
    auto& io = gui.io;

    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0) },
            .childGap = io.theme->childGap1,
            .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_BOTTOM},
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0) },
                .childGap = io.theme->childGap1,
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            if(strokeColorChangeData.guiColorSelectorData.has_value())
                t.color_settings_popup(&strokeColorChangeData.newColor, strokeColorChangeData.guiColorSelectorData.value(), nullptr);
        }
        // EncxdedCanvas: Scale/Stretch mode and the thickness drag-slider
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            gui.element<LayoutElement>("selection transform options", [&](LayoutElement*, const Clay_ElementId& lId) {
                CLAY(lId, {
                    .layout = {
                        .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)},
                        .padding = CLAY_PADDING_ALL(io.theme->padding1),
                        .childGap = io.theme->childGap1,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT
                    },
                    .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor1),
                    .cornerRadius = CLAY_CORNER_RADIUS(io.theme->windowCorners1)
                }) {
                    text_button(gui, "scale mode", "Scale", {
                        .drawType = SelectableButton::DrawType::TRANSPARENT_BORDER,
                        .isSelected = scaleMode == ScaleMode::SCALE,
                        .bold = true,
                        .onClick = [&] { scaleMode = ScaleMode::SCALE; }
                    });
                    text_button(gui, "stretch mode", "Stretch", {
                        .drawType = SelectableButton::DrawType::TRANSPARENT_BORDER,
                        .isSelected = scaleMode == ScaleMode::STRETCH,
                        .bold = true,
                        .onClick = [&] { scaleMode = ScaleMode::STRETCH; }
                    });
                    text_label_bold(gui, "Thickness");
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_FIXED(180), .height = CLAY_SIZING_FIT(0)}}}) {
                        // Runs -1..1 around the middle: full left halves the pen width twice (1/4), full right doubles it twice (4x).
                        // Exponential, so it never reaches zero. Snaps back to the middle on release for the next drag.
                        gui.element<NumberSlider<float>>("thickness slider", &thicknessDrag, -1.0f, 1.0f, NumberSliderData{
                            .onChange = [&] { thickness_apply_factor(std::exp2(thicknessDrag * 2.0f)); },
                            .onHold = [&] { thickness_capture_originals(); },
                            .onRelease = [&] {
                                thickness_commit(false);
                                thicknessDrag = 0.0f;
                                calculate_aabb();
                            }
                        });
                    }
                }
            });
        }
        CLAY_AUTO_ID({
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0) },
                .childGap = io.theme->childGap1,
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
                .layoutDirection = CLAY_LEFT_TO_RIGHT
            }
        }) {
            gui.element<LayoutElement>("selection bottom toolbar", [&](LayoutElement*, const Clay_ElementId& lId) {
                CLAY(lId, {
                    .layout = {
                        .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIT(0)}
                    },
                    .backgroundColor = convert_vec4<Clay_Color>(io.theme->backColor1),
                    .cornerRadius = CLAY_CORNER_RADIUS(io.theme->windowCorners1)
                }) {
                    gui.clipping_element<ScrollArea>("selection tools scroll", ScrollArea::Options{
                        .scrollHorizontal = true,
                        .clipHorizontal = true,
                        .scrollbarX = ScrollArea::ScrollbarType::NONE,
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .xAlign = CLAY_ALIGN_X_LEFT,
                        .yAlign = CLAY_ALIGN_Y_CENTER,
                        .innerContent = [&](auto&) {
                            phone_bottom_toolbar_gui(t);
                        }
                    });
                }
            });
        }
    }
}

void DrawingProgramSelection::phone_bottom_toolbar_gui(CanvasEditorScreen& t) {
    using namespace GUIStuff;
    using namespace ElementHelpers;

    auto& gui = drawP.world.main.g.gui;

    gui.new_id("phone selection bottom toolbar", [&] {
        svg_icon_button(gui, "Copy button", "data/icons/RemixIcon/file-copy-line.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .onClick = [&] {
                selection_to_clipboard();
            }
        });
        svg_icon_button(gui, "Cut button", "data/icons/RemixIcon/scissors-line.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .onClick = [&] {
                selection_to_clipboard();
                delete_all();
            }
        });
        svg_icon_button(gui, "Delete button", "data/icons/trash.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .onClick = [&] {
                delete_all();
            }
        });
        svg_icon_button(gui, "Bring to front of layer", "data/icons/RemixIcon/bring-to-front.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .onClick = [&] {
                push_selection_to_front();
            }
        });
        svg_icon_button(gui, "Send to back of layer", "data/icons/RemixIcon/send-to-back.svg", {
            .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
            .onClick = [&] {
                push_selection_to_back();
            }
        });
        color_button(gui, "Stroke color button", &strokeColorChangeData.newColor, {
            .isSelected = strokeColorChangeData.guiColorSelectorData.has_value(),
            .onClick = [&] {
                if(strokeColorChangeData.guiColorSelectorData.has_value())
                    strokeColorChangeData.guiColorSelectorData = std::nullopt;
                else {
                    if(strokeColorChangeData.newColor.w() == 0.0f)
                        strokeColorChangeData.newColor.w() = 1.0f;
                    update_selection_stroke_color();
                    strokeColorChangeData.guiColorSelectorData = {
                        .onChange = [&] {
                            update_selection_stroke_color();
                        }
                    };
                }
            }
        });
    });
}

void DrawingProgramSelection::update_selection_stroke_color() {
    if(strokeColorChangeData.oldColor != strokeColorChangeData.newColor) {
        for(auto& c : selectedSet) {
            if(c->obj->get_comp().get_stroke_color() != std::nullopt) {
                strokeColorChangeData.oldColorData.emplace(c->obj.get_net_id(), c->obj->get_comp().get_stroke_color().value());
                c->obj->get_comp().change_stroke_color(strokeColorChangeData.newColor);
                c->obj->send_comp_update(drawP, false); // Might be deselected, so make sure to send update before its deselected
            }
        }
        commitChangeColorUpdate = true;
        strokeColorChangeData.oldColor = strokeColorChangeData.newColor;
    }
}

void DrawingProgramSelection::check_add_stroke_color_change_undo() {
    if(!strokeColorChangeData.oldColorData.empty()) {
        class EditCanvasComponentsStrokeColorWorldUndoAction : public WorldUndoAction {
            public:
                EditCanvasComponentsStrokeColorWorldUndoAction(std::vector<WorldUndoManager::UndoObjectID> initUndoIDs, std::vector<Vector4f> initOldColors):
                    undoIDs(std::move(initUndoIDs)),
                    colors(std::move(initOldColors))
                {}
                std::string get_name() const override {
                    return "Edit Stroke Colors";
                }
                bool undo(WorldUndoManager& undoMan) override {
                    return undo_redo(undoMan);
                }
                bool redo(WorldUndoManager& undoMan) override {
                    return undo_redo(undoMan);
                }
                bool undo_redo(WorldUndoManager& undoMan) {
                    std::vector<NetworkingObjects::NetObjID> netIDs;

                    if(!undoMan.fill_netid_list_from_undoid_list(netIDs, undoIDs))
                        return false;

                    {
                        for(size_t i = 0; i < netIDs.size(); i++) {
                            auto objPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<CanvasComponentContainer>(netIDs[i]);
                            if(objPtr) {
                                Vector4f oldColor = objPtr->get_comp().get_stroke_color().value();
                                objPtr->get_comp().change_stroke_color(colors[i]);
                                colors[i] = oldColor;
                                objPtr->commit_update(undoMan.world.drawProg);
                                objPtr->send_comp_update(undoMan.world.drawProg, true);
                            }
                        }
                    }

                    return true;
                }
                ~EditCanvasComponentsStrokeColorWorldUndoAction() {}

                std::vector<WorldUndoManager::UndoObjectID> undoIDs;
                std::vector<Vector4f> colors;
        };

        std::vector<WorldUndoManager::UndoObjectID> undoIDs;
        std::vector<Vector4f> oldColors;

        for(auto& [netID, strokeColor] : strokeColorChangeData.oldColorData) {
            undoIDs.emplace_back(drawP.world.undo.get_undoid_from_netid(netID));
            oldColors.emplace_back(strokeColor);
        }

        drawP.world.undo.push(std::make_unique<EditCanvasComponentsStrokeColorWorldUndoAction>(std::move(undoIDs), std::move(oldColors)));
        strokeColorChangeData.oldColorData.clear();
    }
}

void DrawingProgramSelection::add_from_cam_coord_collider_to_selection(const SkPath& cC, DrawingProgramLayerManager::LayerSelector layerSelector, bool frontObjectOnly) {
    check_add_stroke_color_change_undo();

    std::vector<CanvasComponentContainer::ObjInfo*> selectedComponents;
    if(frontObjectOnly) {
        CanvasComponentContainer::ObjInfo* a = drawP.drawCache.get_front_object_colliding_with_in_editing_layer(cC);
        if(a) {
            selectedComponents.emplace_back(a);
            drawP.drawCache.erase_component(a);
        }
    }
    else {
        auto cCWorldBounds = drawP.world.drawData.cam.c.collider_to_world<SCollision::AABB<WorldScalar>, SCollision::AABB<float>>(cC.getBounds());
        drawP.drawCache.traverse_bvh_run_function(cCWorldBounds, erase_select_objects_in_bvh_func(selectedComponents, cC, layerSelector));
    }
    add_to_selection(selectedComponents);
}

void DrawingProgramSelection::remove_from_cam_coord_collider_to_selection(const SkPath& cC, DrawingProgramLayerManager::LayerSelector layerSelector, bool frontObjectOnly) {
    check_add_stroke_color_change_undo();

    if(frontObjectOnly) {
        CanvasComponentContainer::ObjInfo* p = get_front_object_colliding_with_in_editing_layer(cC);
        if(p) {
            drawP.drawCache.add_component(p);
            std::erase(selectedSet, p);
        }
    }
    else {
        std::erase_if(selectedSet, [&](auto& c) {
            if(drawP.layerMan.component_passes_layer_selector(c, layerSelector) && c->obj->collides_with(drawP.world.drawData.cam.c, cC)) {
                drawP.drawCache.add_component(c);
                return true;
            }
            return false;
        });
    }

    calculate_aabb();
}

std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>&)> DrawingProgramSelection::erase_select_objects_in_bvh_func(std::vector<CanvasComponentContainer::ObjInfo*>& selectedComponents, const SkPath& cC, DrawingProgramLayerManager::LayerSelector layerSelector) {
    auto toRet = [&](const auto& bvhNode) {
        if(bvhNode && (bvhNode->coords.inverseScale << 9) < drawP.world.drawData.cam.c.inverseScale &&
           cC.contains(convert_vec2<SkPoint>(drawP.world.drawData.cam.c.to_space(bvhNode->bounds.min))) &&
           cC.contains(convert_vec2<SkPoint>(drawP.world.drawData.cam.c.to_space(bvhNode->bounds.max))) &&
           cC.contains(convert_vec2<SkPoint>(drawP.world.drawData.cam.c.to_space(bvhNode->bounds.top_right()))) &&
           cC.contains(convert_vec2<SkPoint>(drawP.world.drawData.cam.c.to_space(bvhNode->bounds.bottom_left())))) {
            drawP.drawCache.invalidate_cache_at_aabb(bvhNode->bounds);
            drawP.drawCache.traverse_bvh_run_function_starting_at_node_no_collision_check(bvhNode, [&](const auto& bvhNodeChild) {
                drawP.drawCache.node_loop_erase_if_components(bvhNodeChild, [&](auto c) {
                    if(drawP.layerMan.component_passes_layer_selector(c, drawP.controls.layerSelector)) {
                        selectedComponents.emplace_back(c);
                        return true;
                    }
                    return false;
                });
                return true;
            });
            return false;
        }
        drawP.drawCache.node_loop_erase_if_components(bvhNode, [&](auto c) {
            if(drawP.layerMan.component_passes_layer_selector(c, drawP.controls.layerSelector) && c->obj->collides_with(drawP.world.drawData.cam.c, cC)) {
                selectedComponents.emplace_back(c);
                drawP.drawCache.invalidate_cache_at_optional_aabb(c->obj->get_world_bounds());
                return true;
            }
            return false;
        });
        return true;
    };

    return toRet;
}

void DrawingProgramSelection::add_to_selection(const std::vector<CanvasComponentContainer::ObjInfo*>& newSelection) {
    selectedSet.insert(selectedSet.end(), newSelection.begin(), newSelection.end());
    sort_selection();
    calculate_aabb();
}

void DrawingProgramSelection::set_to_selection(const std::vector<CanvasComponentContainer::ObjInfo*>& newSelection) {
    selectedSet = newSelection;
    sort_selection();
    calculate_aabb();
}

void DrawingProgramSelection::sort_selection() {
    std::vector<DrawingProgramLayerListItem*> flattenedLayerList = drawP.layerMan.get_flattened_layer_list();
    std::unordered_map<DrawingProgramLayerListItem*, size_t> flattenedLayerListOrder;
    for(size_t i = 0; i < flattenedLayerList.size(); i++)
        flattenedLayerListOrder[flattenedLayerList[i]] = flattenedLayerList.size() - 1 - i;
    std::sort(selectedSet.begin(), selectedSet.end(), [&](auto& a, auto& b) {
        return (flattenedLayerListOrder[a->obj->parentLayer] < flattenedLayerListOrder[b->obj->parentLayer]) || (a->obj->parentLayer == b->obj->parentLayer && a->pos < b->pos);
    });
}

void DrawingProgramSelection::calculate_aabb() {
    if(is_something_selected()) {
        initialSelectionAABB = (*selectedSet.begin())->obj->get_world_bounds().value();
        for(auto& c : selectedSet)
            initialSelectionAABB.include_aabb_in_bounds(c->obj->get_world_bounds().value());
        if(!showRotationAnchor)
            rotateData.centerPos = initialSelectionAABB.center();
    }
}

bool DrawingProgramSelection::is_something_selected() {
    return !selectedSet.empty();
}

bool DrawingProgramSelection::is_selected(CanvasComponentContainer::ObjInfo* objToCheck) {
    return std::find(selectedSet.begin(), selectedSet.end(), objToCheck) != selectedSet.end();
}

void DrawingProgramSelection::reset_all() {
    check_add_stroke_color_change_undo();
    showRotationAnchor = false;
    thicknessOriginals.clear();
    thicknessDrag = 0.0f;
    selectedSet.clear();
    reset_transform_data();
    strokeColorChangeData = StrokeColorChangeData();
}

void DrawingProgramSelection::reset_transform_data() {
    check_add_stroke_color_change_undo();

    selectionTransformCoords = CoordSpaceHelperTransform();
    transformOpHappening = TransformOperation::NONE;
    translateData = TranslationData();
    scaleData = ScaleData();
    WorldVec keepRotationCenter = rotateData.centerPos; // Custom anchor (if shown) survives between transforms
    rotateData = RotationData();
    rotateData.centerPos = keepRotationCenter;
}

bool DrawingProgramSelection::point_collided_with_selection_aabb(const Vector2f& p) {
    if(SCollision::collide(camSpaceSelection, p))
        return true;
    // Tiny selections get a finger-sized grab area around their center
    Vector2f center = (scaleData.cornerHandlePoints[0] + scaleData.cornerHandlePoints[2]) * 0.5f;
    return selection_is_small_on_screen() && (p - center).norm() < drawP.drag_point_radius() * 2.5f;
}

bool DrawingProgramSelection::point_collided_with_rotate_center_handle_point(const Vector2f& p) {
    return SCollision::collide(SCollision::Circle(rotateData.centerHandlePoint, drawP.drag_point_radius()), p);
}

int DrawingProgramSelection::scale_corner_at(const Vector2f& p) {
    for(int i = 0; i < 4; i++) {
        if(SCollision::collide(SCollision::Circle(scaleData.cornerHandlePoints[i], drawP.drag_point_radius()), p))
            return i;
    }
    return -1;
}

int DrawingProgramSelection::rotate_corner_at(const Vector2f& p) {
    for(int i = 0; i < 4; i++) {
        if(SCollision::collide(SCollision::Circle(rotateData.cornerHandlePoints[i], drawP.drag_point_radius()), p))
            return i;
    }
    return -1;
}

// Small on screen = handles would cover most of the box, so moving takes priority over them
bool DrawingProgramSelection::selection_is_small_on_screen() {
    float minSide = std::min((scaleData.cornerHandlePoints[0] - scaleData.cornerHandlePoints[1]).norm(), (scaleData.cornerHandlePoints[0] - scaleData.cornerHandlePoints[3]).norm());
    return minSide < drawP.drag_point_radius() * 5.0f;
}

void DrawingProgramSelection::commit_transform_selection() {
    check_add_stroke_color_change_undo();

    if(!is_empty_transform()) {
        std::vector<WorldUndoManager::UndoObjectID> undoIDList;
        std::vector<std::pair<CoordSpaceHelper, CoordSpaceHelper>> transformDataList;
        for(auto& comp : selectedSet) {
            undoIDList.emplace_back(drawP.world.undo.get_undoid_from_netid(comp->obj.get_net_id()));
            auto& transformData = transformDataList.emplace_back();
            transformData.first = comp->obj->coords;
            comp->obj->coords = selectionTransformCoords.other_coord_space_from_this_space(comp->obj->coords);
            transformData.second = comp->obj->coords;
            comp->obj->commit_transform_dont_invalidate_cache();
        }

        class TransformCanvasComponentsWorldUndoAction : public WorldUndoAction {
            public:
                TransformCanvasComponentsWorldUndoAction(std::vector<WorldUndoManager::UndoObjectID> initUndoIDs, std::vector<std::pair<CoordSpaceHelper, CoordSpaceHelper>> initTransformData):
                    undoIDs(std::move(initUndoIDs)),
                    transformData(std::move(initTransformData))
                {}
                std::string get_name() const override {
                    return "Transform Canvas Components";
                }
                bool undo(WorldUndoManager& undoMan) override {
                    std::vector<NetworkingObjects::NetObjID> toEditIDs;
                    if(!undoMan.fill_netid_list_from_undoid_list(toEditIDs, undoIDs))
                        return false;

                    std::vector<CanvasComponentContainer::ObjInfo*> transformSet;

                    for(size_t i = 0; i < toEditIDs.size(); i++) {
                        auto objPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<CanvasComponentContainer>(toEditIDs[i]);
                        objPtr->coords = transformData[i].first;
                        objPtr->commit_transform(undoMan.world.drawProg);
                        transformSet.emplace_back(&(*objPtr->objInfo));
                    }

                    undoMan.world.drawProg.send_transforms_for(transformSet);
                    return true;
                }
                bool redo(WorldUndoManager& undoMan) override {
                    std::vector<NetworkingObjects::NetObjID> toEditIDs;
                    if(!undoMan.fill_netid_list_from_undoid_list(toEditIDs, undoIDs))
                        return false;

                    std::vector<CanvasComponentContainer::ObjInfo*> transformSet;

                    for(size_t i = 0; i < toEditIDs.size(); i++) {
                        auto objPtr = undoMan.world.netObjMan.get_obj_temporary_ref_from_id<CanvasComponentContainer>(toEditIDs[i]);
                        objPtr->coords = transformData[i].second;
                        objPtr->commit_transform(undoMan.world.drawProg);
                        transformSet.emplace_back(&(*objPtr->objInfo));
                    }

                    undoMan.world.drawProg.send_transforms_for(transformSet);
                    return true;
                }
                void scale_up(const WorldScalar& scaleUpAmount) override {
                    for(auto& [oldTransform, newTransform] : transformData) {
                        oldTransform.scale_about(WorldVec{0, 0}, scaleUpAmount, true);
                        newTransform.scale_about(WorldVec{0, 0}, scaleUpAmount, true);
                    }
                }
                ~TransformCanvasComponentsWorldUndoAction() {}

                std::vector<WorldUndoManager::UndoObjectID> undoIDs;
                std::vector<std::pair<CoordSpaceHelper, CoordSpaceHelper>> transformData;
        };

        drawP.world.undo.push(std::make_unique<TransformCanvasComponentsWorldUndoAction>(std::move(undoIDList), std::move(transformDataList)));

        // EncxdedCanvas Stretch mode: the pen-thickness compensation applied live joins the same undo step
        if(!thicknessOriginals.empty())
            thickness_commit(true);
    }
    else if(!thicknessOriginals.empty())
        thickness_restore_originals();

    drawP.send_transforms_for(selectedSet);

    reset_transform_data();
    calculate_aabb();
}

// Only pen/highlighter/line strokes that carry their pen source change; images, text, shapes and older strokes are left alone
void DrawingProgramSelection::thickness_capture_originals() {
    check_add_stroke_color_change_undo();
    thicknessOriginals.clear();
    for(auto& c : selectedSet) {
        auto& comp = c->obj->get_comp();
        if(comp.get_type() == CanvasComponentType::MESH && static_cast<MeshCanvasComponent&>(comp).has_pen_source())
            thicknessOriginals.emplace_back(c, comp.get_data_copy());
    }
}

void DrawingProgramSelection::thickness_apply_factor(float factor) {
    for(auto& [c, original] : thicknessOriginals) {
        auto& mesh = static_cast<MeshCanvasComponent&>(c->obj->get_comp());
        mesh.set_data_from(*original);
        if(factor != 1.0f)
            mesh.scale_stroke_thickness(factor);
        c->obj->commit_update_dont_invalidate_cache(drawP);
        c->obj->send_comp_update(drawP, false);
    }
}

void DrawingProgramSelection::thickness_restore_originals() {
    thickness_apply_factor(1.0f);
    thicknessOriginals.clear();
}

void DrawingProgramSelection::thickness_commit(bool joinPreviousUndo) {
    for(auto& [c, original] : thicknessOriginals) {
        c->obj->send_comp_update(drawP, true);
        auto undoAction = std::make_unique<EditCanvasComponentWorldUndoAction>(std::move(original), drawP.world.undo.get_undoid_from_netid(c->obj.get_net_id()));
        if(joinPreviousUndo)
            drawP.world.undo.push_on_last(std::move(undoAction));
        else
            drawP.world.undo.push(std::move(undoAction));
        joinPreviousUndo = true; // The rest of the strokes join the same undo step
    }
    thicknessOriginals.clear();
}

void DrawingProgramSelection::update() {
    if(commitChangeColorUpdate) {
        for(auto& c : selectedSet) {
            if(c->obj->get_comp().get_stroke_color() != std::nullopt)
                c->obj->commit_update_dont_invalidate_cache(drawP); // Object isn't in the cache since it's selected
        }
        commitChangeColorUpdate = false;
    }
}

void DrawingProgramSelection::input_key_callback_modify_selection(const InputManager::KeyCallbackArgs& key) {
    switch(key.key) {
        case InputManager::KEY_GENERIC_UP: {
            translate_key(InputManager::KEY_GENERIC_UP, key.down);
            break;
        }
        case InputManager::KEY_GENERIC_DOWN: {
            translate_key(InputManager::KEY_GENERIC_DOWN, key.down);
            break;
        }
        case InputManager::KEY_GENERIC_LEFT: {
            translate_key(InputManager::KEY_GENERIC_LEFT, key.down);
            break;
        }
        case InputManager::KEY_GENERIC_RIGHT: {
            translate_key(InputManager::KEY_GENERIC_RIGHT, key.down);
            break;
        }
    }
}

void DrawingProgramSelection::input_key_callback_display_selection(const InputManager::KeyCallbackArgs& key) {
    switch(key.key) {
        case InputManager::KEY_DRAW_DELETE: {
            if(key.down && !key.repeat) {
                delete_all();
                drawP.world.main.g.gui.set_to_layout();
            }
            break;
        }
        case InputManager::KEY_COPY: {
            if(key.down && !key.repeat) {
                selection_to_clipboard();
                drawP.world.main.g.gui.set_to_layout();
            }
            break;
        }
        case InputManager::KEY_CUT: {
            if(key.down && !key.repeat) {
                selection_to_clipboard();
                delete_all();
                drawP.world.main.g.gui.set_to_layout();
            }
            break;
        }
        case InputManager::KEY_PASTE: {
            if(key.down && !key.repeat) {
                deselect_all();
                paste_clipboard(drawP.world.main.input.mouse.pos);
                drawP.world.main.g.gui.set_to_layout();
            }
            break;
        }
        case InputManager::KEY_PASTE_IMAGE: {
            if(key.down && !key.repeat) {
                deselect_all();
                drawP.world.main.input.call_paste(CustomEvents::PasteEvent::DataType::IMAGE, {
                    .pastePosition = drawP.world.main.input.mouse.pos
                });
                drawP.world.main.g.gui.set_to_layout();
            }
            break;
        }
        case InputManager::KEY_DESELECT_AND_EDIT_TOOL: {
            if(key.down && !key.repeat) {
                if(is_something_selected())
                    deselect_all();
                else
                    drawP.switch_to_tool(DrawingProgramToolType::EDIT, true);
                drawP.world.main.g.gui.set_to_layout();
            }
            break;
        }
    }
}

void DrawingProgramSelection::input_mouse_button_on_canvas_callback_modify_selection(const InputManager::MouseButtonCallbackArgs& button) {
    if(button.button == InputManager::MouseButton::LEFT && is_something_selected()) {
        switch(transformOpHappening) {
            case TransformOperation::NONE: {
                if(button.down && !drawP.world.main.input.key(InputManager::KEY_GENERIC_LSHIFT).held && !drawP.world.main.input.key(InputManager::KEY_GENERIC_LALT).held) {
                    rebuild_cam_space();

                    auto start_translate = [&] {
                        translateData.startPos = drawP.world.drawData.cam.c.from_space(button.pos);
                        transformOpHappening = TransformOperation::TRANSLATE;
                        translateData.translateWithKeys = false;
                        check_add_stroke_color_change_undo();
                    };

                    // EncxdedCanvas: moving is the primary action. On small selections it wins over every handle;
                    // otherwise handles are checked first, since they sit on/just outside the box edges.
                    bool smallSelection = selection_is_small_on_screen();
                    int scaleCorner = scale_corner_at(button.pos);
                    int rotateCorner = rotate_corner_at(button.pos);

                    if(showRotationAnchor && point_collided_with_rotate_center_handle_point(button.pos)) {
                        transformOpHappening = TransformOperation::ROTATE_RELOCATE_CENTER;
                        check_add_stroke_color_change_undo();
                    }
                    else if(smallSelection && point_collided_with_selection_aabb(button.pos))
                        start_translate();
                    else if(scaleCorner != -1) {
                        // Scale about the opposite corner
                        scaleData.currentPos = scaleData.startPos = selectionRectPoints[scaleCorner];
                        scaleData.centerPos = selectionRectPoints[(scaleCorner + 2) % 4];
                        scaleData.sizeFactor = 1.0f;
                        transformOpHappening = TransformOperation::SCALE;
                        check_add_stroke_color_change_undo();
                        if(scaleMode == ScaleMode::STRETCH)
                            thickness_capture_originals();
                    }
                    else if(rotateCorner != -1) {
                        auto now = std::chrono::steady_clock::now();
                        if(now - lastRotateHandleTap < std::chrono::milliseconds(350)) {
                            // Double tap: toggle the movable rotation anchor (hidden = rotate about the center)
                            showRotationAnchor = !showRotationAnchor;
                            if(!showRotationAnchor)
                                rotateData.centerPos = selectionTransformCoords.from_space_world(initialSelectionAABB.center());
                            lastRotateHandleTap = {};
                            drawP.world.main.g.gui.set_to_layout();
                        }
                        else {
                            lastRotateHandleTap = now;
                            Vector2f fromCenter = button.pos - rotateData.centerHandlePoint;
                            rotateData.startAngle = std::atan2(fromCenter.y(), fromCenter.x());
                            rotateData.rotationAngle = 0.0;
                            transformOpHappening = TransformOperation::ROTATE;
                            check_add_stroke_color_change_undo();
                        }
                    }
                    else if(point_collided_with_selection_aabb(button.pos))
                        start_translate();
                }
                break;
            }
            case TransformOperation::TRANSLATE:
            case TransformOperation::ROTATE:
            case TransformOperation::SCALE: {
                if(!button.down)
                    commit_transform_selection();
                break;
            }
            case TransformOperation::ROTATE_RELOCATE_CENTER: {
                if(!button.down)
                    transformOpHappening = TransformOperation::NONE;
                break;
            }
        }
    }
}

void DrawingProgramSelection::input_mouse_motion_callback_modify_selection(const InputManager::MouseMotionCallbackArgs& motion) {
    if(drawP.is_device_type_down(motion.deviceType) && is_something_selected()) {
        rebuild_cam_space();

        switch(transformOpHappening) {
            case TransformOperation::NONE:
                break;
            case TransformOperation::TRANSLATE: {
                if(!translateData.translateWithKeys)
                    selectionTransformCoords = CoordSpaceHelperTransform(drawP.world.drawData.cam.c.from_space(motion.pos) - translateData.startPos);
                break;
            }
            case TransformOperation::SCALE: {
                WorldVec centerToScaleStart = scaleData.startPos - scaleData.centerPos;
                Vector2f centerToScaleStartInCamSpace = drawP.world.drawData.cam.c.normalized_dir_to_space(centerToScaleStart.normalized());
                Vector2f scaleCenterPointInCamSpace = drawP.world.drawData.cam.c.to_space(scaleData.centerPos);
                scaleData.currentPos = drawP.world.drawData.cam.c.from_space(project_point_on_line(motion.pos, scaleCenterPointInCamSpace, (scaleCenterPointInCamSpace + centerToScaleStartInCamSpace).eval()));

                WorldVec centerToScaleCurrent = scaleData.currentPos - scaleData.centerPos;

                WorldScalar centerToScaleStartNorm = centerToScaleStart.norm();
                WorldScalar centerToScaleCurrentNorm = centerToScaleCurrent.norm();
                bool isAnyNumberZero = centerToScaleCurrentNorm == WorldScalar(0) || centerToScaleStartNorm == WorldScalar(0);

                if(!isAnyNumberZero) {
                    WorldMultiplier scaleAmount = WorldMultiplier(centerToScaleStartNorm) / WorldMultiplier(centerToScaleCurrentNorm);
                    selectionTransformCoords = CoordSpaceHelperTransform(scaleData.centerPos, scaleAmount);
                    scaleData.sizeFactor = static_cast<float>(WorldMultiplier(centerToScaleCurrentNorm) / WorldMultiplier(centerToScaleStartNorm));
                    // Stretch: shrink the pen width by the same amount the selection grows, so the visible thickness stays put
                    if(!thicknessOriginals.empty() && scaleData.sizeFactor > 0.0f)
                        thickness_apply_factor(1.0f / scaleData.sizeFactor);
                }
                break;
            }
            case TransformOperation::ROTATE_RELOCATE_CENTER: {
                rotateData.centerPos = drawP.world.drawData.cam.c.from_space(motion.pos);
                break;
            }
            case TransformOperation::ROTATE: {
                // Relative to where the handle was grabbed, so the selection doesn't jump
                Vector2f rotationPointDiff = motion.pos - rotateData.centerHandlePoint;
                rotateData.rotationAngle = std::atan2(rotationPointDiff.y(), rotationPointDiff.x()) - rotateData.startAngle;
                selectionTransformCoords = CoordSpaceHelperTransform(rotateData.centerPos, rotateData.rotationAngle);
                break;
            }
        }
    }
}

void DrawingProgramSelection::cancel_finger_touch_callback(const FingerInput::TouchCallbackArgs& touch) {
    if(is_something_selected() && transformOpHappening != TransformOperation::NONE) {
        if(!thicknessOriginals.empty())
            thickness_restore_originals();
        reset_transform_data();
        calculate_aabb();
    }
}

void DrawingProgramSelection::translate_key(unsigned keyPressed, bool pressed) {
    if(is_something_selected()) {
        if(pressed) {
            constexpr float KEY_TRANSLATE_MAGNITUDE = 5.0f;
            Vector2f moveVec;
            switch(keyPressed) {
                case InputManager::KEY_GENERIC_UP:
                    moveVec = {0.0f, -1.0f};
                    break;
                case InputManager::KEY_GENERIC_DOWN:
                    moveVec = {0.0f, 1.0f};
                    break;
                case InputManager::KEY_GENERIC_RIGHT:
                    moveVec = {1.0f, 0.0f};
                    break;
                case InputManager::KEY_GENERIC_LEFT:
                    moveVec = {-1.0f, 0.0f};
                    break;
            }
            moveVec *= KEY_TRANSLATE_MAGNITUDE;
            if(transformOpHappening == TransformOperation::NONE) {
                translateData.startPos = drawP.world.drawData.cam.c.dir_from_space(moveVec);
                translateData.translateWithKeys = true;
                transformOpHappening = TransformOperation::TRANSLATE;
                check_add_stroke_color_change_undo();
            }
            else if(transformOpHappening == TransformOperation::TRANSLATE && translateData.translateWithKeys) {
                moveVec *= KEY_TRANSLATE_MAGNITUDE;
                translateData.startPos += drawP.world.drawData.cam.c.dir_from_space(moveVec);
            }
            selectionTransformCoords = CoordSpaceHelperTransform(translateData.startPos);
        }
        else if(transformOpHappening == TransformOperation::TRANSLATE && translateData.translateWithKeys) {
            bool anyKeyHeld = drawP.world.main.input.key(InputManager::KEY_GENERIC_LEFT).held || drawP.world.main.input.key(InputManager::KEY_GENERIC_RIGHT).held || drawP.world.main.input.key(InputManager::KEY_GENERIC_DOWN).held || drawP.world.main.input.key(InputManager::KEY_GENERIC_UP).held;
            if(!anyKeyHeld) {
                commit_transform_selection();
                return;
            }
        }
    }
}

void DrawingProgramSelection::deselect_all() {
    check_add_stroke_color_change_undo();

    if(is_something_selected()) {
        commit_transform_selection();
        for(auto& obj : selectedSet)
            drawP.drawCache.add_component(obj);
        reset_all();
    }
}

void DrawingProgramSelection::push_selection_to_front() {
    check_add_stroke_color_change_undo();

    auto selectedVec = selectedSet;
    deselect_all();
    drawP.layerMan.push_components_to(selectedVec, false);
}

void DrawingProgramSelection::push_selection_to_back() {
    check_add_stroke_color_change_undo();

    auto selectedVec = selectedSet;
    deselect_all();
    drawP.layerMan.push_components_to(selectedVec, true);
}

void DrawingProgramSelection::delete_all() {
    check_add_stroke_color_change_undo();

    if(is_something_selected()) {
        auto selectedSetTemp = selectedSet;
        reset_all(); // Clear set before erasing, since calling erase_component_set will run a check to see if each object is selected and erase them one by one, which is slower
        drawP.layerMan.erase_component_container(selectedSetTemp);
    }
}

void DrawingProgramSelection::erase_component(CanvasComponentContainer::ObjInfo* objToCheck) {
    check_add_stroke_color_change_undo();

    std::erase(selectedSet, objToCheck);
    if(selectedSet.empty())
        reset_all();
}

CanvasComponentContainer::ObjInfo* DrawingProgramSelection::get_front_object_colliding_with_in_editing_layer(const SkPath& cC) {
    CanvasComponentContainer::ObjInfo* p = nullptr;
    for(auto& c : selectedSet) {
        if(drawP.layerMan.component_passes_layer_selector(c, DrawingProgramLayerManager::LayerSelector::LAYER_BEING_EDITED) && (!p || c->pos >= p->pos) && c->obj->collides_with(drawP.world.drawData.cam.c, cC))
            p = c;
    }
    return p;
}

void DrawingProgramSelection::selection_to_clipboard() {
    check_add_stroke_color_change_undo();

    if(is_something_selected()) {
        auto& clipboard = drawP.world.main.clipboard;
        std::unordered_set<NetworkingObjects::NetObjID> resourceSet;
        for(auto& c : selectedSet)
            c->obj->get_comp().get_used_resources(resourceSet);
        clipboard.components.clear();
        for(auto& c : selectedSet)
            clipboard.components.emplace_back(c->obj->get_data_copy());
        clipboard.pos = initialSelectionAABB.center();
        clipboard.inverseScale = drawP.world.drawData.cam.c.inverseScale;
        clipboard.resources = drawP.world.rMan.copy_resource_set_to_map(resourceSet);
    }
}

void DrawingProgramSelection::paste_clipboard(Vector2f pasteScreenPos) {
    check_add_stroke_color_change_undo();

    if(drawP.layerMan.is_a_layer_being_edited()) {
        auto& clipboard = drawP.world.main.clipboard;

        if(!drawP.is_selection_allowing_tool(drawP.drawTool->get_type()))
            drawP.switch_to_tool(DrawingProgramToolType::EDIT);

        std::vector<std::pair<CanvasComponentContainer::ObjInfoIterator, CanvasComponentContainer*>> placedComponents;
        WorldVec mousePos = drawP.world.drawData.cam.c.from_space(pasteScreenPos);
        WorldVec moveVec = drawP.world.main.clipboard.pos - mousePos;
        WorldMultiplier scaleMultiplier = WorldMultiplier(drawP.world.main.clipboard.inverseScale) / WorldMultiplier(drawP.world.drawData.cam.c.inverseScale);
        WorldScalar scaleLimit(0.001);
        for(auto& c : drawP.world.main.clipboard.components) {
            if((c->coords.inverseScale / scaleMultiplier) < scaleLimit) {
                Logger::get().log(Logger::LogType::WORLDFATAL, "Some pasted objects will be too small! Scale up the canvas first (zoom in alot)");
                return;
            }
        }

        std::unordered_map<NetworkingObjects::NetObjID, NetworkingObjects::NetObjID> resourceRemapIDs;
        for(auto& r : clipboard.resources)
            resourceRemapIDs[r.first] = drawP.world.rMan.add_resource(r.second).get_net_id();
        for(auto& c : clipboard.components) {
            CanvasComponentContainer* newComponentContainer = new CanvasComponentContainer(drawP.world.netObjMan, *c);
            newComponentContainer->get_comp().remap_resource_ids(resourceRemapIDs);
            newComponentContainer->coords.translate(-moveVec);
            newComponentContainer->coords.scale_about(mousePos, scaleMultiplier);
            placedComponents.emplace_back(drawP.layerMan.get_edited_layer_end_iterator(), newComponentContainer);
        }
        std::vector<CanvasComponentContainer::ObjInfoIterator> newlyInsertedObjectIts;
        drawP.layerMan.disable_add_to_cache_and_commit_update_block([&]() {
            newlyInsertedObjectIts = drawP.layerMan.add_many_components_to_layer_being_edited(placedComponents);
        });
        std::vector<CanvasComponentContainer::ObjInfo*> compSetInserted;
        for(auto& it : newlyInsertedObjectIts)
            compSetInserted.emplace_back(&(*it));
        parallel_loop_container(compSetInserted, [&drawP = drawP](CanvasComponentContainer::ObjInfo* comp) {
            comp->obj->commit_update_dont_invalidate_cache(drawP);
        });
        set_to_selection(compSetInserted);
    }
}

void DrawingProgramSelection::paste_image_process_event(const CustomEvents::PasteEvent& paste) {
    check_add_stroke_color_change_undo();

    if(drawP.layerMan.is_a_layer_being_edited()) {
        if(!drawP.is_selection_allowing_tool(drawP.drawTool->get_type()))
            drawP.switch_to_tool(DrawingProgramToolType::EDIT);

        std::vector<CanvasComponentContainer::ObjInfo*> compSetInserted;
        compSetInserted.emplace_back(drawP.add_file_to_canvas_by_data("Image from clipboard", paste.data, paste.mousePos.value()));
        if(compSetInserted.back() != nullptr) {
            drawP.drawCache.erase_component(compSetInserted.back());
            set_to_selection(compSetInserted);
        }
    }
}

bool DrawingProgramSelection::is_being_transformed() {
    return transformOpHappening != TransformOperation::NONE;
}

bool DrawingProgramSelection::is_empty_transform() {
    return transformOpHappening == TransformOperation::NONE || selectionTransformCoords.is_identity();
}

std::unordered_set<CanvasComponentContainer::ObjInfo*> DrawingProgramSelection::get_selection_as_set() {
    return std::unordered_set<CanvasComponentContainer::ObjInfo*>(selectedSet.begin(), selectedSet.end());
}

void DrawingProgramSelection::rebuild_cam_space() {
    selectionRectPoints[0] = selectionTransformCoords.from_space_world(initialSelectionAABB.min);
    selectionRectPoints[1] = selectionTransformCoords.from_space_world(initialSelectionAABB.bottom_left());
    selectionRectPoints[2] = selectionTransformCoords.from_space_world(initialSelectionAABB.max);
    selectionRectPoints[3] = selectionTransformCoords.from_space_world(initialSelectionAABB.top_right());

    // EncxdedCanvas: scale handles on each corner; rotate handles diagonally outside each corner
    Vector2f camCenter{0.0f, 0.0f};
    for(int i = 0; i < 4; i++) {
        scaleData.cornerHandlePoints[i] = drawP.world.drawData.cam.c.to_space(selectionRectPoints[i]);
        camCenter += scaleData.cornerHandlePoints[i] * 0.25f;
    }
    float rotateHandleDistance = drawP.drag_point_radius() * 2.2f;
    for(int i = 0; i < 4; i++) {
        Vector2f outward = scaleData.cornerHandlePoints[i] - camCenter;
        float len = outward.norm();
        outward = len > 0.001f ? (outward / len).eval() : Vector2f{0.7071f, 0.7071f};
        rotateData.cornerHandlePoints[i] = scaleData.cornerHandlePoints[i] + outward * rotateHandleDistance;
    }
    rotateData.centerHandlePoint = drawP.world.drawData.cam.c.to_space(rotateData.centerPos);

    SCollision::ColliderCollection<WorldScalar> collideRectWorld;
    collideRectWorld.triangle.emplace_back(selectionRectPoints[0], selectionRectPoints[1], selectionRectPoints[2]);
    collideRectWorld.triangle.emplace_back(selectionRectPoints[0], selectionRectPoints[2], selectionRectPoints[3]);
    collideRectWorld.recalculate_bounds();

    camSpaceSelection = drawP.world.drawData.cam.c.world_collider_to_coords<SCollision::ColliderCollection<float>>(collideRectWorld);
}

void DrawingProgramSelection::draw_components(SkCanvas* canvas, const DrawData& drawData) {
    if(is_something_selected()) {
        rebuild_cam_space();

        DrawData selectionDrawData = drawData;
        selectionDrawData.cam.c = selectionTransformCoords.other_coord_space_to_this_space(selectionDrawData.cam.c);
        selectionDrawData.cam.set_viewing_area(drawP.world.main.window.size.cast<float>());
        selectionDrawData.refresh_draw_optimizing_values();

        for(auto& c : selectedSet)
            c->obj->draw(canvas, selectionDrawData);
    }
}

std::vector<NetworkingObjects::NetObjID> DrawingProgramSelection::get_selected_ids() const {
    std::vector<NetworkingObjects::NetObjID> ids;
    ids.reserve(selectedSet.size());
    for(auto& c : selectedSet)
        ids.emplace_back(c->obj.get_net_id());
    return ids;
}

CoordSpaceHelper DrawingProgramSelection::get_selection_draw_camera(const CoordSpaceHelper& cam) const {
    return selectionTransformCoords.other_coord_space_to_this_space(cam);
}

void DrawingProgramSelection::draw_gui(SkCanvas* canvas, const DrawData& drawData) {
    if(is_something_selected() && !camSpaceSelection.triangle.empty()) {
        SkPathBuilder selectionRectPath;
        selectionRectPath.moveTo(convert_vec2<SkPoint>(camSpaceSelection.triangle[0].p[0]));
        selectionRectPath.lineTo(convert_vec2<SkPoint>(camSpaceSelection.triangle[0].p[1]));
        selectionRectPath.lineTo(convert_vec2<SkPoint>(camSpaceSelection.triangle[0].p[2]));
        selectionRectPath.lineTo(convert_vec2<SkPoint>(camSpaceSelection.triangle[1].p[2]));
        selectionRectPath.close();
        SkPath selectionRect = selectionRectPath.detach();
        SkPaint p{SkColor4f{0.3f, 0.6f, 0.9f, 0.12f}};
        canvas->drawPath(selectionRect, p);
        SkPaint outline{SkColor4f{0.55f, 0.75f, 1.0f, 0.9f}};
        outline.setStyle(SkPaint::kStroke_Style);
        outline.setStrokeWidth(1.5f);
        outline.setAntiAlias(drawData.skiaAA);
        canvas->drawPath(selectionRect, outline);

        if(drawP.is_actual_selection_tool(drawP.drawTool->get_type())) {
            float uiScale = drawP.world.main.g.final_gui_scale();
            SkPaint handleFill{SkColor4f{1.0f, 1.0f, 1.0f, 1.0f}};
            handleFill.setAntiAlias(drawData.skiaAA);
            SkPaint handleStroke{SkColor4f{0.15f, 0.2f, 0.3f, 1.0f}};
            handleStroke.setStyle(SkPaint::kStroke_Style);
            handleStroke.setStrokeWidth(1.5f * uiScale);
            handleStroke.setAntiAlias(drawData.skiaAA);

            if(transformOpHappening == TransformOperation::NONE || transformOpHappening == TransformOperation::SCALE) {
                for(const Vector2f& corner : scaleData.cornerHandlePoints) {
                    canvas->drawCircle(corner.x(), corner.y(), 5.0f * uiScale, handleFill);
                    canvas->drawCircle(corner.x(), corner.y(), 5.0f * uiScale, handleStroke);
                }
            }
            if(transformOpHappening == TransformOperation::NONE || transformOpHappening == TransformOperation::ROTATE) {
                // Small curved arrow facing outward from each corner
                SkPaint arrowPaint{SkColor4f{1.0f, 1.0f, 1.0f, 0.95f}};
                arrowPaint.setStyle(SkPaint::kStroke_Style);
                arrowPaint.setStrokeWidth(2.0f * uiScale);
                arrowPaint.setStrokeCap(SkPaint::kRound_Cap);
                arrowPaint.setAntiAlias(drawData.skiaAA);
                SkPaint arrowShadow = arrowPaint;
                arrowShadow.setColor4f({0.1f, 0.12f, 0.18f, 0.8f});
                arrowShadow.setStrokeWidth(4.0f * uiScale);
                Vector2f camCenter = (scaleData.cornerHandlePoints[0] + scaleData.cornerHandlePoints[2]) * 0.5f;
                float r = 7.0f * uiScale;
                for(const Vector2f& h : rotateData.cornerHandlePoints) {
                    Vector2f outward = (h - camCenter).normalized();
                    float baseAngle = std::atan2(outward.y(), outward.x()) * 180.0f / std::numbers::pi_v<float>;
                    SkRect arcRect = SkRect::MakeLTRB(h.x() - r, h.y() - r, h.x() + r, h.y() + r);
                    SkPathBuilder arrow;
                    arrow.arcTo(arcRect, baseAngle - 70.0f, 140.0f, true);
                    float endAngle = (baseAngle + 70.0f) * std::numbers::pi_v<float> / 180.0f;
                    Vector2f tip{h.x() + r * std::cos(endAngle), h.y() + r * std::sin(endAngle)};
                    Vector2f tangent{-std::sin(endAngle), std::cos(endAngle)};
                    Vector2f normal{std::cos(endAngle), std::sin(endAngle)};
                    arrow.moveTo(convert_vec2<SkPoint>((tip - tangent * (4.0f * uiScale) + normal * (3.0f * uiScale)).eval()));
                    arrow.lineTo(convert_vec2<SkPoint>(tip));
                    arrow.lineTo(convert_vec2<SkPoint>((tip - tangent * (4.0f * uiScale) - normal * (3.0f * uiScale)).eval()));
                    SkPath arrowPath = arrow.detach();
                    canvas->drawPath(arrowPath, arrowShadow);
                    canvas->drawPath(arrowPath, arrowPaint);
                }
            }
            if(showRotationAnchor && (transformOpHappening == TransformOperation::NONE || transformOpHappening == TransformOperation::ROTATE || transformOpHappening == TransformOperation::ROTATE_RELOCATE_CENTER))
                drawP.draw_drag_circle(canvas, rotateData.centerHandlePoint, {0.9f, 0.5f, 0.1f, 1.0f}, drawData);
        }
    }
}
