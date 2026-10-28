// EncxdedCanvas: MP4 (H.264 + AAC) encoding with the platform encoder (Phase 7).
// Android: NDK MediaCodec + MediaMuxer. Windows: Media Foundation sink writer. Other platforms aren't supported yet.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Encxded {

class VideoEncoder {
    public:
        struct Options {
            int width = 1920;   // Even
            int height = 1080;  // Even
            int fps = 30;
            int bitrate = 12000000;
            std::filesystem::path outPath;
            std::filesystem::path wavPath; // Optional: 16-bit mono PCM to add as AAC audio
        };

        static bool is_supported();

        VideoEncoder();
        ~VideoEncoder(); // Aborts if not finished (the output file is deleted)

        // Each returns an empty string on success, else an error message
        std::string start(const Options& options);
        // rgba: width*height opaque RGBA8 pixels, rowBytes apart
        std::string add_frame(const uint8_t* rgba, size_t rowBytes, int64_t ptsUs);
        std::string finish();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
};

}
