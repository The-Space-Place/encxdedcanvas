#include "MainMenuScreen.hpp"
#include "EncxdedApp.hpp"
#include "SessionsGUI.hpp"
#include "FsUtil.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../Screens/CanvasEditorScreen.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/StringHelpers.hpp>
#ifdef __ANDROID__
    #include "../AndroidJNICalls.hpp"
#endif

namespace Encxded {

using namespace GUIStuff;
using namespace GUIStuff::ElementHelpers;
using namespace Widgets;

namespace {
    constexpr float SIDEBAR_WIDTH = 240.0f;
    constexpr float OVERLAY_SIDEBAR_BELOW_WIDTH = 760.0f;
}

MainMenuScreen::MainMenuScreen(MainProgram& m):
    Screen(m),
    app(Encxded::app(m))
{
    savePath = main.conf.configPath / "saves";
    trashPath = main.conf.configPath / "trash";
    backupsPath = main.conf.configPath / "backups";
    std::error_code ec;
    std::filesystem::create_directories(savePath, ec);
    std::filesystem::create_directories(trashPath, ec);
    refresh_safety_backups();

    section = static_cast<Section>(app.menuSection);
    if(!std::filesystem::is_directory(to_absolute(app.notesFolder), ec))
        app.notesFolder.clear();
    load_trash_index();
    refresh_notes();
    if(section == Section::SETTINGS)
        settingsGUI.on_settings_open();
}

MainMenuScreen::~MainMenuScreen() {
    app.save_now();
}

MenuData& MainMenuScreen::data() {
    return app.store.data;
}

void MainMenuScreen::changed() {
    app.store.mark_dirty();
    main.g.gui.set_to_layout();
}

void MainMenuScreen::set_section(Section s) {
    section = s;
    app.menuSection = static_cast<int>(s);
    close_menu();
    openGoal = std::nullopt;
    drag = DragState();
    switch(s) {
        case Section::NOTES:
            refresh_notes();
            break;
        case Section::TRASH:
            load_trash_index();
            break;
        case Section::SETTINGS:
            settingsGUI.on_settings_open();
            break;
        default:
            break;
    }
    if(sidebar_is_overlay())
        data().sidebarOpen = false;
    main.g.gui.set_to_layout();
}

void MainMenuScreen::update() {
    std::erase_if(main.logMessages, [&](auto& logM) {
        logM.time.update_time_since();
        if(logM.time > UserLogMessage::FADE_START_TIME) {
            main.g.gui.set_to_layout();
            return logM.time >= UserLogMessage::DISPLAY_TIME;
        }
        return false;
    });
    board_update();
    handle_pending_file_choice();
    app.update();
}

void MainMenuScreen::draw(SkCanvas* canvas) {
    canvas->clear(main.g.gui.io.theme->pageColor);
}

bool MainMenuScreen::sidebar_is_overlay() {
    return main.g.gui.io.windowSize.x() < OVERLAY_SIDEBAR_BELOW_WIDTH;
}

void MainMenuScreen::gui_layout_run() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    boardColumnBBs.clear();
    boardItemBBs.clear();
    boardMoreButtons.clear();

    CLAY_AUTO_ID({
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        },
        .backgroundColor = convert_vec4<Clay_Color>(theme.pageColor)
    }) {
        if(data().sidebarOpen && !sidebar_is_overlay())
            sidebar();
        content_area();
    }

    if(data().sidebarOpen && sidebar_is_overlay()) {
        gui.set_z_index(gui.get_z_index() + 80, [&] {
            gui.element<LayoutElement>("sidebar drawer dim", [&](LayoutElement*, const Clay_ElementId& lId) {
                CLAY(lId, {
                    .layout = {
                        .sizing = {.width = CLAY_SIZING_FIXED(gui.io.windowSize.x()), .height = CLAY_SIZING_FIXED(gui.io.windowSize.y())},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT
                    },
                    .backgroundColor = {0.0f, 0.0f, 0.0f, 0.5f},
                    .floating = {.zIndex = gui.get_z_index(), .attachTo = CLAY_ATTACH_TO_ROOT}
                }) {
                    sidebar();
                }
            }, LayoutElement::Callbacks{
                .onClick = [&](LayoutElement* l, const InputManager::MouseButtonCallbackArgs& b) {
                    if(b.down && l->mouseHovering && !l->childMouseHovering) {
                        data().sidebarOpen = false;
                        main.g.gui.set_to_layout();
                    }
                }
            });
        });
    }

    board_drag_ghost();
    open_menu_layout();
    dialogs.layout(gui);
    global_log();
    app.overlay_layout();
}

void MainMenuScreen::sidebar() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    gui.element<LayoutElement>("sidebar", [&](LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_FIXED(SIDEBAR_WIDTH + gui.io.safeWindowRect.min.x()), .height = CLAY_SIZING_GROW(0)},
                .padding = {
                    .left = static_cast<uint16_t>(gui.io.safeWindowRect.min.x() + 14),
                    .right = 14,
                    .top = static_cast<uint16_t>(gui.io.safeWindowRect.min.y() + 10),
                    .bottom = 14
                },
                .childGap = 4,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .backgroundColor = convert_vec4<Clay_Color>(theme.sidebarColor),
            .border = {.color = convert_vec4<Clay_Color>(color_mul_alpha(theme.accentColor, 0.35f)), .width = {.right = 1}}
        }) {
            icon_button(gui, "sidebar toggle", "data/icons/encxded/sidebar.svg", [&] {
                data().sidebarOpen = false;
                changed();
            }, 40.0f);
            CLAY_AUTO_ID({.layout = {.sizing = {.height = CLAY_SIZING_FIXED(12)}}}) {}
            sidebar_entry("notes", "data/icons/folder.svg", "Notes", Section::NOTES);
            sidebar_entry("schedule", "data/icons/encxded/schedule.svg", "Schedule", Section::SCHEDULE);
            sidebar_entry("goals", "data/icons/encxded/trophy.svg", "Goal Cycle", Section::GOALS);
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(24)}, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}}) {
                divider(gui, 20.0f);
            }
            button(gui, "sessions", "Sessions", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/encxded/sessions.svg", .wide = true, .centered = false, .height = 48.0f, .scale = 1.1f}, [&] {
                app.sessions->open();
                if(sidebar_is_overlay())
                    data().sidebarOpen = false;
            });
            sidebar_entry("backup", "data/icons/encxded/backup.svg", "Backup", Section::BACKUP);
            sidebar_entry("settings", "data/icons/RemixIcon/settings-3-line.svg", "Settings", Section::SETTINGS);
            sidebar_entry("trash", "data/icons/trash.svg", "Trash", Section::TRASH);
        }
    });
}

void MainMenuScreen::sidebar_entry(const char* id, const char* iconPath, const char* text, Section s) {
    button(main.g.gui, id, text, {.kind = ButtonStyle::Kind::PLAIN, .iconPath = iconPath, .wide = true, .isSelected = section == s, .centered = false, .height = 48.0f, .scale = 1.1f}, [&, s] {
        set_section(s);
    });
}

void MainMenuScreen::sidebar_toggle_if_hidden() {
    if(!data().sidebarOpen) {
        icon_button(main.g.gui, "sidebar toggle", "data/icons/encxded/sidebar.svg", [&] {
            data().sidebarOpen = true;
            changed();
        }, 40.0f);
    }
}

void MainMenuScreen::content_area() {
    auto& gui = main.g.gui;
    auto& io = gui.io;
    gui.element<LayoutElement>("content area", [&](LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {
            .layout = {
                .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                .padding = {
                    .left = static_cast<uint16_t>((data().sidebarOpen && !sidebar_is_overlay() ? 0.0f : io.safeWindowRect.min.x()) + 18),
                    .right = static_cast<uint16_t>(io.windowSize.x() - io.safeWindowRect.max.x() + 18),
                    .top = static_cast<uint16_t>(io.safeWindowRect.min.y() + 10),
                    .bottom = static_cast<uint16_t>(io.windowSize.y() - io.safeWindowRect.max.y() + 10)
                },
                .childGap = 12,
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            }
        }) {
            switch(section) {
                case Section::NOTES: notes_view(); break;
                case Section::SCHEDULE: schedule_view(); break;
                case Section::GOALS: goals_view(); break;
                case Section::BACKUP: backup_view(); break;
                case Section::SETTINGS: settings_view(); break;
                case Section::TRASH: trash_view(); break;
            }
        }
    });
}

void MainMenuScreen::page_header(const std::string& title, const std::function<void()>& leftContent, const std::function<void()>& rightContent) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(44)},
        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        .layoutDirection = CLAY_LEFT_TO_RIGHT
    }}) {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 6, .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
            sidebar_toggle_if_hidden();
            if(leftContent)
                leftContent();
        }
        if(!title.empty())
            label(gui, title, theme.frontColor1, GUI_FONT_BOLD, 1.3f);
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 8, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT, .y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
            if(rightContent)
                rightContent();
        }
    }
}

void MainMenuScreen::global_log() {
    auto& gui = main.g.gui;
    auto& io = gui.io;
    float width = std::min(320.0f, io.safeWindowRect.width() - 20.0f);
    gui.set_z_index(gui.get_z_index() + 200, [&] {
        gui.new_id("encxded menu log", [&] {
            CLAY_AUTO_ID({
                .layout = {
                    .sizing = {.width = CLAY_SIZING_FIXED(width), .height = CLAY_SIZING_FIT(0)},
                    .childGap = io.theme->childGap1,
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .offset = {io.safeWindowRect.min.x() + 12.0f, -(io.windowSize.y() - io.safeWindowRect.max.y()) - 12.0f},
                    .zIndex = gui.get_z_index(),
                    .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM},
                    .attachTo = CLAY_ATTACH_TO_ROOT
                }
            }) {
                for(size_t i = 0; i < main.logMessages.size(); i++) {
                    auto& logM = main.logMessages[i];
                    if(logM.time >= UserLogMessage::DISPLAY_TIME)
                        break;
                    if(!logM.should_display(true))
                        continue;
                    float a = 1.0f - lerp_time<float>(logM.time, UserLogMessage::DISPLAY_TIME, UserLogMessage::FADE_START_TIME);
                    CLAY_AUTO_ID({
                        .layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = CLAY_PADDING_ALL(10)},
                        .backgroundColor = convert_vec4<Clay_Color>(color_mul_alpha(io.theme->cardColor, a)),
                        .cornerRadius = CLAY_CORNER_RADIUS(10)
                    }) {
                        label(gui, logM.text, color_mul_alpha(logM.color == UserLogMessage::COLOR_ERROR ? io.theme->errorColor : io.theme->frontColor1, a));
                    }
                }
            }
        });
    });
}

void MainMenuScreen::open_menu(OpenMenu::Kind kind, ItemID id, const std::string& path) {
    openMenu.kind = kind;
    openMenu.id = id;
    openMenu.path = path;
    openMenu.anchor = nullptr;
    main.g.gui.set_to_layout();
}

bool MainMenuScreen::menu_is_open(OpenMenu::Kind kind, ItemID id, const std::string& path) const {
    return openMenu.kind == kind && openMenu.id == id && openMenu.path == path;
}

void MainMenuScreen::anchor_menu(SelectableButton* b, OpenMenu::Kind kind, ItemID id, const std::string& path) {
    if(menu_is_open(kind, id, path))
        openMenu.anchor = b;
}

void MainMenuScreen::close_menu() {
    openMenu = OpenMenu();
    main.g.gui.set_to_layout();
}

void MainMenuScreen::open_menu_layout() {
    if(openMenu.kind == OpenMenu::Kind::NONE)
        return;
    if(!openMenu.anchor) { // The button scrolled away or its item is gone
        openMenu = OpenMenu();
        return;
    }
    switch(openMenu.kind) {
        case OpenMenu::Kind::NONE: break;
        case OpenMenu::Kind::NOTES_HEADER: notes_header_menu(); break;
        case OpenMenu::Kind::NOTES_ENTRY: notes_entry_menu(); break;
        case OpenMenu::Kind::TRASH_ENTRY: trash_entry_menu(); break;
        case OpenMenu::Kind::SCHEDULE_HEADER: schedule_header_menu(); break;
        case OpenMenu::Kind::SCHEDULE_COLUMN: schedule_column_menu(); break;
        case OpenMenu::Kind::SCHEDULE_CARD: schedule_card_menu(); break;
        case OpenMenu::Kind::SCHEDULE_TEMPLATE: schedule_template_menu(); break;
        case OpenMenu::Kind::GOALS_HEADER: goals_header_menu(); break;
        case OpenMenu::Kind::GOAL_COLUMN: goal_column_menu(); break;
        case OpenMenu::Kind::GOAL_CARD: goal_card_menu(); break;
        case OpenMenu::Kind::GOAL_DETAIL: goal_detail_menu(); break;
        case OpenMenu::Kind::GOAL_ENTRY: goal_entry_menu(); break;
        case OpenMenu::Kind::GOAL_TEMPLATE: goal_template_menu(); break;
    }
}

std::string MainMenuScreen::to_relative(const std::filesystem::path& p) {
    std::error_code ec;
    return utf8(std::filesystem::relative(p, savePath, ec).generic_u8string());
}

std::filesystem::path MainMenuScreen::to_absolute(const std::string& rel) {
    if(rel.empty())
        return savePath;
    return savePath / fs_path(rel);
}

std::string MainMenuScreen::display_name(const std::string& relPath) {
    std::filesystem::path p = fs_path(relPath);
    return utf8(p.extension() == fs_path(World::DOT_FILE_EXTENSION) ? p.stem().u8string() : p.filename().u8string());
}

bool MainMenuScreen::canvas_exists(const std::string& relPath) {
    std::error_code ec;
    return !relPath.empty() && std::filesystem::is_regular_file(to_absolute(relPath), ec);
}

void MainMenuScreen::open_canvas(const std::string& relPath) {
    if(!canvas_exists(relPath)) {
        Logger::get().log(Logger::LogType::USERINFO, "Couldn't find the canvas \"" + display_name(relPath) + "\". It may have been moved to the Trash.");
        return;
    }
    CustomEvents::emit_event<CustomEvents::OpenInfiniPaintFileEvent>({
        .isClient = false,
        .saveThumbnail = true,
        .filePathSource = to_absolute(relPath)
    });
}

void MainMenuScreen::create_canvas_in(const std::string& folderRel) {
    CustomEvents::emit_event<CustomEvents::OpenInfiniPaintFileEvent>({
        .isClient = false,
        .saveThumbnail = true,
        .filePathEmptyAutoSaveDir = to_absolute(folderRel)
    });
}

void MainMenuScreen::create_and_open_canvas(const std::string& folderRel, const std::function<void(const std::string& relPath)>& onCreated) {
    std::shared_ptr<World> newWorld = World::create_world_ptr_from_event(main, {
        .isClient = false,
        .saveThumbnail = true,
        .filePathEmptyAutoSaveDir = to_absolute(folderRel)
    });
    if(!newWorld)
        return;
    if(onCreated)
        onCreated(to_relative(newWorld->filePath));
    app.store.mark_dirty();
    app.save_now();
    main.set_screen([&, newWorld](std::unique_ptr<Screen>) { return std::make_unique<CanvasEditorScreen>(main, true, std::vector<std::shared_ptr<World>>{newWorld}); });
}

void MainMenuScreen::input_open_infinipaint_file_callback(const CustomEvents::OpenInfiniPaintFileEvent& openFile) {
    std::shared_ptr<World> newWorld = World::create_world_ptr_from_event(main, openFile);
    if(newWorld) {
        app.save_now();
        main.set_screen([&, newWorld](std::unique_ptr<Screen>) { return std::make_unique<CanvasEditorScreen>(main, true, std::vector<std::shared_ptr<World>>{newWorld}); });
    }
}

void MainMenuScreen::input_mobile_import_canvas_callback(const CustomEvents::MobileImportCanvasEvent& mobileImport) {
#ifdef __ANDROID__
    std::string name = AndroidJNICalls::getFileNameFromURI(mobileImport.filePath.string());
    if(name.empty())
        name = "Imported Canvas";
    name = utf8(fs_path(name).stem().u8string());
#else
    std::string name = utf8(mobileImport.filePath.stem().u8string());
#endif
    std::filesystem::path newPath = sdl_safe_copy_file(to_absolute(app.notesFolder), mobileImport.filePath, name, World::FILE_EXTENSION);
    refresh_notes();
    CustomEvents::emit_event<CustomEvents::OpenInfiniPaintFileEvent>({
        .isClient = false,
        .saveThumbnail = true,
        .filePathSource = newPath
    });
}

std::vector<std::string> MainMenuScreen::all_canvases() {
    std::vector<std::string> toRet;
    std::error_code ec;
    for(auto it = std::filesystem::recursive_directory_iterator(savePath, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if(it->is_regular_file(ec) && it->path().extension() == fs_path(World::DOT_FILE_EXTENSION))
            toRet.emplace_back(to_relative(it->path()));
    }
    std::sort(toRet.begin(), toRet.end(), [](const std::string& a, const std::string& b) { return natural_less(a, b); });
    return toRet;
}

std::vector<std::string> MainMenuScreen::all_folders() {
    std::vector<std::string> toRet;
    std::error_code ec;
    for(auto it = std::filesystem::recursive_directory_iterator(savePath, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if(it->is_directory(ec))
            toRet.emplace_back(to_relative(it->path()));
    }
    std::sort(toRet.begin(), toRet.end(), [](const std::string& a, const std::string& b) { return natural_less(a, b); });
    return toRet;
}

void MainMenuScreen::pick_canvas(const std::string& title, bool allowNone, const std::function<void(const std::optional<std::string>& relPath, bool createNew)>& onPick) {
    std::vector<std::string> canvases = all_canvases();
    dialogs.custom(title, 520.0f, [this, canvases, allowNone, onPick](GUIManager& gui) {
        auto& theme = *gui.io.theme;
        auto pick = [this, onPick](std::optional<std::string> rel, bool createNew) {
            dialogs.close();
            if(onPick) onPick(rel, createNew);
        };
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(std::min(380.0f, gui.io.safeWindowRect.height() - 200.0f))}}}) {
            gui.clipping_element<ScrollArea>("canvas picker", ScrollArea::Options{
                .scrollVertical = true,
                .clipVertical = true,
                .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 6}, .childGap = 2, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                        button(gui, "new canvas", "New canvas", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/encxded/compose.svg", .wide = true, .centered = false, .height = 44.0f}, [pick] { pick(std::nullopt, true); });
                        if(allowNone)
                            button(gui, "no canvas", "No canvas (title only)", {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/minus.svg", .wide = true, .centered = false, .height = 44.0f}, [pick] { pick(std::nullopt, false); });
                        divider(gui);
                        if(canvases.empty())
                            label(gui, "No canvases yet.", theme.frontColor2);
                        for(size_t i = 0; i < canvases.size(); i++) {
                            gui.new_id(static_cast<int64_t>(i), [&] {
                                std::string rel = canvases[i];
                                std::string folder = utf8(fs_path(rel).parent_path().generic_u8string());
                                std::string text = display_name(rel) + (folder.empty() ? "" : "   (" + folder + ")");
                                button(gui, "canvas", text, {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/encxded/document.svg", .wide = true, .centered = false, .height = 44.0f}, [pick, rel] { pick(rel, false); });
                            });
                        }
                    }
                }
            });
        }
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}}) {
            button(gui, "cancel", "Cancel", {.kind = ButtonStyle::Kind::SOFT}, [this] { dialogs.close(); });
        }
    });
}

void MainMenuScreen::input_global_back_button_callback() {
    auto& gui = main.g.gui;
    if(app.input_global_back_button())
        return;
    if(dialogs.is_open())
        dialogs.close();
    else if(openMenu.kind != OpenMenu::Kind::NONE)
        close_menu();
    else if(data().sidebarOpen && sidebar_is_overlay())
        data().sidebarOpen = false;
    else if(section == Section::GOALS && openGoal.has_value())
        openGoal = std::nullopt;
    else if(section == Section::NOTES && !app.notesFolder.empty())
        navigate_notes(utf8(fs_path(app.notesFolder).parent_path().generic_u8string()));
    else if(section != Section::NOTES)
        set_section(Section::NOTES);
    else
        main.setToQuit = true;
    gui.set_to_layout();
}

void MainMenuScreen::input_app_about_to_go_to_background_callback() {
    app.save_now();
}

bool MainMenuScreen::input_keybind_callback(const Vector2ui32& newKey) {
    if(section == Section::SETTINGS)
        return settingsGUI.input_keybind_callback(main, newKey);
    return false;
}

void MainMenuScreen::settings_view() {
    auto& gui = main.g.gui;
    page_header("", [&] { settingsGUI.top_tabs_layout(main); }, nullptr);
    gui.clipping_element<ScrollArea>("settings scroll area", ScrollArea::Options{
        .scrollVertical = true,
        .clipVertical = true,
        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
        .innerContent = [&](auto&) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                settingsGUI.inner_layout(main, 300.0f, true, nullptr);
            }
        }
    });
}

}
