// EncxdedCanvas: recording controls in the canvas editor (Phase 7). The record button lives in the top toolbar; while
// recording, a red pill at the top shows the elapsed time with pause/resume, mic on/off and stop.
#include "../CanvasEditorScreen.hpp"
#include "../../MainProgram.hpp"
#include "../../World.hpp"
#include "../../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../../GUIStuff/Elements/LayoutElement.hpp"
#include "../../Encxded/EncxdedApp.hpp"
#include "../../Encxded/MenuWidgets.hpp"
#include <Helpers/Logger.hpp>

using namespace GUIStuff;
using namespace ElementHelpers;

namespace {
    constexpr float PILL_HEIGHT = 40.0f;
    const SkColor4f RECORD_RED{0.90f, 0.22f, 0.24f, 1.0f};

    std::string elapsed_string(int64_t ns) {
        int64_t s = ns / 1000000000LL;
        auto two = [](int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
        if(s >= 3600)
            return std::to_string(s / 3600) + ":" + two((s / 60) % 60) + ":" + two(s % 60);
        return two(s / 60) + ":" + two(s % 60);
    }
}

void CanvasEditorScreen::start_recording() {
    if(recorder || !world)
        return;
    mobile_save_file(); // The recording sits next to the canvas file, so make sure it exists
    auto newRecorder = std::make_unique<Encxded::Recorder>(main, *world, Encxded::app(main).store.data.recordMic);
    if(!newRecorder->error().empty()) {
        Logger::get().log(Logger::LogType::USERINFO, newRecorder->error());
        return;
    }
    recorder = std::move(newRecorder);
    main.g.gui.set_to_layout();
}

void CanvasEditorScreen::stop_recording(bool offerExport) {
    if(!recorder)
        return;
    recorder->stop();
    std::string name = recorder->meta().name;
    recorder = nullptr;
    Logger::get().log(Logger::LogType::USERINFO, "Recording saved (" + name + ")");
    if(offerExport && world) {
        // "Export now": the export panel, on the Video tab where the new recording is listed first
        Encxded::app(main).exportPanelVideoTab = true;
        world->drawProg.switch_to_tool(DrawingProgramToolType::SCREENSHOT);
        set_settings_menu_popup_mode_with_color_picker_data(SettingsMenuPopup::SETTINGS);
    }
    main.g.gui.set_to_layout();
}

void CanvasEditorScreen::recording_pill() {
    if(!recorder)
        return;
    auto& gui = main.g.gui;
    auto& io = gui.io;

    gui.set_z_index(gui.get_z_index() + 50, [&] {
        gui.element<LayoutElement>("recording pill", [&](LayoutElement* l, const Clay_ElementId& lId) {
            // Keep the pill itself out of the video (its bounds are from the previous layout, which is where it's drawn)
            if(l->get_bb().has_value()) {
                SCollision::AABB<float> bb = l->get_bb().value();
                recorder->set_ui_exclusion(SCollision::AABB<float>(bb.min * io.guiScaleMultiplier, bb.max * io.guiScaleMultiplier));
            }
            CLAY(lId, {
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIXED(PILL_HEIGHT)},
                    .padding = {.left = 12, .right = 4},
                    .childGap = 4,
                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                },
                .backgroundColor = convert_vec4<Clay_Color>(color_mul_alpha(RECORD_RED, recorder->is_paused() ? 0.6f : 1.0f)),
                .cornerRadius = CLAY_CORNER_RADIUS(PILL_HEIGHT * 0.5f),
                .floating = {
                    // Below the session timer pill when a focus session is running (it takes the same spot)
                    .offset = {0.0f, io.safeWindowRect.min.y() + 8.0f + (Encxded::app(main).store.data.sessions.active.active ? 48.0f : 0.0f)},
                    .zIndex = gui.get_z_index(),
                    .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP, .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                    .attachTo = CLAY_ATTACH_TO_ROOT
                }
            }) {
                const SkColor4f white{1.0f, 1.0f, 1.0f, 1.0f};
                Encxded::Widgets::label(gui, recorder->is_paused() ? "Paused  " + elapsed_string(recorder->elapsed_ns()) : elapsed_string(recorder->elapsed_ns()), white, GUI_FONT_BOLD);
                Encxded::Widgets::tinted_icon_button(gui, "pause", recorder->is_paused() ? "data/icons/encxded/play.svg" : "data/icons/encxded/pause.svg", white, [&] {
                    recorder->set_paused(!recorder->is_paused());
                    gui.set_to_layout();
                }, 32.0f);
                Encxded::Widgets::tinted_icon_button(gui, "mic", recorder->mic_on() ? "data/icons/encxded/mic.svg" : "data/icons/encxded/mic-off.svg", white, [&] {
                    bool on = !recorder->mic_on();
                    recorder->set_mic(on);
                    auto& app = Encxded::app(main);
                    app.store.data.recordMic = on;
                    app.store.mark_dirty();
                    gui.set_to_layout();
                }, 32.0f);
                Encxded::Widgets::tinted_icon_button(gui, "stop", "data/icons/encxded/stop.svg", white, [&] {
                    stop_recording();
                }, 32.0f);
            }
        });
    });
}
