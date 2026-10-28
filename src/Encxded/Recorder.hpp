// EncxdedCanvas: records a canvas session to a timeline (see Recording.hpp). Owned by the canvas editor while recording.
// Compression and file writing happen on a background thread, so recording costs little while drawing.
#pragma once
#include "Recording.hpp"
#include <Helpers/SCollision.hpp>
#include <Helpers/Networking/NetServer.hpp>
#include <SDL3/SDL_audio.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>

class MainProgram;
class World;

namespace Encxded {

class RecordingWriter;
class WavWriter;

class Recorder {
    public:
        // Starts recording right away. Check error() afterwards: non-empty means it couldn't start.
        Recorder(MainProgram& main, World& world, bool micOn);
        ~Recorder(); // Stops and saves

        const std::string& error() const { return startError; }
        void update(); // Every frame while on the canvas

        void set_paused(bool p);
        bool is_paused() const { return paused; }
        void set_mic(bool on);
        bool mic_on() const { return micOn; }
        bool mic_permission_denied() const { return micDenied; }
        int64_t elapsed_ns() const;

        // Finishes the files. Safe to call more than once.
        void stop();
        const std::filesystem::path& timeline_path() const { return timelinePath; }
        const Recording::RecordingMeta& meta() const { return metaData; }
        const World* recorded_world() const { return &world; }
        // Part of the UI layer (surface pixels) left out of recordings, i.e. the recording controls themselves
        void set_ui_exclusion(const std::optional<SCollision::AABB<float>>& r) { uiExclusion = r; }

    private:
        MainProgram& main;
        World& world;
        std::string startError;
        bool stopped = false;

        std::filesystem::path timelinePath;
        std::filesystem::path audioPath;
        std::filesystem::path metaPath;
        Recording::RecordingMeta metaData;
        void write_meta();

        std::unique_ptr<RecordingWriter> writer;
        void push(Recording::RecordType type, int64_t time, std::string_view payload);

        // Recording clock: steady time minus paused time
        bool paused = false;
        int64_t accumulatedNs = 0;
        std::chrono::steady_clock::time_point segmentStart;

        std::shared_ptr<NetServer::ClientData> netClient;

        std::string lastView;
        std::string lastSettings;
        std::string lastSelection;
        std::string lastOverlay;
        void record_view();
        void record_settings();
        void record_selection();
        void record_overlays();

        std::optional<SCollision::AABB<float>> pendingUIArea;
        std::optional<SCollision::AABB<float>> uiExclusion;
        std::chrono::steady_clock::time_point lastUISnapshot;
        void record_ui(bool force);
        // UI patches are read back from the GPU asynchronously where possible
        struct UIReadback {
            Recorder* recorder;
            Recording::UIPatchHeader header;
            int64_t time;
            std::optional<SCollision::AABB<float>> exclusion;
        };
        bool uiReadbacksInFlight = false;
        void push_ui_patch(const Recording::UIPatchHeader& h, int64_t time, const uint8_t* pixels, size_t rowBytes, const std::optional<SCollision::AABB<float>>& exclusion);
        // Runs the callbacks of finished readbacks; with wait, blocks until all of them have finished
        void finish_ui_readbacks(bool wait);

        // Microphone. The WAV is created the first time the mic is on and padded with silence so it lines up with the timeline.
        bool micOn = false;
        // Android asks for the permission asynchronously; shared so a late answer can't touch a destroyed recorder
        struct MicPermission {
            std::atomic<bool> answered = false;
            std::atomic<bool> granted = false;
        };
        std::shared_ptr<MicPermission> micPermission;
        bool micDenied = false;
        SDL_AudioStream* micStream = nullptr;
        std::unique_ptr<WavWriter> wav;
        void update_mic();
        void open_mic();
        void close_mic();
};

}
