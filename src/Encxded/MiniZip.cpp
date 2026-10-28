#include "MiniZip.hpp"
#include "FsUtil.hpp"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_time.h>
#include <array>
#include <cstdint>
#include <limits>

namespace Encxded { namespace MiniZip {

namespace {
    constexpr uint32_t LOCAL_HEADER_SIG = 0x04034b50;
    constexpr uint32_t CENTRAL_HEADER_SIG = 0x02014b50;
    constexpr uint32_t END_OF_CENTRAL_DIR_SIG = 0x06054b50;
    constexpr uint16_t FLAG_UTF8_NAME = 0x0800;

    uint32_t crc32(const std::string& data) {
        static const std::array<uint32_t, 256> table = [] {
            std::array<uint32_t, 256> t{};
            for(uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for(int k = 0; k < 8; k++)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t crc = 0xFFFFFFFFu;
        for(unsigned char b : data)
            crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    void put16(std::string& s, uint16_t v) {
        s.push_back(static_cast<char>(v & 0xFF));
        s.push_back(static_cast<char>((v >> 8) & 0xFF));
    }

    void put32(std::string& s, uint32_t v) {
        put16(s, static_cast<uint16_t>(v & 0xFFFF));
        put16(s, static_cast<uint16_t>(v >> 16));
    }

    uint16_t get16(const std::string& s, size_t p) {
        return static_cast<uint16_t>(static_cast<unsigned char>(s[p]) | (static_cast<unsigned char>(s[p + 1]) << 8));
    }

    uint32_t get32(const std::string& s, size_t p) {
        return static_cast<uint32_t>(get16(s, p)) | (static_cast<uint32_t>(get16(s, p + 2)) << 16);
    }

    std::pair<uint16_t, uint16_t> dos_time_date() {
        SDL_Time now = 0;
        SDL_DateTime dt{};
        if(!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &dt, true) || dt.year < 1980)
            return {0, (1 << 5) | 1}; // 1980-01-01
        uint16_t time = static_cast<uint16_t>((dt.hour << 11) | (dt.minute << 5) | (dt.second / 2));
        uint16_t date = static_cast<uint16_t>(((dt.year - 1980) << 9) | (dt.month << 5) | dt.day);
        return {time, date};
    }

    bool read_whole_file(const std::filesystem::path& p, std::string& out) {
        size_t size = 0;
        void* data = SDL_LoadFile(utf8(p).c_str(), &size);
        if(!data)
            return false;
        out.assign(static_cast<const char*>(data), size);
        SDL_free(data);
        return true;
    }
}

std::string write_zip(const std::filesystem::path& outPath, const std::vector<FileToAdd>& files) {
    constexpr uint64_t MAX_ZIP32 = std::numeric_limits<uint32_t>::max();
    auto [dosTime, dosDate] = dos_time_date();

    SDL_IOStream* out = SDL_IOFromFile(utf8(outPath).c_str(), "wb");
    if(!out)
        return std::string("Couldn't create the backup file: ") + SDL_GetError();

    std::string centralDir;
    uint64_t offset = 0;
    std::string error;

    for(const FileToAdd& f : files) {
        std::string data;
        if(!read_whole_file(f.sourcePath, data)) {
            error = "Couldn't read " + utf8(f.sourcePath);
            break;
        }
        if(data.size() > MAX_ZIP32 || offset > MAX_ZIP32) {
            error = "The backup is larger than 4 GB, which isn't supported yet";
            break;
        }
        uint32_t crc = crc32(data);
        uint32_t size = static_cast<uint32_t>(data.size());

        std::string header;
        put32(header, LOCAL_HEADER_SIG);
        put16(header, 20);              // Version needed
        put16(header, FLAG_UTF8_NAME);
        put16(header, 0);               // Stored
        put16(header, dosTime);
        put16(header, dosDate);
        put32(header, crc);
        put32(header, size);
        put32(header, size);
        put16(header, static_cast<uint16_t>(f.nameInZip.size()));
        put16(header, 0);               // Extra field length
        header += f.nameInZip;

        put32(centralDir, CENTRAL_HEADER_SIG);
        put16(centralDir, 20);          // Version made by
        put16(centralDir, 20);          // Version needed
        put16(centralDir, FLAG_UTF8_NAME);
        put16(centralDir, 0);
        put16(centralDir, dosTime);
        put16(centralDir, dosDate);
        put32(centralDir, crc);
        put32(centralDir, size);
        put32(centralDir, size);
        put16(centralDir, static_cast<uint16_t>(f.nameInZip.size()));
        put16(centralDir, 0);           // Extra
        put16(centralDir, 0);           // Comment
        put16(centralDir, 0);           // Disk number
        put16(centralDir, 0);           // Internal attributes
        put32(centralDir, 0);           // External attributes
        put32(centralDir, static_cast<uint32_t>(offset));
        centralDir += f.nameInZip;

        if(SDL_WriteIO(out, header.data(), header.size()) != header.size() || SDL_WriteIO(out, data.data(), data.size()) != data.size()) {
            error = std::string("Couldn't write the backup file: ") + SDL_GetError();
            break;
        }
        offset += header.size() + data.size();
    }

    if(error.empty()) {
        if(files.size() > 0xFFFF || offset > MAX_ZIP32)
            error = "Too many files for one backup";
        else {
            std::string end;
            put32(end, END_OF_CENTRAL_DIR_SIG);
            put16(end, 0);
            put16(end, 0);
            put16(end, static_cast<uint16_t>(files.size()));
            put16(end, static_cast<uint16_t>(files.size()));
            put32(end, static_cast<uint32_t>(centralDir.size()));
            put32(end, static_cast<uint32_t>(offset));
            put16(end, 0);
            if(SDL_WriteIO(out, centralDir.data(), centralDir.size()) != centralDir.size() || SDL_WriteIO(out, end.data(), end.size()) != end.size())
                error = std::string("Couldn't write the backup file: ") + SDL_GetError();
        }
    }

    if(!SDL_CloseIO(out) && error.empty())
        error = std::string("Couldn't finish writing the backup file: ") + SDL_GetError();
    if(!error.empty())
        SDL_RemovePath(utf8(outPath).c_str());
    return error;
}

std::string read_zip(const std::filesystem::path& zipPath, std::vector<ReadEntry>& entries) {
    std::string z;
    if(!read_whole_file(zipPath, z))
        return std::string("Couldn't open the backup file: ") + SDL_GetError();
    if(z.size() < 22)
        return "This isn't a zip file";

    // The end-of-central-directory record is at the end, possibly followed by a comment of up to 64 KB
    size_t eocd = std::string::npos;
    size_t searchStart = z.size() - 22;
    size_t searchEnd = z.size() > 22 + 0xFFFF ? z.size() - 22 - 0xFFFF : 0;
    for(size_t p = searchStart + 1; p-- > searchEnd;) {
        if(get32(z, p) == END_OF_CENTRAL_DIR_SIG) {
            eocd = p;
            break;
        }
    }
    if(eocd == std::string::npos)
        return "This isn't a zip file";

    uint16_t count = get16(z, eocd + 10);
    uint32_t cdOffset = get32(z, eocd + 16);
    size_t p = cdOffset;
    entries.clear();
    for(uint16_t i = 0; i < count; i++) {
        if(p + 46 > z.size() || get32(z, p) != CENTRAL_HEADER_SIG)
            return "The backup file is damaged";
        uint16_t method = get16(z, p + 10);
        uint32_t crc = get32(z, p + 16);
        uint32_t compSize = get32(z, p + 20);
        uint32_t uncompSize = get32(z, p + 24);
        uint16_t nameLen = get16(z, p + 28);
        uint16_t extraLen = get16(z, p + 30);
        uint16_t commentLen = get16(z, p + 32);
        uint32_t localOffset = get32(z, p + 42);
        if(p + 46 + nameLen > z.size())
            return "The backup file is damaged";
        std::string name = z.substr(p + 46, nameLen);
        p += 46 + nameLen + extraLen + commentLen;

        if(!name.empty() && name.back() == '/')
            continue; // Directory entry
        if(method != 0)
            return "This zip is compressed. Only backups made by EncxdedCanvas can be restored";
        if(localOffset + 30 > z.size() || get32(z, localOffset) != LOCAL_HEADER_SIG)
            return "The backup file is damaged";
        size_t dataStart = localOffset + 30 + get16(z, localOffset + 26) + get16(z, localOffset + 28);
        if(dataStart + compSize > z.size() || compSize != uncompSize)
            return "The backup file is damaged";
        entries.push_back({name, z.substr(dataStart, compSize)});
        if(crc32(entries.back().data) != crc)
            return "The backup file is damaged (" + name + ")";
    }
    return "";
}

}}
