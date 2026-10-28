// EncxdedCanvas: small filesystem/string helpers. Paths are converted through UTF-8 explicitly so names with
// emoji or accents work on Windows too (std::filesystem::path(std::string) uses the ANSI code page there).
#pragma once
#include <cctype>
#include <filesystem>
#include <string>

namespace Encxded {

inline std::filesystem::path fs_path(const std::string& utf8Str) {
    return std::filesystem::path(std::u8string(utf8Str.begin(), utf8Str.end()));
}

inline std::string utf8(const std::u8string& s) {
    return std::string(s.begin(), s.end());
}

inline std::string utf8(const std::filesystem::path& p) {
    return utf8(p.u8string());
}

// Case-insensitive, with runs of digits compared by value ("Ch 2" < "Ch 10")
inline bool natural_less(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while(i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if(std::isdigit(ca) && std::isdigit(cb)) {
            size_t i2 = i, j2 = j;
            while(i2 < a.size() && a[i2] == '0') i2++;
            while(j2 < b.size() && b[j2] == '0') j2++;
            size_t ie = i2, je = j2;
            while(ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ie++;
            while(je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) je++;
            if(ie - i2 != je - j2)
                return ie - i2 < je - j2;
            int c = a.compare(i2, ie - i2, b, j2, je - j2);
            if(c != 0)
                return c < 0;
            i = ie;
            j = je;
            continue;
        }
        int la = std::tolower(ca), lb = std::tolower(cb);
        if(la != lb)
            return la < lb;
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

// dir/stem+ext if free, otherwise "stem 2", "stem 3", ...
inline std::filesystem::path unique_child_path(const std::filesystem::path& dir, const std::string& stem, const std::string& ext) {
    std::error_code ec;
    std::filesystem::path p = dir / fs_path(stem + ext);
    for(int n = 2; std::filesystem::exists(p, ec); n++)
        p = dir / fs_path(stem + " " + std::to_string(n) + ext);
    return p;
}

inline std::string human_file_size(uintmax_t bytes) {
    if(bytes < 1024)
        return std::to_string(bytes) + " B";
    if(bytes < 1024 * 1024)
        return std::to_string((bytes + 512) / 1024) + " KB";
    if(bytes < 1024ULL * 1024 * 1024)
        return std::to_string((bytes + 512 * 1024) / (1024 * 1024)) + " MB";
    return std::to_string((bytes + 512ULL * 1024 * 1024) / (1024ULL * 1024 * 1024)) + " GB";
}

// Valid as a single file or folder name on every platform the app runs on
inline bool is_valid_entry_name(const std::string& name) {
    if(name.empty() || name == "." || name == "..")
        return false;
    for(char c : name) {
        if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

}
