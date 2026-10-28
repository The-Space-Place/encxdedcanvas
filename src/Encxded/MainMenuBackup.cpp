// EncxdedCanvas: Backup page. Exports everything to one .zip and restores (merges) one.
//
// Zip layout: saves/** (canvases, folders, thumbnails), themes/**, encxdedMenu.json, config.json, palettes.json, plus
// encxdedBackup.json holding each file's modification time (zip headers only store when the backup was made).
//
// Restoring (decided in planning-files/Plan.md):
//  - A safety backup of the current state goes to backups/ first (the newest few are kept).
//  - Canvases and themes: when a path exists on both sides, the newer file wins.
//  - Menu data is merged (MenuData::merge_from). config.json and palettes.json come from the backup.
#include "MainMenuScreen.hpp"
#include "EncxdedApp.hpp"
#include "FsUtil.hpp"
#include "MiniZip.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include "../GUIStuff/Elements/ScrollArea.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/StringHelpers.hpp>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <chrono>
#ifdef __ANDROID__
    #include "../AndroidJNICalls.hpp"
#endif

namespace Encxded {

using namespace GUIStuff;
using namespace Widgets;

namespace {
    constexpr const char* MANIFEST_NAME = "encxdedBackup.json";
    constexpr const char* MENU_DATA_NAME = MenuDataStore::FILE_NAME;
    constexpr int MANIFEST_FORMAT = 1;
    constexpr size_t SAFETY_BACKUPS_TO_KEEP = 3;
    const std::vector<std::string> SETTINGS_FILES = {"config.json", "palettes.json"};

    std::string two_digits(int v) {
        return (v < 10 ? "0" : "") + std::to_string(v);
    }

    // "2026-10-27" or "2026-10-27 21-05" (no ':' so it's a valid file name everywhere)
    std::string date_stamp(bool withTime) {
        SDL_DateTime dt{};
        if(!SDL_TimeToDateTime(now_time(), &dt, true))
            return "backup";
        std::string s = std::to_string(dt.year) + "-" + two_digits(dt.month) + "-" + two_digits(dt.day);
        if(withTime)
            s += " " + two_digits(dt.hour) + "-" + two_digits(dt.minute);
        return s;
    }

    std::optional<SDL_Time> modified_time(const std::filesystem::path& p) {
        SDL_PathInfo info;
        if(!SDL_GetPathInfo(utf8(p).c_str(), &info) || info.type != SDL_PATHTYPE_FILE)
            return std::nullopt;
        return info.modify_time;
    }

    void set_modified_time(const std::filesystem::path& p, SDL_Time t) {
        using FileClock = std::filesystem::file_time_type::clock;
        std::error_code ec;
        auto offset = std::chrono::duration_cast<FileClock::duration>(std::chrono::nanoseconds(t - now_time()));
        std::filesystem::last_write_time(p, FileClock::now() + offset, ec);
    }

    // Rejects absolute paths and "..", so a crafted zip can't write outside the config folder
    bool is_safe_entry_name(const std::string& name) {
        if(name.empty() || name.front() == '/' || name.find('\\') != std::string::npos || name.find(':') != std::string::npos)
            return false;
        size_t start = 0;
        while(start <= name.size()) {
            size_t end = name.find('/', start);
            if(end == std::string::npos)
                end = name.size();
            std::string part = name.substr(start, end - start);
            if(part.empty() || part == "." || part == "..")
                return false;
            start = end + 1;
        }
        return true;
    }

    bool starts_with(const std::string& s, const char* prefix) {
        return s.rfind(prefix, 0) == 0;
    }

    void add_tree(std::vector<MiniZip::FileToAdd>& files, const std::filesystem::path& root, const std::string& zipPrefix) {
        std::error_code ec;
        for(auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if(it->is_regular_file(ec))
                files.push_back({zipPrefix + utf8(std::filesystem::relative(it->path(), root, ec).generic_u8string()), it->path()});
        }
    }

    void log_user(const std::string& text) {
        Logger::get().log(Logger::LogType::USERINFO, text);
    }

    // What restoring a zip would do, shown before confirming
    struct RestorePlan {
        std::shared_ptr<std::vector<MiniZip::ReadEntry>> entries = std::make_shared<std::vector<MiniZip::ReadEntry>>();
        nlohmann::json manifestFiles = nlohmann::json::object();
        size_t canvasesNew = 0;
        size_t canvasesNewer = 0;
        size_t canvasesKept = 0;
        bool hasMenuData = false;
        bool hasSettings = false;
    };

    std::optional<SDL_Time> manifest_time(const nlohmann::json& manifestFiles, const std::string& name) {
        auto it = manifestFiles.find(name);
        if(it == manifestFiles.end() || !it->is_number_integer())
            return std::nullopt;
        return it->get<SDL_Time>();
    }

    // Newer wins. Without a recorded time (not made by this app) an existing file is kept.
    bool should_write_file(const std::filesystem::path& dest, std::optional<SDL_Time> backupTime) {
        std::optional<SDL_Time> deviceTime = modified_time(dest);
        if(!deviceTime.has_value())
            return true;
        return backupTime.has_value() && backupTime.value() > deviceTime.value();
    }
}

void MainMenuScreen::show_backup_file_dialog(PendingFileChoice::Kind kind) {
    static const SDL_DialogFileFilter ZIP_FILTER[] = {{"Zip archive", "zip"}};
    using Holder = std::pair<std::shared_ptr<PendingFileChoice>, PendingFileChoice::Kind>;
    auto callback = [](void* userData, const char* const* fileList, int) {
        std::unique_ptr<Holder> holder(static_cast<Holder*>(userData));
        if(!fileList || !fileList[0])
            return; // Cancelled or failed
        {
            std::scoped_lock lock(holder->first->mutex);
            holder->first->result = std::make_pair(holder->second, std::string(fileList[0]));
        }
        // Wake the main loop in case it's waiting for events
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_USER;
        SDL_PushEvent(&e);
    };
    Holder* holder = new Holder(pendingFileChoice, kind);
    if(kind == PendingFileChoice::Kind::SAVE_BACKUP) {
        std::string name = "EncxdedCanvas Backup " + date_stamp(false) + ".zip";
#ifdef __ANDROID__
        AndroidJNICalls::setSaveDialogSuggestedName(name);
#endif
        std::string defaultLocation = utf8(main.homePath / fs_path(name));
        SDL_ShowSaveFileDialog(callback, holder, main.window.sdlWindow, ZIP_FILTER, 1, defaultLocation.c_str());
    }
    else
        SDL_ShowOpenFileDialog(callback, holder, main.window.sdlWindow, ZIP_FILTER, 1, nullptr, false);
}

void MainMenuScreen::handle_pending_file_choice() {
    std::optional<std::pair<PendingFileChoice::Kind, std::string>> choice;
    {
        std::scoped_lock lock(pendingFileChoice->mutex);
        choice.swap(pendingFileChoice->result);
    }
    if(!choice.has_value())
        return;
    if(choice->first == PendingFileChoice::Kind::SAVE_BACKUP)
        create_backup(choice->second);
    else
        confirm_restore(choice->second);
    main.g.gui.set_to_layout();
}

void MainMenuScreen::refresh_safety_backups() {
    safetyBackups.clear();
    std::error_code ec;
    for(auto it = std::filesystem::directory_iterator(backupsPath, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if(it->is_regular_file(ec) && it->path().extension() == ".zip")
            safetyBackups.push_back(it->path());
    }
    // Newest first (names start with the date)
    std::sort(safetyBackups.begin(), safetyBackups.end(), [](const auto& a, const auto& b) { return natural_less(utf8(b.filename()), utf8(a.filename())); });
}

std::string MainMenuScreen::write_backup(const std::filesystem::path& zipPath) {
    app.save_now();
    main.save_config();

    const std::filesystem::path& config = main.conf.configPath;
    std::vector<MiniZip::FileToAdd> files;
    add_tree(files, savePath, "saves/");
    add_tree(files, config / "themes", "themes/");
    std::error_code ec;
    for(const std::string& name : std::vector<std::string>{MENU_DATA_NAME, "config.json", "palettes.json"}) {
        if(std::filesystem::is_regular_file(config / name, ec))
            files.push_back({name, config / name});
    }

    nlohmann::json manifest;
    manifest["app"] = "EncxdedCanvas";
    manifest["format"] = MANIFEST_FORMAT;
    manifest["created"] = now_time();
    manifest["files"] = nlohmann::json::object();
    for(const auto& f : files) {
        if(std::optional<SDL_Time> t = modified_time(f.sourcePath))
            manifest["files"][f.nameInZip] = t.value();
    }
    std::filesystem::create_directories(backupsPath, ec);
    std::filesystem::path manifestPath = backupsPath / "manifest.tmp";
    std::string manifestStr = manifest.dump();
    if(!SDL_SaveFile(utf8(manifestPath).c_str(), manifestStr.data(), manifestStr.size()))
        return std::string("Couldn't prepare the backup: ") + SDL_GetError();
    files.push_back({MANIFEST_NAME, manifestPath});

    std::string error = MiniZip::write_zip(zipPath, files);
    std::filesystem::remove(manifestPath, ec);
    return error;
}

void MainMenuScreen::create_backup(const std::string& destination) {
    std::string dest = destination;
    // Desktop save dialogs don't always add the extension (content:// URIs on Android are left alone)
    if(dest.find("://") == std::string::npos && fs_path(dest).extension() != ".zip")
        dest += ".zip";
    std::string error = write_backup(fs_path(dest));
    if(error.empty()) {
        backupStatus = "Backup saved.";
        log_user("Backup saved");
    }
    else {
        backupStatus = error;
        log_user(error);
    }
}

void MainMenuScreen::confirm_restore(const std::string& zipPath, bool rollBack) {
    RestorePlan plan;
    std::string error = MiniZip::read_zip(fs_path(zipPath), *plan.entries);
    if(!error.empty()) {
        backupStatus = error;
        log_user(error);
        return;
    }

    bool hasManifest = false;
    for(const MiniZip::ReadEntry& e : *plan.entries) {
        if(e.name == MANIFEST_NAME) {
            try {
                nlohmann::json m = nlohmann::json::parse(e.data);
                plan.manifestFiles = m.at("files");
                hasManifest = true;
            }
            catch(...) {}
        }
    }
    for(const MiniZip::ReadEntry& e : *plan.entries) {
        if(!is_safe_entry_name(e.name))
            continue;
        if(e.name == MENU_DATA_NAME)
            plan.hasMenuData = true;
        else if(std::find(SETTINGS_FILES.begin(), SETTINGS_FILES.end(), e.name) != SETTINGS_FILES.end())
            plan.hasSettings = true;
        else if(starts_with(e.name, "saves/") && fs_path(e.name).extension() == fs_path(World::DOT_FILE_EXTENSION)) {
            std::filesystem::path dest = main.conf.configPath / fs_path(e.name);
            if(!modified_time(dest).has_value())
                plan.canvasesNew++;
            else if(rollBack || should_write_file(dest, manifest_time(plan.manifestFiles, e.name)))
                plan.canvasesNewer++;
            else
                plan.canvasesKept++;
        }
    }
    if(!hasManifest && !plan.hasMenuData) {
        backupStatus = "This zip isn't an EncxdedCanvas backup.";
        log_user(backupStatus);
        return;
    }

    if(rollBack) {
        dialogs.confirm("Roll back to this point?", "Every canvas, theme and setting in this safety backup replaces the current version, and the schedule, goals and sessions go back to how they were. Canvases created since then are kept. A new safety backup is saved first.",
            "Roll Back", [this, zipPath] { restore_backup(zipPath, true); });
        return;
    }

    std::string message = "Canvases: " + std::to_string(plan.canvasesNew) + " new, " + std::to_string(plan.canvasesNewer) + " updated (newer in the backup), "
        + std::to_string(plan.canvasesKept) + " kept (newer on this device).";
    if(plan.hasMenuData)
        message += " Schedule, goals and sessions from the backup are added to yours.";
    if(plan.hasSettings)
        message += " Settings and palettes are replaced with the backup's.";
    message += " A safety backup of everything is saved first.";

    dialogs.confirm("Restore this backup?", message, "Restore", [this, zipPath] { restore_backup(zipPath, false); });
}

void MainMenuScreen::restore_backup(const std::string& zipPath, bool rollBack) {
    std::vector<MiniZip::ReadEntry> entries;
    std::string error = MiniZip::read_zip(fs_path(zipPath), entries);
    if(!error.empty()) {
        backupStatus = error;
        log_user(error);
        return;
    }

    // Safety backup first; don't touch anything if it can't be made
    std::error_code ec;
    std::filesystem::create_directories(backupsPath, ec);
    std::filesystem::path safetyPath = unique_child_path(backupsPath, date_stamp(true) + " before restore", ".zip");
    error = write_backup(safetyPath);
    if(!error.empty()) {
        backupStatus = "Restore cancelled. The safety backup couldn't be made: " + error;
        log_user(backupStatus);
        return;
    }
    refresh_safety_backups();
    while(safetyBackups.size() > SAFETY_BACKUPS_TO_KEEP) {
        std::filesystem::remove(safetyBackups.back(), ec);
        safetyBackups.pop_back();
    }

    nlohmann::json manifestFiles = nlohmann::json::object();
    for(const MiniZip::ReadEntry& e : entries) {
        if(e.name == MANIFEST_NAME) {
            try { manifestFiles = nlohmann::json::parse(e.data).at("files"); } catch(...) {}
        }
    }

    size_t filesWritten = 0;
    size_t filesKept = 0;
    size_t menuItemsAdded = 0;
    bool settingsRestored = false;
    for(const MiniZip::ReadEntry& e : entries) {
        if(!is_safe_entry_name(e.name) || e.name == MANIFEST_NAME)
            continue;
        std::filesystem::path dest = main.conf.configPath / fs_path(e.name);
        if(e.name == MENU_DATA_NAME) {
            try {
                MenuData backupData;
                nlohmann::json::parse(e.data).get_to(backupData);
                if(rollBack) {
                    // Keep this device's trash index; the trash folder isn't part of backups
                    backupData.trash = data().trash;
                    backupData.sessions.active = data().sessions.active;
                    data() = backupData;
                    data().fill_missing_defaults();
                }
                else
                    menuItemsAdded = data().merge_from(backupData);
            }
            catch(...) {
                log_user("The backup's schedule and goals couldn't be read, so they were skipped");
            }
            continue;
        }
        bool isSettings = std::find(SETTINGS_FILES.begin(), SETTINGS_FILES.end(), e.name) != SETTINGS_FILES.end();
        if(!isSettings && !starts_with(e.name, "saves/") && !starts_with(e.name, "themes/"))
            continue;
        std::optional<SDL_Time> backupTime = manifest_time(manifestFiles, e.name);
        if(!isSettings && !rollBack && !should_write_file(dest, backupTime)) {
            filesKept++;
            continue;
        }
        std::filesystem::create_directories(dest.parent_path(), ec);
        if(!SDL_SaveFile(utf8(dest).c_str(), e.data.data(), e.data.size())) {
            log_user("Couldn't restore " + e.name + ": " + SDL_GetError());
            continue;
        }
        if(backupTime.has_value())
            set_modified_time(dest, backupTime.value());
        if(isSettings)
            settingsRestored = true;
        else
            filesWritten++;
    }

    changed();
    app.save_now();
    if(settingsRestored)
        main.load_config();
    load_trash_index();
    refresh_notes();
    if(rollBack) {
        backupStatus = "Rolled back. " + std::to_string(filesWritten) + " files put back.";
        log_user("Rolled back");
        return;
    }
    backupStatus = "Restored. " + std::to_string(filesWritten) + " files added or updated, " + std::to_string(filesKept) + " kept (newer on this device), "
        + std::to_string(menuItemsAdded) + " schedule, goal and session items added.";
    log_user("Backup restored");
}

void MainMenuScreen::backup_view() {
    auto& gui = main.g.gui;
    auto& theme = *gui.io.theme;

    page_header("Backup", nullptr, nullptr);

    gui.clipping_element<ScrollArea>("backup scroll area", ScrollArea::Options{
        .scrollVertical = true,
        .clipVertical = true,
        .scrollbarY = ScrollArea::ScrollbarType::NORMAL,
        .innerContent = [&](const ScrollArea::InnerContentParameters&) {
            CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIT(0)}, .padding = {.right = 8}, .childGap = 14, .layoutDirection = CLAY_TOP_TO_BOTTOM}}) {
                card(gui, [&] {
                    label(gui, "Create a backup", theme.frontColor1, GUI_FONT_BOLD, 1.1f);
                    label(gui, "Saves every canvas and folder, your schedule, goals, sessions, settings, palettes and themes into one .zip file.", theme.frontColor2);
                    button(gui, "create backup", "Create Backup...", {.kind = ButtonStyle::Kind::ACCENT, .iconPath = "data/icons/encxded/backup.svg"}, [&] {
                        show_backup_file_dialog(PendingFileChoice::Kind::SAVE_BACKUP);
                    });
                });
                card(gui, [&] {
                    label(gui, "Restore from a backup", theme.frontColor1, GUI_FONT_BOLD, 1.1f);
                    label(gui, "Merges a backup into what's here. When a canvas exists in both, the newer one is kept. Schedule cards, goals and templates from the backup are added to yours, and settings come from the backup. A safety backup is saved first.", theme.frontColor2);
                    button(gui, "restore backup", "Restore...", {.kind = ButtonStyle::Kind::SOFT}, [&] {
                        show_backup_file_dialog(PendingFileChoice::Kind::RESTORE_BACKUP);
                    });
                });
                if(!backupStatus.empty())
                    label(gui, backupStatus, theme.frontColor2);
                if(!safetyBackups.empty()) {
                    card(gui, [&] {
                        label(gui, "Safety backups", theme.frontColor1, GUI_FONT_BOLD, 1.1f);
                        label(gui, "Saved automatically before each restore, so a restore can be rolled back. The newest " + std::to_string(SAFETY_BACKUPS_TO_KEEP) + " are kept.", theme.frontColor2);
                        for(size_t i = 0; i < safetyBackups.size(); i++) {
                            gui.new_id(static_cast<int64_t>(i), [&] {
                                std::filesystem::path p = safetyBackups[i];
                                CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(48)}, .childGap = 10, .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}, .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
                                    icon(gui, "icon", "data/icons/encxded/backup.svg", 24.0f, theme.frontColor2);
                                    CLAY_AUTO_ID({.layout = {.sizing = {.width = CLAY_SIZING_GROW(0)}}}) {
                                        label(gui, utf8(p.stem()), theme.frontColor1);
                                    }
                                    button(gui, "roll back", "Roll Back", {.kind = ButtonStyle::Kind::SOFT}, [&, p] { confirm_restore(utf8(p), true); });
                                }
                            });
                        }
                    });
                }
            }
        }
    });
}

}
