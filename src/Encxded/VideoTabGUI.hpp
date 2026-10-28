// EncxdedCanvas: "Video" tab of the export panel (Phase 7): this canvas's recordings, export options, Export MP4 / Rename / Delete
#pragma once
#include <filesystem>

class MainProgram;

namespace Encxded {

void video_export_tab(MainProgram& main, const std::filesystem::path& canvasPath);

}
