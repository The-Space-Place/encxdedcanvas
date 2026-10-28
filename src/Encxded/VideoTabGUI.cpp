#include "VideoTabGUI.hpp"
#include "EncxdedApp.hpp"
#include "FsUtil.hpp"
#include "Recording.hpp"
#include "../MainProgram.hpp"
#include "../GUIStuff/ElementHelpers/ButtonHelpers.hpp"
#include "../GUIStuff/ElementHelpers/CheckBoxHelpers.hpp"
#include "../GUIStuff/ElementHelpers/LayoutHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextBoxHelpers.hpp"
#include "../GUIStuff/ElementHelpers/TextLabelHelpers.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/StringHelpers.hpp>
#include <algorithm>
#include <vector>

namespace Encxded {

using namespace GUIStuff;
using namespace GUIStuff::ElementHelpers;
using namespace Recording;

namespace {
    struct Entry {
        std::filesystem::path timeline;
        RecordingMeta meta;
    };

    // Rename / delete in progress (only one row at a time)
    std::filesystem::path renaming;
    std::string renameText;
    std::filesystem::path confirmingDelete;

    std::vector<Entry> list_recordings(const std::filesystem::path& canvasPath) {
        std::vector<Entry> entries;
        std::error_code ec;
        std::filesystem::path folder = folder_for_canvas(canvasPath);
        for(auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if(it->path().extension() != TIMELINE_EXTENSION)
                continue;
            Entry e;
            e.timeline = it->path();
            std::filesystem::path metaPath = e.timeline;
            metaPath.replace_extension(META_EXTENSION);
            try {
                nlohmann::json::parse(read_file_to_string(metaPath)).get_to(e.meta);
            }
            catch(...) {
                e.meta.name = utf8(e.timeline.stem());
            }
            entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.meta.created > b.meta.created; });
        return entries;
    }

    std::string length_string(int64_t ns) {
        int64_t s = ns / 1000000000LL;
        auto two = [](int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
        if(s >= 3600)
            return std::to_string(s / 3600) + ":" + two((s / 60) % 60) + ":" + two(s % 60);
        return std::to_string(s / 60) + ":" + two(s % 60);
    }

    std::filesystem::path sibling(const std::filesystem::path& timeline, const char* ext) {
        std::filesystem::path p = timeline;
        p.replace_extension(ext);
        return p;
    }

    void rename_recording(const Entry& e, const std::string& newName) {
        if(!is_valid_entry_name(newName)) {
            Logger::get().log(Logger::LogType::USERINFO, "Names can't be empty or contain / \\ : * ? \" < > |");
            return;
        }
        std::filesystem::path newTimeline = e.timeline.parent_path() / fs_path(newName + TIMELINE_EXTENSION);
        std::error_code ec;
        if(newTimeline == e.timeline)
            return;
        if(std::filesystem::exists(newTimeline, ec)) {
            Logger::get().log(Logger::LogType::USERINFO, "A recording named \"" + newName + "\" already exists");
            return;
        }
        std::filesystem::rename(e.timeline, newTimeline, ec);
        if(ec) {
            Logger::get().log(Logger::LogType::USERINFO, "Couldn't rename the recording: " + ec.message());
            return;
        }
        std::filesystem::rename(sibling(e.timeline, AUDIO_EXTENSION), sibling(newTimeline, AUDIO_EXTENSION), ec);
        std::filesystem::remove(sibling(e.timeline, META_EXTENSION), ec);
        RecordingMeta meta = e.meta;
        meta.name = newName;
        std::string s = nlohmann::json(meta).dump();
        SDL_SaveFile(utf8(sibling(newTimeline, META_EXTENSION)).c_str(), s.data(), s.size());
    }

    void delete_recording(const Entry& e) {
        std::error_code ec;
        for(const char* ext : {TIMELINE_EXTENSION, AUDIO_EXTENSION, META_EXTENSION})
            std::filesystem::remove(sibling(e.timeline, ext), ec);
    }
}

void video_export_tab(MainProgram& main, const std::filesystem::path& canvasPath) {
    auto& gui = main.g.gui;
    App& app = Encxded::app(main);
    VideoExportOptions& o = app.store.data.videoExport;

    auto choice = [&]<typename T>(const char* id, const char* label, T* value, const std::vector<std::pair<const char*, T>>& options) {
        gui.new_id(id, [&] {
            text_label_bold(gui, label);
            left_to_right_line_layout(gui, [&] {
                for(auto& [text, v] : options) {
                    text_button(gui, text, text, {
                        .drawType = SelectableButton::DrawType::TRANSPARENT_BORDER,
                        .isSelected = *value == v,
                        .onClick = [&gui, &app, value, v = v] {
                            *value = v;
                            app.store.mark_dirty();
                            gui.set_to_layout();
                        }
                    });
                }
            });
        });
    };

    choice("resolution", "Resolution", &o.shortSide, std::vector<std::pair<const char*, int>>{{"720p", 720}, {"1080p", 1080}, {"1440p", 1440}, {"4K", 2160}});
    choice("fps", "Frame rate", &o.fps, std::vector<std::pair<const char*, int>>{{"30 fps", 30}, {"60 fps", 60}});
    choice("camera", "Camera", &o.smoothCamera, std::vector<std::pair<const char*, bool>>{{"Exact", false}, {"Smoothed", true}});
    bool includeUI = o.includeUI;
    checkbox_boolean_field(gui, "include ui", "Include UI", &o.includeUI);
    if(o.includeUI != includeUI)
        app.store.mark_dirty();

    text_label_bold(gui, "Recordings");
    std::vector<Entry> entries = canvasPath.empty() ? std::vector<Entry>() : list_recordings(canvasPath);
    if(entries.empty())
        text_label(gui, "No recordings yet. Use the record button\nin the top bar to make one.");

    for(size_t i = 0; i < entries.size(); i++) {
        gui.new_id(static_cast<int64_t>(i), [&] {
            const Entry& e = entries[i];
            if(renaming == e.timeline) {
                input_text(gui, "rename", &renameText, {});
                left_to_right_line_layout(gui, [&] {
                    text_button(gui, "save", "Save", {.onClick = [&gui, e] {
                        rename_recording(e, renameText);
                        renaming.clear();
                        gui.set_to_layout();
                    }});
                    text_button(gui, "cancel", "Cancel", {.drawType = SelectableButton::DrawType::TRANSPARENT_BORDER, .onClick = [&gui] {
                        renaming.clear();
                        gui.set_to_layout();
                    }});
                });
                return;
            }
            text_label_bold(gui, e.meta.name);
            std::string details = length_string(e.meta.durationNs);
            if(e.meta.created)
                details += "  -  " + long_date_string(e.meta.created);
            if(e.meta.hasAudio)
                details += "  -  with audio";
            if(!e.meta.finished)
                details += "  -  incomplete";
            text_label(gui, details);
            if(confirmingDelete == e.timeline) {
                left_to_right_line_layout(gui, [&] {
                    text_label(gui, "Delete this recording?");
                    text_button(gui, "yes", "Delete", {.onClick = [&gui, e] {
                        delete_recording(e);
                        confirmingDelete.clear();
                        gui.set_to_layout();
                    }});
                    text_button(gui, "no", "Keep", {.drawType = SelectableButton::DrawType::TRANSPARENT_BORDER, .onClick = [&gui] {
                        confirmingDelete.clear();
                        gui.set_to_layout();
                    }});
                });
                return;
            }
            left_to_right_line_layout(gui, [&] {
                text_button(gui, "export", app.is_exporting_video() ? "Exporting..." : "Export MP4", {.onClick = [&main, &app, e] {
                    if(!app.start_video_export(e.timeline, app.store.data.videoExport))
                        Logger::get().log(Logger::LogType::USERINFO, "Another video is still exporting");
                }});
                text_button(gui, "rename", "Rename", {.drawType = SelectableButton::DrawType::TRANSPARENT_BORDER, .onClick = [&gui, e] {
                    renaming = e.timeline;
                    renameText = e.meta.name;
                    confirmingDelete.clear();
                    gui.set_to_layout();
                }});
                text_button(gui, "delete", "Delete", {.drawType = SelectableButton::DrawType::TRANSPARENT_BORDER, .onClick = [&gui, e] {
                    confirmingDelete = e.timeline;
                    renaming.clear();
                    gui.set_to_layout();
                }});
            });
        });
    }
}

}
