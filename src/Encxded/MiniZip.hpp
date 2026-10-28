// EncxdedCanvas: minimal ZIP support for backups. Files are stored uncompressed (canvases are already zstd
// compressed), so any zip tool can open the result. Reading only supports stored entries, which covers
// everything this app writes.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Encxded { namespace MiniZip {

struct FileToAdd {
    std::string nameInZip; // '/' separated
    std::filesystem::path sourcePath;
};

struct ReadEntry {
    std::string name;
    std::string data;
};

// Returns an empty string on success, otherwise an error message
std::string write_zip(const std::filesystem::path& outPath, const std::vector<FileToAdd>& files);
std::string read_zip(const std::filesystem::path& zipPath, std::vector<ReadEntry>& entries);

}}
