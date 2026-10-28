// EncxdedCanvas: turns a recording into an MP4 (Phase 7). Runs a few frames at a time from App::update, so the app stays usable;
// it renders slowly while a canvas is open and at full speed on the main menu.
#pragma once
#include "MenuData.hpp"
#include "Replayer.hpp"
#include "VideoEncoder.hpp"
#include <chrono>
#include <include/core/SkSurface.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

class MainProgram;

namespace Encxded {

class VideoExportJob {
    public:
        VideoExportJob(MainProgram& main, const std::filesystem::path& timelinePath, const VideoExportOptions& options);
        ~VideoExportJob();
        // Renders and encodes frames until the budget is used up (at least one frame). Returns false once finished or failed.
        bool step(std::chrono::milliseconds budget);
        void cancel();

        float progress() const;
        bool is_done() const { return done; }
        const std::string& error() const { return errorMessage; }
        const std::string& name() const { return displayName; }

    private:
        MainProgram& main;
        VideoExportOptions options;
        std::string displayName;
        std::filesystem::path tempPath;
        std::unique_ptr<Replayer> replayer;
        VideoEncoder encoder;
        sk_sp<SkSurface> surface;
        std::vector<uint8_t> pixels; // Synchronous readback (when async isn't available)
        int width = 0;
        int height = 0;
        int64_t frame = 0;         // Next frame to render
        int64_t encodedFrames = 0; // Frames handed to the encoder so far
        int64_t frameCount = 0;
        int64_t frame_time_ns(int64_t f) const;

        // Asynchronous readback: rendered frames are copied out on the GPU and encoded in order as they arrive
        static constexpr int MAX_READBACKS_IN_FLIGHT = 3;
        struct Readback {
            VideoExportJob* job;
            int64_t frame;
        };
        int readbacksInFlight = 0;
        bool readbackFailed = false;
        std::map<int64_t, std::vector<uint8_t>> readyFrames;
        bool encode_ready_frames();
        void wait_for_readbacks();

        bool done = false;
        std::string errorMessage;
        void fail(const std::string& e);
        void complete();
};

}
