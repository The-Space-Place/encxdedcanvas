#include "Replayer.hpp"
#include "FsUtil.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/Networking/ByteStream.hpp>
#include <Helpers/Serializers.hpp>
#include <Helpers/StringHelpers.hpp>
#include "../CanvasComponents/CanvasComponentContainer.hpp"
#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/vector.hpp>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <cmath>
#include <cstring>
#include <numbers>
#include <zstd.h>

namespace Encxded {

using namespace Recording;

namespace {
    constexpr size_t RECORD_HEADER_SIZE = 13;
    constexpr double SMOOTH_CAMERA_SECONDS = 0.25; // Time constant of the camera easing
    constexpr float POINTER_RADIUS = 7.0f;

    uint32_t get_u32(const char* p) {
        uint32_t v = 0;
        for(int i = 0; i < 4; i++)
            v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
        return v;
    }
    int64_t get_i64(const char* p) {
        uint64_t v = 0;
        for(int i = 0; i < 8; i++)
            v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
        return static_cast<int64_t>(v);
    }
    int32_t get_i32(const char* p) {
        return static_cast<int32_t>(get_u32(p));
    }

    const char* channel_name(NetChannel c) {
        switch(c) {
            case NetChannel::RELIABLE: return RELIABLE_COMMAND_CHANNEL;
            case NetChannel::UNRELIABLE: return UNRELIABLE_COMMAND_CHANNEL;
            case NetChannel::RESOURCE: return RESOURCE_COMMAND_CHANNEL;
        }
        return RELIABLE_COMMAND_CHANNEL;
    }
}

RecordingReader::~RecordingReader() {
    if(dctx)
        ZSTD_freeDCtx(dctx);
    if(file)
        SDL_CloseIO(file);
}

bool RecordingReader::open(const std::filesystem::path& p, std::string& error) {
    file = SDL_IOFromFile(utf8(p).c_str(), "rb");
    if(!file) {
        error = std::string("Couldn't open the recording: ") + SDL_GetError();
        return false;
    }
    char magic[sizeof(MAGIC)];
    if(SDL_ReadIO(file, magic, sizeof(magic)) != sizeof(magic) || std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0) {
        error = "This isn't an EncxdedCanvas recording";
        return false;
    }
    dctx = ZSTD_createDCtx();
    inBuf.resize(ZSTD_DStreamInSize());
    return true;
}

bool RecordingReader::fill(size_t bytes) {
    std::vector<char> chunk(ZSTD_DStreamOutSize());
    while(out.size() - outPos < bytes) {
        if(inPos == inSize && !eof) {
            inSize = SDL_ReadIO(file, inBuf.data(), inBuf.size());
            inPos = 0;
            if(inSize == 0)
                eof = true;
        }
        ZSTD_inBuffer in{inBuf.data(), inSize, inPos};
        ZSTD_outBuffer o{chunk.data(), chunk.size(), 0};
        size_t r = ZSTD_decompressStream(dctx, &o, &in);
        inPos = in.pos;
        if(ZSTD_isError(r))
            return false;
        if(outPos > (1 << 20)) { // Drop consumed bytes now and then
            out.erase(0, outPos);
            outPos = 0;
        }
        out.append(chunk.data(), o.pos);
        if(o.pos == 0 && inPos == inSize && eof)
            return out.size() - outPos >= bytes;
    }
    return true;
}

bool RecordingReader::next(Record& r) {
    if(!fill(RECORD_HEADER_SIZE))
        return false;
    const char* h = out.data() + outPos;
    uint8_t type = static_cast<uint8_t>(h[0]);
    int64_t time = get_i64(h + 1);
    uint32_t size = get_u32(h + 9);
    if(!fill(RECORD_HEADER_SIZE + size))
        return false;
    r.type = static_cast<RecordType>(type);
    r.time = time;
    r.payload.assign(out.data() + outPos + RECORD_HEADER_SIZE, size);
    outPos += RECORD_HEADER_SIZE + size;
    return true;
}

Replayer::Replayer(MainProgram& m, const std::filesystem::path& timelinePath):
    main(m)
{
    std::filesystem::path metaPath = timelinePath;
    metaPath.replace_extension(META_EXTENSION);
    try {
        nlohmann::json::parse(read_file_to_string(metaPath)).get_to(metaData);
    }
    catch(...) {}

    if(!reader.open(timelinePath, startError))
        return;
    world = World::create_world_ptr_from_event(main, {
        .isClient = true,
        .isReplay = true
    });
    if(!world)
        startError = "Couldn't create the replay canvas";
}

Replayer::~Replayer() {}

void Replayer::advance_to(int64_t t) {
    if(!world)
        return;
    for(;;) {
        if(!pending.has_value()) {
            if(readerDone)
                break;
            RecordingReader::Record r;
            if(!reader.next(r)) {
                readerDone = true;
                break;
            }
            pending = std::move(r);
        }
        if(pending->time > t)
            break;
        apply(pending.value());
        pending = std::nullopt;
    }
    world->update(); // Processes the network messages pushed above
    world->rMan.update();
}

void Replayer::apply(const RecordingReader::Record& r) {
    switch(r.type) {
        case RecordType::NET: {
            if(r.payload.empty())
                break;
            NetChannel c = static_cast<NetChannel>(r.payload[0]);
            world->netClient->push_replay_message(channel_name(c), r.payload.substr(1));
            break;
        }
        case RecordType::VIEW: {
            try {
                ByteMemStream strm(const_cast<char*>(r.payload.data()), r.payload.size());
                cereal::PortableBinaryInputArchive a(strm);
                a(cam, view);
                haveView = true;
            }
            catch(...) {}
            break;
        }
        case RecordType::UI: {
            if(r.payload.size() < 24)
                break;
            const char* p = r.payload.data();
            UIPatchHeader h{get_i32(p), get_i32(p + 4), get_i32(p + 8), get_i32(p + 12), get_i32(p + 16), get_i32(p + 20)};
            if(h.w <= 0 || h.h <= 0 || r.payload.size() < 24 + static_cast<size_t>(h.w) * h.h * 4)
                break;
            if(uiLayer.width() != h.surfaceW || uiLayer.height() != h.surfaceH) {
                uiLayer.allocPixels(SkImageInfo::Make(h.surfaceW, h.surfaceH, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
                uiLayer.eraseColor(SK_ColorTRANSPARENT);
            }
            if(h.x < 0 || h.y < 0 || h.x + h.w > h.surfaceW || h.y + h.h > h.surfaceH)
                break;
            for(int y = 0; y < h.h; y++)
                std::memcpy(uiLayer.getAddr32(h.x, h.y + y), p + 24 + static_cast<size_t>(y) * h.w * 4, static_cast<size_t>(h.w) * 4);
            uiLayer.notifyPixelsChanged();
            uiImage = nullptr;
            break;
        }
        case RecordType::SETTINGS: {
            try {
                nlohmann::json::parse(r.payload).at("backgroundGrid").get_to(grid);
            }
            catch(...) {}
            break;
        }
        case RecordType::OVERLAY:
            overlay = r.payload.empty() ? nullptr : SkPicture::MakeFromData(r.payload.data(), r.payload.size());
            break;
        case RecordType::SELECTION: {
            selectedIds.clear();
            hiddenIds.clear();
            if(r.payload.empty())
                break;
            try {
                ByteMemStream strm(const_cast<char*>(r.payload.data()), r.payload.size());
                cereal::PortableBinaryInputArchive a(strm);
                a(selectedIds, selectionCam);
                hiddenIds.insert(selectedIds.begin(), selectedIds.end());
            }
            catch(...) {
                selectedIds.clear();
            }
            break;
        }
        case RecordType::END:
            break;
    }
}

void Replayer::update_smoothing(double frameSeconds) {
    WorldVec center = cam.from_space(Vector2f{view.windowW * 0.5f, view.windowH * 0.5f});
    if(!smoothInitialized) {
        smoothCenter = center;
        smoothInverseScale = cam.inverseScale;
        smoothRotation = cam.rotation;
        smoothInitialized = true;
        return;
    }
    double a = 1.0 - std::exp(-frameSeconds / SMOOTH_CAMERA_SECONDS);
    double zoomRatio = static_cast<double>(cam.inverseScale / smoothInverseScale);
    if(std::isfinite(zoomRatio) && zoomRatio > 0.0)
        smoothInverseScale = smoothInverseScale * WorldScalar(std::pow(zoomRatio, a));
    else
        smoothInverseScale = cam.inverseScale;
    smoothCenter = smoothCenter + (center - smoothCenter) * WorldScalar(a);
    double rotationDiff = std::remainder(cam.rotation - smoothRotation, 2.0 * std::numbers::pi);
    smoothRotation += rotationDiff * a;
}

void Replayer::render(SkCanvas* canvas, int outW, int outH, bool includeUI, CameraMode cameraMode, double frameSeconds) {
    if(!world || !haveView || view.windowW <= 0.0f || view.windowH <= 0.0f) {
        canvas->clear(SK_ColorBLACK);
        return;
    }
    float k = std::min(outW / view.windowW, outH / view.windowH);

    WorldVec center;
    WorldScalar inverseScale;
    double rotation;
    if(cameraMode == CameraMode::SMOOTHED) {
        update_smoothing(frameSeconds);
        center = smoothCenter;
        inverseScale = smoothInverseScale;
        rotation = smoothRotation;
    }
    else {
        center = cam.from_space(Vector2f{view.windowW * 0.5f, view.windowH * 0.5f});
        inverseScale = cam.inverseScale;
        rotation = cam.rotation;
    }

    DrawData drawData = world->drawData;
    drawData.cam.c.inverseScale = inverseScale / WorldScalar(k);
    drawData.cam.c.set_rotation(rotation);
    drawData.cam.c.pos = center - drawData.cam.c.dir_from_space(Vector2f{outW * 0.5f, outH * 0.5f});
    drawData.cam.set_viewing_area(Vector2f{static_cast<float>(outW), static_cast<float>(outH)});
    drawData.takingScreenshot = true; // Draws the layers directly, without the live canvas's caches or tool overlays
    drawData.transparentBackground = false;
    drawData.drawGrids = true;
    drawData.hiddenComponents = selectedIds.empty() ? nullptr : &hiddenIds; // Drawn below, where the recorded selection put them
    drawData.refresh_draw_optimizing_values();

    // The background grid is an app setting, so use the recorded one, sized for the output resolution
    GlobalConfig::BackgroundGridOptions liveGrid = main.conf.backgroundGrid;
    main.conf.backgroundGrid = grid;
    main.conf.backgroundGrid.spacing *= k;
    main.conf.backgroundGrid.width *= k;
    main.draw_world(canvas, *world, drawData);
    main.conf.backgroundGrid = liveGrid;

    // Recorded window pixels to output pixels. Follows the canvas, so it includes the smoothing (unlike the UI layer below).
    SkMatrix recordedToOutput;
    {
        Vector2f corners[3] = {{0.0f, 0.0f}, {view.windowW, 0.0f}, {0.0f, view.windowH}};
        SkPoint src[3];
        SkPoint dst[3];
        for(int i = 0; i < 3; i++) {
            Vector2f mapped = drawData.cam.c.to_space(cam.from_space(corners[i]));
            src[i] = SkPoint::Make(corners[i].x(), corners[i].y());
            dst[i] = SkPoint::Make(mapped.x(), mapped.y());
        }
        if(!recordedToOutput.setPolyToPoly(src, dst))
            recordedToOutput = SkMatrix::Translate((outW - view.windowW * k) * 0.5f, (outH - view.windowH * k) * 0.5f).preScale(k, k);
    }

    if(!selectedIds.empty()) {
        // Same as DrawingProgramSelection::draw_components, in the recorded window's pixels
        DrawData selectionDrawData = drawData;
        selectionDrawData.hiddenComponents = nullptr;
        selectionDrawData.cam.c = selectionCam;
        selectionDrawData.cam.set_viewing_area(Vector2f{view.windowW, view.windowH});
        selectionDrawData.refresh_draw_optimizing_values();
        selectionDrawData.skiaAA = main.conf.antialiasing == GlobalConfig::AntiAliasing::SKIA;
        canvas->save();
        canvas->concat(recordedToOutput);
        for(const NetworkingObjects::NetObjID& id : selectedIds) {
            if(auto c = world->netObjMan.get_obj_temporary_ref_from_id<CanvasComponentContainer>(id))
                c->draw(canvas, selectionDrawData);
        }
        canvas->restore();
    }

    if(includeUI) {
        if(overlay) {
            canvas->save();
            canvas->concat(recordedToOutput);
            canvas->drawPicture(overlay);
            canvas->restore();
        }
        canvas->save();
        canvas->translate((outW - view.windowW * k) * 0.5f, (outH - view.windowH * k) * 0.5f);
        canvas->scale(k, k);
        if(!uiLayer.drawsNothing()) {
            if(!uiImage)
                uiImage = uiLayer.asImage();
            canvas->drawImage(uiImage, 0, 0, SkSamplingOptions(SkFilterMode::kLinear));
        }
        draw_pointer(canvas);
        canvas->restore();
    }
}

void Replayer::draw_pointer(SkCanvas* canvas) {
    bool down = view.penDown || view.mouseDown;
    if(!(view.penInProximity || down))
        return;
    SkPaint fill;
    fill.setAntiAlias(true);
    fill.setColor4f(down ? SkColor4f{1.0f, 1.0f, 1.0f, 0.9f} : SkColor4f{1.0f, 1.0f, 1.0f, 0.5f});
    SkPaint ring;
    ring.setAntiAlias(true);
    ring.setStyle(SkPaint::kStroke_Style);
    ring.setStrokeWidth(1.5f);
    ring.setColor4f({0.0f, 0.0f, 0.0f, 0.6f});
    float r = POINTER_RADIUS * (down ? 0.8f : 1.0f);
    canvas->drawCircle(view.pointerX, view.pointerY, r, fill);
    canvas->drawCircle(view.pointerX, view.pointerY, r, ring);
}

}
