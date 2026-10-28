// EncxdedCanvas: state shared by every screen (menu data, sessions). Created on first use.
#pragma once
#include "MenuData.hpp"
#include "VideoExport.hpp"
#include <memory>

class MainProgram;

namespace Encxded {

class SessionsGUI;

class App {
    public:
        App(MainProgram& main);
        ~App();
        // Call from each screen's update()
        void update();
        // Call at the end of each screen's layout: the sessions popup and the floating timer pill
        void overlay_layout();
        // Returns true if the back button was used (closed the sessions popup)
        bool input_global_back_button();
        void save_now();

        MainProgram& main;
        MenuDataStore store;
        std::unique_ptr<SessionsGUI> sessions;

        // Video export in the background (one at a time). Returns false if one is already running.
        bool start_video_export(const std::filesystem::path& timelinePath, const VideoExportOptions& options);
        bool is_exporting_video() const { return exportJob != nullptr; }
        std::unique_ptr<VideoExportJob> exportJob;
        void export_pill_layout();
        bool exportPanelVideoTab = false;      // Export panel shows the Video tab instead of Image

        // Where the main menu was left, so returning from a canvas comes back to the same place
        int menuSection = 0;
        std::string notesFolder; // Relative to the saves folder, '/' separated, empty = top level
};

App& app(MainProgram& main);

}
