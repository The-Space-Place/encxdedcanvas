// EncxdedCanvas: plays a recording back offscreen (Phase 7). The canvas is rebuilt by a replay client World fed the recorded
// network messages; the camera, UI layer and settings come from the other records.
#pragma once
#include "Recording.hpp"
#include "../CoordSpaceHelper.hpp"
#include "../GlobalConfig.hpp"
#include <include/core/SkBitmap.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPicture.h>
#include <Helpers/NetworkingObjects/NetObjID.hpp>
#include <SDL3/SDL_iostream.h>
#include <unordered_set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class MainProgram;
class World;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;

namespace Encxded {

class RecordingReader {
    public:
        struct Record {
            Recording::RecordType type;
            int64_t time = 0;
            std::string payload;
        };
        ~RecordingReader();
        bool open(const std::filesystem::path& p, std::string& error);
        // False at the end (or if the file is cut short, e.g. the app was killed while recording)
        bool next(Record& r);
    private:
        bool fill(size_t bytes);
        SDL_IOStream* file = nullptr;
        ZSTD_DCtx* dctx = nullptr;
        std::vector<char> inBuf;
        size_t inPos = 0;
        size_t inSize = 0;
        std::string out; // Decompressed bytes not yet consumed
        size_t outPos = 0;
        bool eof = false;
};

class Replayer {
    public:
        enum class CameraMode { EXACT, SMOOTHED };
        Replayer(MainProgram& main, const std::filesystem::path& timelinePath);
        ~Replayer();
        const std::string& error() const { return startError; }
        const Recording::RecordingMeta& meta() const { return metaData; }

        // Applies everything up to time t (ns). Times must not go backwards.
        void advance_to(int64_t t);
        // Draws the current frame. The recorded view fills the output (extra canvas is shown if the aspect ratio differs).
        void render(SkCanvas* canvas, int outW, int outH, bool includeUI, CameraMode cameraMode, double frameSeconds);

    private:
        MainProgram& main;
        std::string startError;
        Recording::RecordingMeta metaData;
        RecordingReader reader;
        std::optional<RecordingReader::Record> pending;
        bool readerDone = false;
        void apply(const RecordingReader::Record& r);

        std::shared_ptr<World> world;
        CoordSpaceHelper cam;
        Recording::ViewState view;
        bool haveView = false;
        GlobalConfig::BackgroundGridOptions grid;
        SkBitmap uiLayer;
        sk_sp<SkImage> uiImage; // Snapshot of uiLayer, remade only when it changes
        sk_sp<SkPicture> overlay; // Tool overlays, in recorded window pixels

        // Recorded selection: these components are drawn with selectionCam instead of in their layers
        std::vector<NetworkingObjects::NetObjID> selectedIds;
        std::unordered_set<NetworkingObjects::NetObjID> hiddenIds;
        CoordSpaceHelper selectionCam;

        // Smoothed camera: view center, zoom and rotation eased towards the recorded ones
        bool smoothInitialized = false;
        WorldVec smoothCenter;
        WorldScalar smoothInverseScale;
        double smoothRotation = 0.0;
        void update_smoothing(double frameSeconds);

        void draw_pointer(SkCanvas* canvas);
};

}
