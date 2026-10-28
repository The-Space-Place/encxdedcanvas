#include "Recorder.hpp"
#include "FsUtil.hpp"
#include "MenuData.hpp"
#include "../MainProgram.hpp"
#include "../World.hpp"
#include <Helpers/Logger.hpp>
#include <Helpers/Serializers.hpp>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_system.h>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/vector.hpp>
#include <include/core/SkData.h>
#include <include/core/SkPicture.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkSurface.h>
#ifdef USE_SKIA_BACKEND_GANESH
    #include <include/gpu/ganesh/GrDirectContext.h>
#endif
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <zstd.h>

namespace Encxded {

using namespace Recording;

namespace {
    constexpr int64_t NS_PER_SECOND = 1000000000LL;
    constexpr auto UI_SNAPSHOT_INTERVAL = std::chrono::milliseconds(33); // UI changes are captured at up to ~30 per second
    constexpr int64_t AUDIO_SYNC_TOLERANCE_SAMPLES = AUDIO_SAMPLE_RATE / 10;
    constexpr int ZSTD_LEVEL = 3;

    void put_u32(std::string& s, uint32_t v) {
        for(int i = 0; i < 4; i++)
            s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    void put_i64(std::string& s, int64_t v) {
        uint64_t u = static_cast<uint64_t>(v);
        for(int i = 0; i < 8; i++)
            s.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
    }
    void put_i32(std::string& s, int32_t v) {
        put_u32(s, static_cast<uint32_t>(v));
    }

    std::optional<NetChannel> channel_from_name(const std::string& name) {
        if(name == RELIABLE_COMMAND_CHANNEL) return NetChannel::RELIABLE;
        if(name == UNRELIABLE_COMMAND_CHANNEL) return NetChannel::UNRELIABLE;
        if(name == RESOURCE_COMMAND_CHANNEL) return NetChannel::RESOURCE;
        return std::nullopt;
    }

    std::string date_time_name() {
        SDL_DateTime dt{};
        if(!SDL_TimeToDateTime(now_time(), &dt, true))
            return "Recording";
        auto two = [](int v) { return (v < 10 ? "0" : "") + std::to_string(v); };
        return std::to_string(dt.year) + "-" + two(dt.month) + "-" + two(dt.day) + " " + two(dt.hour) + "-" + two(dt.minute) + "-" + two(dt.second);
    }
}

// Compresses records into one zstd stream on a background thread. Each batch is flushed, so a crash loses at most the last moments.
class RecordingWriter {
    public:
        RecordingWriter(SDL_IOStream* f): file(f) {
            ctx = ZSTD_createCCtx();
            // In-progress strokes are re-sent whole every frame; one stream (instead of a frame per record) lets those repeats
            // compress against the previous copy, which is well within the default window
            ZSTD_CCtx_setParameter(ctx, ZSTD_c_compressionLevel, ZSTD_LEVEL);
            thread = std::thread([this] { run(); });
        }
        ~RecordingWriter() { finish(); }

        void push(std::string&& record) {
            {
                std::scoped_lock lock(mutex);
                queue.emplace_back(std::move(record));
            }
            cv.notify_one();
        }

        void finish() {
            if(!thread.joinable())
                return;
            {
                std::scoped_lock lock(mutex);
                stopping = true;
            }
            cv.notify_one();
            thread.join();
            ZSTD_freeCCtx(ctx);
            SDL_CloseIO(file);
        }

        bool failed() const { return writeFailed; }

    private:
        void run() {
            std::vector<char> out(ZSTD_CStreamOutSize());
            for(;;) {
                std::deque<std::string> batch;
                bool last;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopping || !queue.empty(); });
                    batch.swap(queue);
                    last = stopping;
                }
                for(size_t i = 0; i < batch.size(); i++) {
                    bool final = last && i + 1 == batch.size();
                    compress(batch[i], final ? ZSTD_e_end : (i + 1 == batch.size() ? ZSTD_e_flush : ZSTD_e_continue), out);
                }
                if(last) {
                    if(batch.empty())
                        compress("", ZSTD_e_end, out);
                    return;
                }
            }
        }

        void compress(const std::string& data, ZSTD_EndDirective mode, std::vector<char>& out) {
            ZSTD_inBuffer in{data.data(), data.size(), 0};
            for(;;) {
                ZSTD_outBuffer o{out.data(), out.size(), 0};
                size_t remaining = ZSTD_compressStream2(ctx, &o, &in, mode);
                if(ZSTD_isError(remaining)) {
                    writeFailed = true;
                    return;
                }
                if(o.pos && SDL_WriteIO(file, out.data(), o.pos) != o.pos)
                    writeFailed = true;
                bool done = mode == ZSTD_e_continue ? in.pos == in.size : remaining == 0;
                if(done)
                    return;
            }
        }

        SDL_IOStream* file;
        ZSTD_CCtx* ctx = nullptr;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::string> queue;
        bool stopping = false;
        std::atomic<bool> writeFailed = false;
};

// 16-bit mono PCM. The header's sizes are filled in on close.
class WavWriter {
    public:
        WavWriter(const std::filesystem::path& p) {
            file = SDL_IOFromFile(utf8(p).c_str(), "wb");
            if(file) {
                std::string header(44, '\0');
                SDL_WriteIO(file, header.data(), header.size());
            }
        }
        ~WavWriter() {
            if(!file)
                return;
            uint32_t dataBytes = static_cast<uint32_t>(std::min<int64_t>(samples * 2, UINT32_MAX - 44));
            std::string h;
            h += "RIFF"; put_u32(h, 36 + dataBytes); h += "WAVE";
            h += "fmt "; put_u32(h, 16);
            h.push_back(1); h.push_back(0);   // PCM
            h.push_back(1); h.push_back(0);   // Mono
            put_u32(h, AUDIO_SAMPLE_RATE);
            put_u32(h, AUDIO_SAMPLE_RATE * 2); // Byte rate
            h.push_back(2); h.push_back(0);   // Block align
            h.push_back(16); h.push_back(0);  // Bits per sample
            h += "data"; put_u32(h, dataBytes);
            SDL_SeekIO(file, 0, SDL_IO_SEEK_SET);
            SDL_WriteIO(file, h.data(), h.size());
            SDL_CloseIO(file);
        }
        bool ok() const { return file != nullptr; }
        void write(const int16_t* data, size_t count) {
            if(file && count)
                SDL_WriteIO(file, data, count * sizeof(int16_t));
            samples += static_cast<int64_t>(count);
        }
        void write_silence(int64_t count) {
            std::vector<int16_t> zeros(static_cast<size_t>(std::min<int64_t>(count, AUDIO_SAMPLE_RATE)), 0);
            while(count > 0) {
                size_t n = static_cast<size_t>(std::min<int64_t>(count, static_cast<int64_t>(zeros.size())));
                write(zeros.data(), n);
                count -= static_cast<int64_t>(n);
            }
        }
        int64_t samples = 0;
    private:
        SDL_IOStream* file = nullptr;
};

Recorder::Recorder(MainProgram& m, World& w, bool startMicOn):
    main(m),
    world(w)
{
    if(world.filePath.empty()) {
        startError = "Save the canvas before recording";
        return;
    }
    std::filesystem::path folder = folder_for_canvas(world.filePath);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::string name = date_time_name();
    std::filesystem::path base = unique_child_path(folder, name, TIMELINE_EXTENSION);
    std::string stem = utf8(base.stem());
    timelinePath = folder / fs_path(stem + TIMELINE_EXTENSION);
    audioPath = folder / fs_path(stem + AUDIO_EXTENSION);
    metaPath = folder / fs_path(stem + META_EXTENSION);

    SDL_IOStream* f = SDL_IOFromFile(utf8(timelinePath).c_str(), "wb");
    if(!f) {
        startError = std::string("Couldn't create the recording: ") + SDL_GetError();
        return;
    }
    SDL_WriteIO(f, MAGIC, sizeof(MAGIC));
    writer = std::make_unique<RecordingWriter>(f);

    metaData.name = stem;
    metaData.created = now_time();
    metaData.width = main.window.size.x();
    metaData.height = main.window.size.y();
    write_meta();

    segmentStart = std::chrono::steady_clock::now();
    lastUISnapshot = segmentStart - UI_SNAPSHOT_INTERVAL;

    netClient = world.start_recording_client([this](const std::string& channel, std::string&& data) {
        std::optional<NetChannel> c = channel_from_name(channel);
        if(!c.has_value())
            return;
        std::string payload;
        payload.reserve(data.size() + 1);
        payload.push_back(static_cast<char>(c.value()));
        payload += data;
        push(RecordType::NET, elapsed_ns(), payload);
    });
    if(!netClient) {
        startError = "This canvas can't be recorded while connected to someone else's canvas";
        writer = nullptr;
        std::filesystem::remove(timelinePath, ec);
        std::filesystem::remove(metaPath, ec);
        return;
    }

    main.g.gui.trackRedrawnArea = true;
    main.g.gui.take_redrawn_area();
    record_settings();
    record_view();
    record_selection();
    record_overlays();
    record_ui(true);
    set_mic(startMicOn);
}

Recorder::~Recorder() {
    stop();
}

int64_t Recorder::elapsed_ns() const {
    if(paused)
        return accumulatedNs;
    return accumulatedNs + std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - segmentStart).count();
}

void Recorder::set_paused(bool p) {
    if(p == paused || stopped)
        return;
    if(p)
        accumulatedNs = elapsed_ns();
    else
        segmentStart = std::chrono::steady_clock::now();
    paused = p;
}

void Recorder::push(RecordType type, int64_t time, std::string_view payload) {
    if(!writer)
        return;
    std::string r;
    r.reserve(13 + payload.size());
    r.push_back(static_cast<char>(type));
    put_i64(r, time);
    put_u32(r, static_cast<uint32_t>(payload.size()));
    r.append(payload);
    writer->push(std::move(r));
}

void Recorder::update() {
    if(stopped)
        return;
    finish_ui_readbacks(false);
    record_settings();
    record_view();
    record_selection();
    record_overlays();
    record_ui(false);
    update_mic();
}

void Recorder::record_selection() {
    const DrawingProgramSelection& selection = world.drawProg.get_selection();
    std::vector<NetworkingObjects::NetObjID> ids = selection.get_selected_ids();
    std::string s;
    if(!ids.empty()) {
        std::stringstream ss(std::ios::binary | std::ios::out);
        {
            cereal::PortableBinaryOutputArchive a(ss);
            a(ids, selection.get_selection_draw_camera(world.drawData.cam.c));
        }
        s = ss.str();
    }
    if(s == lastSelection)
        return;
    push(RecordType::SELECTION, elapsed_ns(), s);
    lastSelection = std::move(s);
}

void Recorder::record_overlays() {
    DrawData drawData = world.drawData;
    drawData.skiaAA = main.conf.antialiasing == GlobalConfig::AntiAliasing::SKIA;
    SkPictureRecorder pictureRecorder;
    world.drawProg.draw_overlays(pictureRecorder.beginRecording(SkRect::MakeWH(main.window.size.x(), main.window.size.y())), drawData);
    sk_sp<SkPicture> picture = pictureRecorder.finishRecordingAsPicture();
    std::string s;
    if(picture && picture->approximateOpCount() > 0) {
        if(sk_sp<SkData> data = picture->serialize())
            s.assign(static_cast<const char*>(data->data()), data->size());
    }
    if(s == lastOverlay)
        return;
    push(RecordType::OVERLAY, elapsed_ns(), s);
    lastOverlay = std::move(s);
}

void Recorder::record_view() {
    ViewState v;
    v.windowW = main.window.size.x();
    v.windowH = main.window.size.y();
    v.pointerX = main.input.mouse.pos.x();
    v.pointerY = main.input.mouse.pos.y();
    v.penInProximity = main.input.pen.inProximity;
    v.penDown = main.input.pen.isDown;
    v.mouseDown = main.input.mouse.leftDown;
    v.touchDevice = main.input.isTouchDevice;
    std::stringstream ss(std::ios::binary | std::ios::out);
    {
        cereal::PortableBinaryOutputArchive a(ss);
        a(world.drawData.cam.c, v);
    }
    std::string s = ss.str();
    if(s == lastView)
        return;
    push(RecordType::VIEW, elapsed_ns(), s);
    lastView = std::move(s);
}

void Recorder::record_settings() {
    nlohmann::json j;
    j["backgroundGrid"] = main.conf.backgroundGrid;
    std::string s = j.dump();
    if(s == lastSettings)
        return;
    push(RecordType::SETTINGS, elapsed_ns(), s);
    lastSettings = std::move(s);
}

void Recorder::record_ui(bool force) {
    auto& gui = main.g.gui;
    if(std::optional<SCollision::AABB<float>> a = gui.take_redrawn_area()) {
        if(pendingUIArea.has_value())
            pendingUIArea = SCollision::AABB<float>(pendingUIArea->min.cwiseMin(a->min), pendingUIArea->max.cwiseMax(a->max));
        else
            pendingUIArea = a;
    }
    sk_sp<SkSurface>& surface = gui.io.surface;
    if(!surface)
        return;
    int sw = surface->width();
    int sh = surface->height();
    if(force)
        pendingUIArea = SCollision::AABB<float>({0.0f, 0.0f}, {static_cast<float>(sw), static_cast<float>(sh)});
    auto now = std::chrono::steady_clock::now();
    if(!pendingUIArea.has_value() || (!force && now - lastUISnapshot < UI_SNAPSHOT_INTERVAL))
        return;

    int x0 = std::clamp(static_cast<int>(std::floor(pendingUIArea->min.x())), 0, sw);
    int y0 = std::clamp(static_cast<int>(std::floor(pendingUIArea->min.y())), 0, sh);
    int x1 = std::clamp(static_cast<int>(std::ceil(pendingUIArea->max.x())), 0, sw);
    int y1 = std::clamp(static_cast<int>(std::ceil(pendingUIArea->max.y())), 0, sh);
    pendingUIArea = std::nullopt;
    lastUISnapshot = now;
    if(x1 <= x0 || y1 <= y0)
        return;

    UIPatchHeader h{x0, y0, x1 - x0, y1 - y0, sw, sh};
    SkImageInfo info = SkImageInfo::Make(h.w, h.h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
#ifdef USE_SKIA_BACKEND_GANESH
    // Asynchronous: the patch is copied on the GPU now and arrives a frame or two later (see finish_ui_readbacks), so opening a
    // menu while recording doesn't stall drawing. The record keeps the time it was requested at.
    if(main.window.ctx) {
        auto* request = new UIReadback{this, h, elapsed_ns(), uiExclusion};
        surface->asyncRescaleAndReadPixels(info, SkIRect::MakeXYWH(h.x, h.y, h.w, h.h), SkSurface::RescaleGamma::kSrc, SkSurface::RescaleMode::kNearest,
            [](SkSurface::ReadPixelsContext c, std::unique_ptr<const SkSurface::AsyncReadResult> result) {
                std::unique_ptr<UIReadback> r(static_cast<UIReadback*>(c));
                if(result && result->count() == 1)
                    r->recorder->push_ui_patch(r->header, r->time, static_cast<const uint8_t*>(result->data(0)), result->rowBytes(0), r->exclusion);
            }, request);
        uiReadbacksInFlight = true;
        return;
    }
#endif
    std::vector<uint8_t> pixels(static_cast<size_t>(h.w) * h.h * 4);
    if(surface->readPixels(info, pixels.data(), static_cast<size_t>(h.w) * 4, h.x, h.y))
        push_ui_patch(h, elapsed_ns(), pixels.data(), static_cast<size_t>(h.w) * 4, uiExclusion);
}

void Recorder::push_ui_patch(const UIPatchHeader& h, int64_t time, const uint8_t* pixels, size_t rowBytes, const std::optional<SCollision::AABB<float>>& exclusion) {
    std::string payload;
    put_i32(payload, h.x); put_i32(payload, h.y); put_i32(payload, h.w); put_i32(payload, h.h); put_i32(payload, h.surfaceW); put_i32(payload, h.surfaceH);
    size_t headerSize = payload.size();
    size_t tightRowBytes = static_cast<size_t>(h.w) * 4;
    payload.resize(headerSize + tightRowBytes * h.h);
    for(int y = 0; y < h.h; y++)
        std::memcpy(payload.data() + headerSize + y * tightRowBytes, pixels + y * rowBytes, tightRowBytes);
    if(exclusion.has_value()) { // Clear the recording controls out of the patch
        int ex0 = std::max(h.x, static_cast<int>(std::floor(exclusion->min.x())));
        int ey0 = std::max(h.y, static_cast<int>(std::floor(exclusion->min.y())));
        int ex1 = std::min(h.x + h.w, static_cast<int>(std::ceil(exclusion->max.x())));
        int ey1 = std::min(h.y + h.h, static_cast<int>(std::ceil(exclusion->max.y())));
        for(int y = ey0; y < ey1; y++) {
            if(ex1 > ex0)
                std::memset(payload.data() + headerSize + (static_cast<size_t>(y - h.y) * h.w + (ex0 - h.x)) * 4, 0, static_cast<size_t>(ex1 - ex0) * 4);
        }
    }
    push(RecordType::UI, time, payload);
}

void Recorder::finish_ui_readbacks(bool wait) {
#ifdef USE_SKIA_BACKEND_GANESH
    if(!uiReadbacksInFlight || !main.window.ctx)
        return;
    if(wait)
        main.window.ctx->flushAndSubmit(GrSyncCpu::kYes);
    main.window.ctx->checkAsyncWorkCompletion();
    if(wait)
        uiReadbacksInFlight = false;
#endif
}

void Recorder::set_mic(bool on) {
    micOn = on;
    if(!on) {
        close_mic();
        return;
    }
#ifdef __ANDROID__
    if(!micPermission) {
        micPermission = std::make_shared<MicPermission>();
        auto* holder = new std::shared_ptr<MicPermission>(micPermission);
        SDL_RequestAndroidPermission("android.permission.RECORD_AUDIO", [](void* userData, const char*, bool granted) {
            std::unique_ptr<std::shared_ptr<MicPermission>> h(static_cast<std::shared_ptr<MicPermission>*>(userData));
            (*h)->granted = granted;
            (*h)->answered = true;
        }, holder);
        return; // Opened in update_mic once answered
    }
    if(!micPermission->answered || !micPermission->granted)
        return;
#endif
    open_mic();
}

void Recorder::open_mic() {
    if(micStream)
        return;
    if(!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Logger::get().log(Logger::LogType::USERINFO, std::string("Couldn't start audio: ") + SDL_GetError());
        micOn = false;
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_S16, 1, AUDIO_SAMPLE_RATE};
    micStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if(!micStream) {
        Logger::get().log(Logger::LogType::USERINFO, std::string("Couldn't open the microphone: ") + SDL_GetError());
        micOn = false;
        return;
    }
    SDL_ResumeAudioStreamDevice(micStream);
    if(!wav) {
        wav = std::make_unique<WavWriter>(audioPath);
        if(!wav->ok()) {
            wav = nullptr;
            close_mic();
            micOn = false;
            return;
        }
        metaData.hasAudio = true;
        write_meta();
    }
}

void Recorder::close_mic() {
    if(micStream) {
        SDL_DestroyAudioStream(micStream);
        micStream = nullptr;
    }
}

void Recorder::update_mic() {
    if(micOn && !micStream && micPermission && micPermission->answered) {
        if(micPermission->granted)
            open_mic();
        else {
            micOn = false;
            micDenied = true;
            Logger::get().log(Logger::LogType::USERINFO, "Microphone permission was denied, so this recording has no audio");
        }
    }
    if(!wav)
        return;
    int64_t expected = elapsed_ns() * AUDIO_SAMPLE_RATE / NS_PER_SECOND;
    if(micStream) {
        int available = SDL_GetAudioStreamAvailable(micStream);
        if(available > 0) {
            std::vector<int16_t> buf(static_cast<size_t>(available) / sizeof(int16_t));
            int got = SDL_GetAudioStreamData(micStream, buf.data(), static_cast<int>(buf.size() * sizeof(int16_t)));
            size_t count = got > 0 ? static_cast<size_t>(got) / sizeof(int16_t) : 0;
            if(!paused) {
                // Keep the audio on the timeline: fill gaps with silence, drop what would run ahead
                if(wav->samples < expected - AUDIO_SYNC_TOLERANCE_SAMPLES - static_cast<int64_t>(count))
                    wav->write_silence(expected - static_cast<int64_t>(count) - wav->samples);
                int64_t room = expected + AUDIO_SYNC_TOLERANCE_SAMPLES - wav->samples;
                wav->write(buf.data(), static_cast<size_t>(std::clamp<int64_t>(room, 0, static_cast<int64_t>(count))));
            }
        }
    }
    else if(wav->samples < expected)
        wav->write_silence(expected - wav->samples); // Mic off: silence
}

void Recorder::write_meta() {
    std::string s = nlohmann::json(metaData).dump();
    SDL_SaveFile(utf8(metaPath).c_str(), s.data(), s.size());
}

void Recorder::stop() {
    if(stopped || !writer)
        return;
    stopped = true;
    world.stop_recording_client(netClient); // Flushes its last messages into the timeline
    netClient = nullptr;
    finish_ui_readbacks(true); // Their callbacks write to the timeline, so they must all run before it's closed
    update_mic();
    close_mic();
    if(wav) {
        int64_t expected = elapsed_ns() * AUDIO_SAMPLE_RATE / NS_PER_SECOND;
        if(wav->samples < expected)
            wav->write_silence(expected - wav->samples);
        wav = nullptr;
    }
    int64_t duration = elapsed_ns();
    push(RecordType::END, duration, "");
    writer->finish();
    bool failed = writer->failed();
    writer = nullptr;
    main.g.gui.trackRedrawnArea = false;

    metaData.durationNs = duration;
    metaData.finished = !failed;
    write_meta();
    if(failed)
        Logger::get().log(Logger::LogType::USERINFO, "The recording couldn't be saved completely (is the storage full?)");
}

}
