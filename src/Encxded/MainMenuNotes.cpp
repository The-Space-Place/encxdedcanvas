// EncxdedCanvas: Notes (folder/canvas list from planning-files/folders3) and Trash
#include "MainMenuScreen.hpp"
#include "EncxdedApp.hpp"
#include "SessionsGUI.hpp"
#include "FsUtil.hpp"
#include "Recording.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include "Helpers/ConvertVec.hpp"
#include "Helpers/MathExtras.hpp"
#include <Helpers/Logger.hpp>
#ifdef __ANDROID__
    #include "../AndroidJNICalls.hpp"
#endif

namespace Encxded {

using namespace GUIStuff;
using namespace GUIStuff::ElementHelpers;
using namespace Widgets;

namespace {
    std::filesystem::path thumbnail_of(const std::filesystem::path& canvasPath) {
        std::filesystem::path t = canvasPath;
        return t.replace_extension(".jpg");
    }

    // Renames a canvas along with its thumbnail and recordings
    void rename_canvas_file(const std::filesystem::path& from, const std::filesystem::path& to) {
        std::filesystem::rename(from, to);
        std::error_code ec;
        if(std::filesystem::exists(thumbnail_of(from), ec))
            std::filesystem::rename(thumbnail_of(from), thumbnail_of(to), ec);
        if(std::filesystem::exists(Recording::folder_for_canvas(from), ec))
            std::filesystem::rename(Recording::folder_for_canvas(from), Recording::folder_for_canvas(to), ec);
    }

    void log_error(const std::string& what, const std::exception& e) {
        Logger::get().log(Logger::LogType::USERINFO, what + ": " + e.what());
    }
}

void MainMenuScreen::refresh_notes() {
    notesEntries.clear();
    std::filesystem::path folder = to_absolute(app.notesFolder);
    std::error_code ec;
    for(auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& p = it->path();
        std::string name = utf8(p.filename().u8string());
        if(name.empty() || name[0] == '.' || Recording::is_recordings_folder(p))
            continue;
        NotesEntry e;
        e.relPath = to_relative(p);
        e.modified = it->last_write_time(ec);
        if(it->is_directory(ec)) {
            e.isFolder = true;
            e.name = name;
            for(auto c = std::filesystem::directory_iterator(p, ec); !ec && c != std::filesystem::directory_iterator(); c.increment(ec)) {
                if((c->is_directory(ec) && !Recording::is_recordings_folder(c->path())) || c->path().extension() == fs_path(World::DOT_FILE_EXTENSION))
                    e.childCount++;
            }
            ec.clear();
        }
        else if(p.extension() == fs_path(World::DOT_FILE_EXTENSION)) {
            e.name = utf8(p.stem().u8string());
            e.size = it->file_size(ec);
        }
        else
            continue;
        notesEntries.push_back(e);
    }
    NotesSort sort = data().notesSort;
    std::stable_sort(notesEntries.begin(), notesEntries.end(), [sort](const NotesEntry& a, const NotesEntry& b) {
        if(a.isFolder != b.isFolder)
            return a.isFolder;
        switch(sort) {
            case NotesSort::NAME_ASCENDING: return natural_less(a.name, b.name);
            case NotesSort::NAME_DESCENDING: return natural_less(b.name, a.name);
            case NotesSort::MODIFIED_NEWEST: return a.modified > b.modified;
            case NotesSort::MODIFIED_OLDEST: return a.modified < b.modified;
        }
        return false;
    });
    main.g.gui.set_to_layout();
}

void MainMenuScreen::navigate_notes(const std::string& folderRel, bool recordHistory) {
    if(folderRel == app.notesFolder)
        return;
    if(recordHistory) {
        notesBackHistory.push_back(app.notesFolder);
        notesForwardHistory.clear();
    }
    app.notesFolder = folderRel;
    close_menu();
    refresh_notes();
}

void MainMenuScreen::notes_view() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;

    page_header("", [&] {
        icon_button(gui, "notes back", "data/icons/encxded/chevron-left.svg", [&] {
            if(notesBackHistory.empty())
                return;
            notesForwardHistory.push_back(app.notesFolder);
            std::string to = notesBackHistory.back();
            notesBackHistory.pop_back();
            navigate_notes(to, false);
        }, 40.0f);
        icon_button(gui, "notes forward", "data/icons/encxded/chevron-right.svg", [&] {
            if(notesForwardHistory.empty())
                return;
            notesBackHistory.push_back(app.notesFolder);
            std::string to = notesForwardHistory.back();
            notesForwardHistory.pop_back();
            navigate_notes(to, false);
        }, 40.0f);
    }, [&] {
        bool sessionActive = data().sessions.active.active;
        button(gui, "focus", sessionActive ? "Session" : "Focus", {.kind = ButtonStyle::Kind::TINT, .iconPath = "data/icons/encxded/play.svg", .height = 40.0f, .scale = 1.05f}, [&] {
            app.sessions->open();
        });
        icon_button(gui, "new folder", "data/icons/encxded/folder-add.svg", [&] { new_folder(); }, 40.0f);
        anchor_menu(more_button(gui, "notes more", [&] { open_menu(OpenMenu::Kind::NOTES_HEADER); }, 40.0f), OpenMenu::Kind::NOTES_HEADER);
    });

    // Breadcrumbs: Notes > Folder > Subfolder
    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
        .childGap = 8,
        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        .layoutDirection = CLAY_LEFT_TO_RIGHT
    }}) {
        std::vector<std::pair<std::string, std::string>> crumbs = {{"Notes", ""}};
        std::string accumulated;
        for(const auto& part : fs_path(app.notesFolder)) {
            std::string partStr = utf8(part.u8string());
            if(partStr.empty())
                continue;
            accumulated = accumulated.empty() ? partStr : accumulated + "/" + partStr;
            crumbs.emplace_back(partStr, accumulated);
        }
        for(size_t i = 0; i < crumbs.size(); i++) {
            gui.new_id(static_cast<int64_t>(i), [&] {
                if(i > 0)
                    label(gui, ">", theme.frontColor2);
                std::string target = crumbs[i].second;
                button(gui, "crumb", crumbs[i].first, {.kind = ButtonStyle::Kind::SOFT, .height = 38.0f, .fontId = i + 1 == crumbs.size() ? GUI_FONT_BOLD : GUI_FONT_REGULAR}, [&, target] {
                    navigate_notes(target);
                });
            });
        }
    }

    gui.element<LayoutElement>("notes list area", [&](LayoutElement*, const Clay_ElementId& lId) {
        CLAY(lId, {.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)}}}) {
            gui.clipping_element<ScrollArea>("notes list", ScrollArea::Options{
                .scrollVertical = true,
                .clipVertical = true,
                .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)},
                        .padding = {.right = 8, .bottom = 90},
                        .layoutDirection = CLAY_TOP_TO_BOTTOM
                    }}) {
                        if(notesEntries.empty()) {
                            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(24), .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
                                label(gui, app.notesFolder.empty() ? "No canvases yet. Tap the yellow button to make one." : "This folder is empty.", theme.frontColor2);
                            }
                        }
                        for(size_t i = 0; i < notesEntries.size(); i++)
                            gui.new_id(static_cast<int64_t>(i), [&] { notes_row(notesEntries[i]); });
                    }
                }
            });
            floating_action_button(gui, "new canvas", "data/icons/encxded/compose.svg", [&] {
                create_canvas_in(app.notesFolder);
            });
        }
    });
}

void MainMenuScreen::notes_row(const NotesEntry& e) {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    std::string rel = e.relPath;
    bool isFolder = e.isFolder;
    CLAY_AUTO_ID({.layout = {
        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(62)},
        .childGap = 4,
        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        .layoutDirection = CLAY_LEFT_TO_RIGHT
    }}) {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(58)}}}) {
            gui.element<SelectableButton>("row", SelectableButton::Data{
                .drawType = SelectableButton::DrawType::TRANSPARENT_ALL,
                .onClick = [&, rel, isFolder] {
                    if(isFolder)
                        navigate_notes(rel);
                    else
                        open_canvas(rel);
                },
                .innerContent = [&](const SelectableButton::InnerContentCallbackParameters&) {
                    CLAY_AUTO_ID({.layout = {
                        .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
                        .padding = {.left = 12, .right = 8},
                        .childGap = 16,
                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                        .layoutDirection = CLAY_LEFT_TO_RIGHT
                    }}) {
                        icon(gui, "icon", isFolder ? "data/icons/folder.svg" : "data/icons/encxded/document.svg", 28.0f, theme.frontColor2);
                        label(gui, e.name, theme.frontColor1, GUI_FONT_REGULAR, 1.12f);
                        spacer(gui);
                        if(isFolder)
                            label(gui, e.childCount == 1 ? "1 item" : std::to_string(e.childCount) + " items", theme.frontColor2);
                        else
                            label(gui, human_file_size(e.size), theme.frontColor2);
                    }
                }
            });
        }
        anchor_menu(more_button(gui, "more", [&, rel, isFolder] {
            open_menu(OpenMenu::Kind::NOTES_ENTRY, isFolder ? 1 : 0, rel);
        }, 40.0f), OpenMenu::Kind::NOTES_ENTRY, isFolder ? 1 : 0, rel);
    }
    divider(gui, 56.0f);
}

void MainMenuScreen::notes_entry_menu() {
    std::string rel = openMenu.path;
    bool isFolder = openMenu.id == 1;
    std::vector<MenuEntry> entries;
    if(isFolder) {
        entries.push_back({"Open", [&, rel] { navigate_notes(rel); }});
        entries.push_back({"Rename", [&, rel] { rename_entry(rel, true); }});
        entries.push_back({"Move to...", [&, rel] { move_entry_dialog(rel, true); }});
        entries.push_back({"Move to Trash", [&, rel] { move_to_trash(rel, true); }});
    }
    else {
        entries.push_back({"Open", [&, rel] { open_canvas(rel); }});
        entries.push_back({"Rename", [&, rel] { rename_entry(rel, false); }});
        entries.push_back({"Move to...", [&, rel] { move_entry_dialog(rel, false); }});
        entries.push_back({"Duplicate", [&, rel] { duplicate_canvas(rel); }});
        entries.push_back({"Add to Schedule", [&, rel] {
            ScheduleCard card;
            card.id = data().new_id();
            card.columnId = data().scheduleColumns.front().id;
            card.title = display_name(rel);
            card.canvasPath = rel;
            card.enteredColumnTime = now_time();
            data().scheduleCards.push_back(card);
            changed();
            Logger::get().log(Logger::LogType::USERINFO, "Added \"" + card.title + "\" to " + data().scheduleColumns.front().name);
        }});
#ifdef __ANDROID__
        entries.push_back({"Share", [&, rel] {
            AndroidJNICalls::shareInternalFiles({"saves/" + rel}, "application/octet-stream", true);
        }});
#endif
        entries.push_back({"Move to Trash", [&, rel] { move_to_trash(rel, false); }});
    }
    popup_menu(main.g.gui, "notes entry menu", openMenu.anchor, entries, [&] { close_menu(); });
}

void MainMenuScreen::notes_header_menu() {
    std::vector<MenuEntry> entries;
    auto setSort = [&](NotesSort s) {
        return [&, s] {
            data().notesSort = s;
            changed();
            refresh_notes();
        };
    };
    entries.push_back({"Sort: Name A-Z", setSort(NotesSort::NAME_ASCENDING)});
    entries.push_back({"Sort: Name Z-A", setSort(NotesSort::NAME_DESCENDING)});
    entries.push_back({"Sort: Newest first", setSort(NotesSort::MODIFIED_NEWEST)});
    entries.push_back({"Sort: Oldest first", setSort(NotesSort::MODIFIED_OLDEST)});
    entries.push_back({"Import canvas (.infpnt)", [&] {
        open_file_selector("Import Canvas", {{"InfiniPaint Canvas", World::FILE_EXTENSION}}, [](const std::filesystem::path& p, const auto&) {
            CustomEvents::emit_event<CustomEvents::MobileImportCanvasEvent>({.filePath = p});
        });
    }});
    popup_menu(main.g.gui, "notes header menu", openMenu.anchor, entries, [&] { close_menu(); }, 250.0f);
}

void MainMenuScreen::new_folder() {
    dialogs.prompt_text("New folder", "", "Create", [&](const std::string& name) {
        if(!is_valid_entry_name(name)) {
            Logger::get().log(Logger::LogType::USERINFO, "Folder names can't be empty or contain / \\ : * ? \" < > |");
            return;
        }
        std::filesystem::path p = to_absolute(app.notesFolder) / fs_path(name);
        std::error_code ec;
        if(std::filesystem::exists(p, ec)) {
            Logger::get().log(Logger::LogType::USERINFO, "Something named \"" + name + "\" already exists here");
            return;
        }
        std::filesystem::create_directory(p, ec);
        if(ec)
            Logger::get().log(Logger::LogType::USERINFO, "Couldn't create the folder: " + ec.message());
        refresh_notes();
    });
}

void MainMenuScreen::rename_entry(const std::string& relPath, bool isFolder) {
    dialogs.prompt_text(isFolder ? "Rename folder" : "Rename canvas", display_name(relPath), "Rename", [&, relPath, isFolder](const std::string& name) {
        if(!is_valid_entry_name(name)) {
            Logger::get().log(Logger::LogType::USERINFO, "Names can't be empty or contain / \\ : * ? \" < > |");
            return;
        }
        std::filesystem::path from = to_absolute(relPath);
        std::filesystem::path to = from.parent_path() / fs_path(isFolder ? name : name + World::DOT_FILE_EXTENSION);
        if(from == to)
            return;
        std::error_code ec;
        if(std::filesystem::exists(to, ec)) {
            Logger::get().log(Logger::LogType::USERINFO, "Something named \"" + name + "\" already exists here");
            return;
        }
        try {
            if(isFolder)
                std::filesystem::rename(from, to);
            else
                rename_canvas_file(from, to);
            data().rename_canvas_links(relPath, to_relative(to), isFolder);
            changed();
        }
        catch(const std::exception& e) {
            log_error("Couldn't rename", e);
        }
        refresh_notes();
    });
}

void MainMenuScreen::move_entry_dialog(const std::string& relPath, bool isFolder) {
    std::vector<std::string> folders = all_folders();
    // A folder can't move into itself or its subfolders
    if(isFolder)
        std::erase_if(folders, [&](const std::string& f) { return f == relPath || (f.size() > relPath.size() && f.compare(0, relPath.size(), relPath) == 0 && f[relPath.size()] == '/'); });
    folders.insert(folders.begin(), "");
    std::string currentParent = utf8(fs_path(relPath).parent_path().generic_u8string());
    dialogs.custom("Move \"" + display_name(relPath) + "\" to", 460.0f, [this, folders, relPath, isFolder, currentParent](GUIManager& gui) {
        CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(std::min(340.0f, gui.io.safeWindowRect.height() - 200.0f))}}}) {
            gui.clipping_element<ScrollArea>("folders", ScrollArea::Options{
                .scrollVertical = true,
                .clipVertical = true,
                .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
                .innerContent = [&](const ScrollArea::InnerContentParameters&) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 6}, .childGap = 2, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                        for(size_t i = 0; i < folders.size(); i++) {
                            gui.new_id(static_cast<int64_t>(i), [&] {
                                std::string folder = folders[i];
                                size_t depth = folder.empty() ? 0 : std::count(folder.begin(), folder.end(), '/') + 1;
                                std::string text = std::string(depth * 4, ' ') + (folder.empty() ? "Notes (top level)" : utf8(fs_path(folder).filename().u8string()));
                                button(gui, "folder", text, {.kind = ButtonStyle::Kind::PLAIN, .iconPath = "data/icons/folder.svg", .wide = true, .isSelected = folder == currentParent, .centered = false, .height = 42.0f}, [this, folder, relPath, isFolder] {
                                    dialogs.close();
                                    move_entry(relPath, isFolder, folder);
                                });
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

void MainMenuScreen::move_entry(const std::string& relPath, bool isFolder, const std::string& toFolderRel) {
    std::filesystem::path from = to_absolute(relPath);
    std::filesystem::path toDir = to_absolute(toFolderRel);
    if(from.parent_path() == toDir)
        return;
    std::filesystem::path to = isFolder ? unique_child_path(toDir, utf8(from.filename().u8string()), "") : unique_child_path(toDir, utf8(from.stem().u8string()), World::DOT_FILE_EXTENSION);
    try {
        if(isFolder)
            std::filesystem::rename(from, to);
        else
            rename_canvas_file(from, to);
        data().rename_canvas_links(relPath, to_relative(to), isFolder);
        changed();
    }
    catch(const std::exception& e) {
        log_error("Couldn't move", e);
    }
    refresh_notes();
}

void MainMenuScreen::duplicate_canvas(const std::string& relPath) {
    std::filesystem::path from = to_absolute(relPath);
    std::filesystem::path to = unique_child_path(from.parent_path(), utf8(from.stem().u8string()) + " copy", World::DOT_FILE_EXTENSION);
    try {
        std::filesystem::copy_file(from, to);
        std::error_code ec;
        if(std::filesystem::exists(thumbnail_of(from), ec))
            std::filesystem::copy_file(thumbnail_of(from), thumbnail_of(to), ec);
    }
    catch(const std::exception& e) {
        log_error("Couldn't duplicate", e);
    }
    refresh_notes();
}

void MainMenuScreen::load_trash_index() {
    std::error_code ec;
    std::filesystem::create_directories(trashPath, ec);
    auto& trash = data().trash;
    // Forget entries whose files are gone
    std::erase_if(trash, [&](const TrashEntry& t) { return !std::filesystem::exists(trashPath / fs_path(t.trashName), ec); });
    // Adopt anything already in the trash folder (e.g. from InfiniPaint's file browser)
    for(auto it = std::filesystem::directory_iterator(trashPath, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        std::string name = utf8(it->path().filename().u8string());
        bool isFolder = it->is_directory(ec);
        if((!isFolder && it->path().extension() != fs_path(World::DOT_FILE_EXTENSION)) || Recording::is_recordings_folder(it->path()))
            continue;
        if(std::any_of(trash.begin(), trash.end(), [&](const TrashEntry& t) { return t.trashName == name; }))
            continue;
        trash.push_back({name, name, isFolder, now_time()});
    }
    std::sort(trash.begin(), trash.end(), [](const TrashEntry& a, const TrashEntry& b) { return a.trashTime > b.trashTime; });
    app.store.mark_dirty();
}

void MainMenuScreen::move_to_trash(const std::string& relPath, bool isFolder) {
    std::filesystem::path from = to_absolute(relPath);
    std::filesystem::path to = isFolder ? unique_child_path(trashPath, utf8(from.filename().u8string()), "") : unique_child_path(trashPath, utf8(from.stem().u8string()), World::DOT_FILE_EXTENSION);
    try {
        if(isFolder)
            std::filesystem::rename(from, to);
        else
            rename_canvas_file(from, to);
        data().trash.insert(data().trash.begin(), {utf8(to.filename().u8string()), relPath, isFolder, now_time()});
        changed();
        Logger::get().log(Logger::LogType::USERINFO, "Moved \"" + display_name(relPath) + "\" to the Trash");
    }
    catch(const std::exception& e) {
        log_error("Couldn't move to the Trash", e);
    }
    refresh_notes();
}

void MainMenuScreen::restore_from_trash(const std::string& trashName) {
    auto& trash = data().trash;
    auto it = std::find_if(trash.begin(), trash.end(), [&](const TrashEntry& t) { return t.trashName == trashName; });
    if(it == trash.end())
        return;
    TrashEntry entry = *it;
    std::filesystem::path from = trashPath / fs_path(entry.trashName);
    std::filesystem::path original = to_absolute(entry.originalPath);
    std::error_code ec;
    std::filesystem::create_directories(original.parent_path(), ec);
    std::filesystem::path to = entry.isFolder ? unique_child_path(original.parent_path(), utf8(original.filename().u8string()), "") : unique_child_path(original.parent_path(), utf8(original.stem().u8string()), World::DOT_FILE_EXTENSION);
    try {
        if(entry.isFolder)
            std::filesystem::rename(from, to);
        else
            rename_canvas_file(from, to);
        std::string newRel = to_relative(to);
        if(newRel != entry.originalPath)
            data().rename_canvas_links(entry.originalPath, newRel, entry.isFolder);
        std::erase_if(trash, [&](const TrashEntry& t) { return t.trashName == trashName; });
        changed();
        Logger::get().log(Logger::LogType::USERINFO, "Restored \"" + display_name(newRel) + "\"");
    }
    catch(const std::exception& e) {
        log_error("Couldn't restore", e);
    }
}

void MainMenuScreen::delete_from_trash(const std::string& trashName) {
    std::filesystem::path p = trashPath / fs_path(trashName);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::remove(thumbnail_of(p), ec);
    std::filesystem::remove_all(Recording::folder_for_canvas(p), ec);
    std::erase_if(data().trash, [&](const TrashEntry& t) { return t.trashName == trashName; });
    changed();
}

void MainMenuScreen::trash_view() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;
    auto& trash = data().trash;

    page_header("Trash", nullptr, [&] {
        if(!trash.empty()) {
            button(gui, "empty trash", "Empty Trash", {.kind = ButtonStyle::Kind::SOFT, .iconPath = "data/icons/trash.svg"}, [&] {
                dialogs.confirm("Empty the Trash?", "Everything in the Trash is permanently deleted. This can't be undone.", "Delete All", [&] {
                    std::vector<std::string> names;
                    for(auto& t : data().trash)
                        names.push_back(t.trashName);
                    for(auto& n : names)
                        delete_from_trash(n);
                });
            });
        }
    });

    gui.clipping_element<ScrollArea>("trash list", ScrollArea::Options{
        .scrollVertical = true,
        .clipVertical = true,
        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
        .innerContent = [&](const ScrollArea::InnerContentParameters&) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 8}, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                if(trash.empty()) {
                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .padding = CLAY_PADDING_ALL(24), .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}}) {
                        label(gui, "The Trash is empty. Deleted canvases and folders stay here until you delete them for good.", theme.frontColor2);
                    }
                }
                for(size_t i = 0; i < trash.size(); i++) {
                    gui.new_id(static_cast<int64_t>(i), [&] {
                        const TrashEntry& t = trash[i];
                        std::string name = t.trashName;
                        std::string from = utf8(fs_path(t.originalPath).parent_path().generic_u8string());
                        CLAY_AUTO_ID({.layout = {
                            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(64)},
                            .padding = {.left = 12},
                            .childGap = 16,
                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                            .layoutDirection = CLAY_LEFT_TO_RIGHT
                        }}) {
                            icon(gui, "icon", t.isFolder ? "data/icons/folder.svg" : "data/icons/encxded/document.svg", 28.0f, theme.frontColor2);
                            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}, .childGap = 4, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                                label(gui, t.isFolder ? name : utf8(fs_path(name).stem().u8string()), theme.frontColor1, GUI_FONT_REGULAR, 1.1f);
                                label(gui, "Deleted " + short_date_string(t.trashTime) + (from.empty() ? "" : "  -  from " + from), theme.frontColor2, GUI_FONT_REGULAR, 0.9f);
                            }
                            anchor_menu(more_button(gui, "more", [&, name] { open_menu(OpenMenu::Kind::TRASH_ENTRY, 0, name); }, 40.0f), OpenMenu::Kind::TRASH_ENTRY, 0, name);
                        }
                        divider(gui, 56.0f);
                    });
                }
            }
        }
    });
}

void MainMenuScreen::trash_entry_menu() {
    std::string name = openMenu.path;
    std::vector<MenuEntry> entries;
    entries.push_back({"Restore", [&, name] { restore_from_trash(name); }});
    entries.push_back({"Delete permanently", [&, name] {
        dialogs.confirm("Delete permanently?", "\"" + utf8(fs_path(name).stem().u8string()) + "\" will be deleted. This can't be undone.", "Delete", [&, name] { delete_from_trash(name); });
    }});
    popup_menu(main.g.gui, "trash entry menu", openMenu.anchor, entries, [&] { close_menu(); });
}

}
