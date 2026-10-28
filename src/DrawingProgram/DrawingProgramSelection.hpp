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
#include <chrono>
#include "../CoordSpaceHelperTransform.hpp"
#include "Layers/DrawingProgramLayerManager.hpp"
#include "DrawingProgramCache.hpp"
#include "../InputManager.hpp"
#include "../Screens/CanvasEditorScreen.hpp"

class DrawingProgram;
class CanvasEditorScreen;

class DrawingProgramSelection {
    public:
        DrawingProgramSelection(DrawingProgram& initDrawP);
        Vector4f* color_picker_color(Vector4f* oldColor);
        void phone_selection_gui(CanvasEditorScreen& t);
        void phone_selection_bottom_toolbar(CanvasEditorScreen& t);
        void add_from_cam_coord_collider_to_selection(const SkPath& cC, DrawingProgramLayerManager::LayerSelector layerSelector, bool frontObjectOnly);
        void remove_from_cam_coord_collider_to_selection(const SkPath& cC, DrawingProgramLayerManager::LayerSelector layerSelector, bool frontObjectOnly);
        void erase_component(CanvasComponentContainer::ObjInfo* objToCheck);
        bool is_something_selected();
        bool is_selected(CanvasComponentContainer::ObjInfo* objToCheck);
        void draw_components(SkCanvas* canvas, const DrawData& drawData);
        void draw_gui(SkCanvas* canvas, const DrawData& drawData);
        bool is_being_transformed();
        void update();
        void sort_selection();
        void deselect_all();
        void paste_clipboard(Vector2f pasteScreenPos);
        void paste_image_process_event(const CustomEvents::PasteEvent& paste);
        std::unordered_set<CanvasComponentContainer::ObjInfo*> get_selection_as_set();
        void push_selection_to_front();
        void push_selection_to_back();
        void delete_all();
        void selection_to_clipboard();
        CanvasComponentContainer::ObjInfo* get_front_object_colliding_with_in_editing_layer(const SkPath& cC);
        void input_key_callback_modify_selection(const InputManager::KeyCallbackArgs& key);
        void input_key_callback_display_selection(const InputManager::KeyCallbackArgs& key);
        void input_mouse_button_on_canvas_callback_modify_selection(const InputManager::MouseButtonCallbackArgs& button);
        void input_mouse_motion_callback_modify_selection(const InputManager::MouseMotionCallbackArgs& motion);
        void cancel_finger_touch_callback(const FingerInput::TouchCallbackArgs& touch);
        // EncxdedCanvas (recordings): the selected components, and the camera they're drawn with. A drag/scale/rotate in progress is
        // only local until it's released, so replays need this to show it.
        std::vector<NetworkingObjects::NetObjID> get_selected_ids() const;
        CoordSpaceHelper get_selection_draw_camera(const CoordSpaceHelper& cam) const;
    private:
        bool commitChangeColorUpdate = false;

        void phone_bottom_toolbar_gui(CanvasEditorScreen& t);
        void translate_key(unsigned keyPressed, bool pressed);
        bool point_collided_with_selection_aabb(const Vector2f& p);
        bool point_collided_with_rotate_center_handle_point(const Vector2f& p);
        // EncxdedCanvas: corner handles. Return the corner index (matching selectionRectPoints), or -1
        int scale_corner_at(const Vector2f& p);
        int rotate_corner_at(const Vector2f& p);
        bool selection_is_small_on_screen();

        // EncxdedCanvas: Scale grows stroke thickness with the size; Stretch keeps it constant
        enum class ScaleMode { SCALE, STRETCH } scaleMode = ScaleMode::SCALE;
        // Double-tap a rotate handle to show (and then drag) the rotation anchor; otherwise rotation is about the center
        bool showRotationAnchor = false;
        std::chrono::steady_clock::time_point lastRotateHandleTap;

        // Live pen-thickness changes (thickness slider and Stretch mode). The original strokes are captured when the
        // drag starts, every update re-applies the factor to those originals, and release commits with undo.
        float thicknessDrag = 0.0f;
        std::vector<std::pair<CanvasComponentContainer::ObjInfo*, std::unique_ptr<CanvasComponent>>> thicknessOriginals;
        void thickness_capture_originals();
        void thickness_apply_factor(float factor);
        void thickness_restore_originals();
        void thickness_commit(bool joinPreviousUndo);

        bool is_empty_transform();

        void commit_transform_selection();
        void rebuild_cam_space();

        void update_selection_stroke_color();

        void check_add_stroke_color_change_undo();
        struct StrokeColorChangeData {
            Vector4f newColor = {1.0f, 1.0f, 1.0f, 0.0f};
            Vector4f oldColor = {1.0f, 1.0f, 1.0f, 0.0f};
            std::unordered_map<NetworkingObjects::NetObjID, Vector4f> oldColorData;
            std::optional<CanvasEditorScreen::ColorSelectorData> guiColorSelectorData;
        } strokeColorChangeData;

        void set_to_selection(const std::vector<CanvasComponentContainer::ObjInfo*>& newSelection);
        void add_to_selection(const std::vector<CanvasComponentContainer::ObjInfo*>& newSelection);
        void calculate_aabb();
        void reset_all();
        void reset_transform_data();
        std::function<bool(const std::shared_ptr<DrawingProgramCacheBVHNode>&)> erase_select_objects_in_bvh_func(std::vector<CanvasComponentContainer::ObjInfo*>& selectedComponents, const SkPath& cC, DrawingProgramLayerManager::LayerSelector layerSelector);

        SCollision::ColliderCollection<float> camSpaceSelection;
        std::array<WorldVec, 4> selectionRectPoints;
        CoordSpaceHelperTransform selectionTransformCoords;
        std::vector<CanvasComponentContainer::ObjInfo*> selectedSet;
        SCollision::AABB<WorldScalar> initialSelectionAABB;
        DrawingProgram& drawP;

        enum class TransformOperation {
            NONE = 0,
            TRANSLATE,
            SCALE,
            ROTATE_RELOCATE_CENTER,
            ROTATE
        } transformOpHappening = TransformOperation::NONE;

        struct TranslationData {
            WorldVec startPos;
            bool translateWithKeys;
        } translateData;

        struct ScaleData {
            WorldVec startPos;
            WorldVec centerPos;
            WorldVec currentPos;
            std::array<Vector2f, 4> cornerHandlePoints;
            float sizeFactor = 1.0f; // Current size / original size
        } scaleData;

        struct RotationData {
            WorldVec centerPos;
            Vector2f centerHandlePoint;
            std::array<Vector2f, 4> cornerHandlePoints;
            double startAngle = 0.0;
            double rotationAngle = 0.0;
        } rotateData;
};
