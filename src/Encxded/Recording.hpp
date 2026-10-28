// EncxdedCanvas: canvas recordings (Phase 7). Shared by the recorder and the replay/export side.
//
// A recording is a timeline that can be replayed offscreen and encoded to video later, so live drawing never encodes video.
// Files live in a hidden "<canvas name>.recordings" folder next to the canvas:
//   <name>.encrec  zstd stream of records (see below), after an 8 byte magic
//   <name>.wav     microphone audio, if the mic was on at any point (16-bit mono, aligned to the timeline)
//   <name>.json    RecordingMeta, for listing recordings without reading the timeline
//
// Each record, before compression: u8 type, i64 time (ns on the recording clock, which stops while paused), u32 size, payload.
#pragma once
#include <nlohmann/json.hpp>
#include <SDL3/SDL_time.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Encxded { namespace Recording {

constexpr char MAGIC[8] = {'E', 'N', 'C', 'R', 'E', 'C', '0', '1'};
constexpr const char* FOLDER_SUFFIX = ".recordings";
constexpr const char* TIMELINE_EXTENSION = ".encrec";
constexpr const char* AUDIO_EXTENSION = ".wav";
constexpr const char* META_EXTENSION = ".json";
constexpr int AUDIO_SAMPLE_RATE = 48000;

enum class RecordType : uint8_t {
    // Payload: u8 channel (see NetChannel), then the message exactly as a network client would receive it.
    // Replayed through a client World, these rebuild the canvas: first the initial state, then every change.
    NET = 1,
    // Payload: ViewState (portable binary cereal). Written whenever it changes.
    VIEW = 2,
    // Payload: UIPatch header, then w*h RGBA8 premultiplied pixels. The part of the UI layer that changed.
    UI = 3,
    // Payload: JSON of app-level settings that affect what the canvas looks like (background grid). Written when it changes.
    SETTINGS = 4,
    // No payload. Last record; its time is the recording's length.
    END = 5,
    // Payload: serialized SkPicture of the tool overlays (selection box and handles, lasso, eraser circle...) in window pixels,
    // or empty for none. Written when it changes.
    OVERLAY = 6,
    // Payload: empty for no selection, else (portable binary cereal) the selected components' IDs and the camera they're drawn
    // with, which differs from the view's while a selection is being dragged, scaled or rotated. Written when it changes.
    SELECTION = 7
};

enum class NetChannel : uint8_t {
    RELIABLE = 0,
    UNRELIABLE = 1,
    RESOURCE = 2
};

struct ViewState {
    // Camera (CoordSpaceHelper, serialized separately since it holds arbitrary precision numbers)
    float windowW = 0.0f;
    float windowH = 0.0f;
    float pointerX = 0.0f;
    float pointerY = 0.0f;
    bool penInProximity = false;
    bool penDown = false;
    bool mouseDown = false;
    bool touchDevice = false;
    template <typename Archive> void serialize(Archive& a) {
        a(windowW, windowH, pointerX, pointerY, penInProximity, penDown, mouseDown, touchDevice);
    }
};

struct UIPatchHeader {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    int32_t surfaceW = 0; // Size of the whole UI layer; a change means the window was resized
    int32_t surfaceH = 0;
};

struct RecordingMeta {
    std::string name;
    SDL_Time created = 0;
    int64_t durationNs = 0;
    int width = 0;  // Window size when recording started (sets the video's aspect ratio)
    int height = 0;
    bool hasAudio = false;
    bool finished = false; // False if the app stopped before the recording was saved properly
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(RecordingMeta, name, created, durationNs, width, height, hasAudio, finished)
};

// "<dir>/<stem>.recordings" for "<dir>/<stem>.infpnt"
inline std::filesystem::path folder_for_canvas(const std::filesystem::path& canvasPath) {
    std::filesystem::path p = canvasPath;
    p.replace_extension(FOLDER_SUFFIX);
    return p;
}

inline bool is_recordings_folder(const std::filesystem::path& p) {
    return p.extension() == FOLDER_SUFFIX;
}

}}
