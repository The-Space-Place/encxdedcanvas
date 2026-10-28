#include "EncxdedApp.hpp"
#include "SessionsGUI.hpp"
#include "MainMenuScreen.hpp"
#include "MenuWidgets.hpp"
#include "../MainProgram.hpp"
#include "Helpers/ConvertVec.hpp"

namespace Encxded {

namespace {
    // Export frame budget per app frame: gentle while a canvas is open, close to full speed on the main menu
    constexpr std::chrono::milliseconds EXPORT_BUDGET_ON_CANVAS{8};
    constexpr std::chrono::milliseconds EXPORT_BUDGET_ON_MENU{250};
}

App::App(MainProgram& initMain):
    main(initMain)
{
    store.load(main.conf.configPath);
    sessions = std::make_unique<SessionsGUI>(main, store);
}

App::~App() = default;

void App::update() {
    sessions->update();
    store.update();
    if(exportJob) {
        bool onMenu = dynamic_cast<MainMenuScreen*>(main.get_screen()) != nullptr;
        float before = exportJob->progress();
        if(!exportJob->step(onMenu ? EXPORT_BUDGET_ON_MENU : EXPORT_BUDGET_ON_CANVAS))
            exportJob = nullptr;
        if(!exportJob || static_cast<int>(before * 100) != static_cast<int>(exportJob->progress() * 100))
            main.g.gui.set_to_layout();
    }
}

bool App::start_video_export(const std::filesystem::path& timelinePath, const VideoExportOptions& options) {
    if(exportJob)
        return false;
    exportJob = std::make_unique<VideoExportJob>(main, timelinePath, options);
    if(exportJob->is_done())
        exportJob = nullptr; // Failed to start; the error was already shown
    main.g.gui.set_to_layout();
    return true;
}

void App::overlay_layout() {
    sessions->layout();
    export_pill_layout();
}

void App::export_pill_layout() {
    if(!exportJob)
        return;
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    gui.set_z_index(gui.get_z_index() + 90, [&] {
        gui.element<GUIStuff::LayoutElement>("encxded export pill", [&](GUIStuff::LayoutElement*, const Clay_ElementId& lId) {
            CLAY(lId, {
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIT(0), .height = CLAY_SIZING_FIXED(40)},
                    .padding = {.left = 14, .right = 4},
                    .childGap = 8,
                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                    .layoutDirection = CLAY_LEFT_TO_RIGHT
                },
                .backgroundColor = convert_vec4<Clay_Color>(theme.cardColor),
                .cornerRadius = CLAY_CORNER_RADIUS(20),
                .floating = {
                    .offset = {0, -(gui.io.windowSize.y() - gui.io.safeWindowRect.max.y()) - 12.0f},
                    .zIndex = gui.get_z_index(),
                    .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM, .parent = CLAY_ATTACH_POINT_CENTER_BOTTOM},
                    .attachTo = CLAY_ATTACH_TO_ROOT
                }
            }) {
                Widgets::label(gui, "Exporting video  " + std::to_string(static_cast<int>(exportJob->progress() * 100)) + "%", theme.frontColor1);
                Widgets::button(gui, "cancel export", "Cancel", {.kind = Widgets::ButtonStyle::Kind::PLAIN, .height = 32.0f}, [&] {
                    exportJob->cancel();
                    exportJob = nullptr;
                    main.g.gui.set_to_layout();
                });
            }
        });
    });
}

bool App::input_global_back_button() {
    return sessions->input_global_back_button();
}

void App::save_now() {
    store.save();
}

App& app(MainProgram& main) {
    static std::unique_ptr<App> instance;
    if(!instance)
        instance = std::make_unique<App>(main);
    return *instance;
}

}
