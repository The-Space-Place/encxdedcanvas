#include "VideoExport.hpp"
#include "FsUtil.hpp"
#include "../MainProgram.hpp"
#include <Helpers/Logger.hpp>
#include <cmath>
#include <cstring>
#ifdef USE_SKIA_BACKEND_GANESH
    #include <include/gpu/ganesh/GrDirectContext.h>
#endif
#ifdef __ANDROID__
    #include "../AndroidJNICalls.hpp"
#elif defined(_WIN32)
    #include <windows.h>
    #include <shlobj.h>
#endif

namespace Encxded {

namespace {
    constexpr int64_t NS_PER_SECOND = 1000000000LL;

    // Bits per second for H.264 at this size; generous, since canvas video has sharp edges
    int bitrate_for(int w, int h, int fps) {
        double bitsPerPixel = 0.12;
        return static_cast<int>(std::min(80e6, w * static_cast<double>(h) * fps * bitsPerPixel));
    }
}

VideoExportJob::VideoExportJob(MainProgram& m, const std::filesystem::path& timelinePath, const VideoExportOptions& o):
    main(m),
    options(o)
{
    if(!VideoEncoder::is_supported()) {
        fail("Video export isn't available on this platform yet");
        return;
    }
    replayer = std::make_unique<Replayer>(main, timelinePath);
    if(!replayer->error().empty()) {
        fail(replayer->error());
        return;
    }
    const Recording::RecordingMeta& meta = replayer->meta();
    displayName = meta.name.empty() ? utf8(timelinePath.stem()) : meta.name;

    // Short side from the preset, long side from the recording's aspect ratio (multiple of 16, which encoders like)
    double aspect = meta.width > 0 && meta.height > 0 ? static_cast<double>(meta.width) / meta.height : 16.0 / 9.0;
    bool landscape = aspect >= 1.0;
    int shortSide = options.shortSide & ~1;
    int longSide = static_cast<int>(std::lround(shortSide * (landscape ? aspect : 1.0 / aspect) / 16.0)) * 16;
    width = landscape ? longSide : shortSide;
    height = landscape ? shortSide : longSide;

    int64_t duration = meta.durationNs;
    if(duration <= 0) { // Not saved properly (e.g. the app was closed while recording): its length is the last record's time
        RecordingReader scan;
        std::string scanError;
        RecordingReader::Record r;
        if(scan.open(timelinePath, scanError)) {
            while(scan.next(r))
                duration = r.time;
        }
    }
    frameCount = std::max<int64_t>(1, duration * options.fps / NS_PER_SECOND);

    std::filesystem::path exportDir = main.conf.configPath / "exports";
    std::error_code ec;
    std::filesystem::create_directories(exportDir, ec);
    tempPath = unique_child_path(exportDir, displayName, ".mp4");

    std::filesystem::path wavPath = timelinePath;
    wavPath.replace_extension(Recording::AUDIO_EXTENSION);
    std::string e = encoder.start({
        .width = width,
        .height = height,
        .fps = options.fps,
        .bitrate = bitrate_for(width, height, options.fps),
        .outPath = tempPath,
        .wavPath = meta.hasAudio ? wavPath : std::filesystem::path()
    });
    if(!e.empty()) {
        fail(e);
        return;
    }

    surface = main.create_native_surface({width, height}, false);
    if(!surface) {
        fail("Couldn't create a render target of " + std::to_string(width) + "x" + std::to_string(height));
        return;
    }
    pixels.resize(static_cast<size_t>(width) * height * 4);
}

void VideoExportJob::fail(const std::string& e) {
    errorMessage = e;
    done = true;
    Logger::get().log(Logger::LogType::USERINFO, "Video export failed: " + e);
}

void VideoExportJob::cancel() {
    if(done)
        return;
    done = true;
    errorMessage = "Cancelled";
    Logger::get().log(Logger::LogType::USERINFO, "Video export cancelled");
}

float VideoExportJob::progress() const {
    return frameCount > 0 ? std::clamp(static_cast<float>(encodedFrames) / frameCount, 0.0f, 1.0f) : 0.0f;
}

VideoExportJob::~VideoExportJob() {
    wait_for_readbacks(); // Their callbacks point at this job
}

void VideoExportJob::wait_for_readbacks() {
#ifdef USE_SKIA_BACKEND_GANESH
    if(readbacksInFlight > 0 && main.window.ctx) {
        main.window.ctx->flushAndSubmit(GrSyncCpu::kYes);
        main.window.ctx->checkAsyncWorkCompletion();
    }
#endif
}

int64_t VideoExportJob::frame_time_ns(int64_t f) const {
    return f * NS_PER_SECOND / options.fps;
}

bool VideoExportJob::encode_ready_frames() {
    while(!readyFrames.empty() && readyFrames.begin()->first == encodedFrames) {
        std::string e = encoder.add_frame(readyFrames.begin()->second.data(), static_cast<size_t>(width) * 4, frame_time_ns(encodedFrames) / 1000);
        readyFrames.erase(readyFrames.begin());
        if(!e.empty()) {
            fail(e);
            return false;
        }
        encodedFrames++;
    }
    return true;
}

bool VideoExportJob::step(std::chrono::milliseconds budget) {
    if(done)
        return false;
    auto start = std::chrono::steady_clock::now();
    double frameSeconds = 1.0 / options.fps;
    SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
#ifdef USE_SKIA_BACKEND_GANESH
    GrDirectContext* ctx = main.window.ctx.get();
#endif
    do {
#ifdef USE_SKIA_BACKEND_GANESH
        if(ctx) {
            ctx->checkAsyncWorkCompletion();
            if(readbackFailed) {
                fail("Couldn't read back a rendered frame");
                return false;
            }
            if(!encode_ready_frames())
                return false;
        }
#endif
        if(encodedFrames >= frameCount) {
            complete();
            return false;
        }
        if(frame >= frameCount && readbacksInFlight == 0) { // Everything was read back, yet a frame is missing
            fail("Couldn't read back a rendered frame");
            return false;
        }
        if(frame >= frameCount || readbacksInFlight >= MAX_READBACKS_IN_FLIGHT) {
            // Rendering is ahead of the readbacks: wait for the GPU
#ifdef USE_SKIA_BACKEND_GANESH
            if(ctx)
                ctx->flushAndSubmit(GrSyncCpu::kYes);
#endif
            continue;
        }

        replayer->advance_to(frame_time_ns(frame));
        SkCanvas* canvas = surface->getCanvas();
        canvas->save();
        replayer->render(canvas, width, height, options.includeUI, options.smoothCamera ? Replayer::CameraMode::SMOOTHED : Replayer::CameraMode::EXACT, frameSeconds);
        canvas->restore();
#ifdef USE_SKIA_BACKEND_GANESH
        if(ctx) {
            // The frame is copied out on the GPU, so the next one can be rendered while this one is still in flight
            auto* request = new Readback{this, frame};
            readbacksInFlight++;
            surface->asyncRescaleAndReadPixels(info, SkIRect::MakeWH(width, height), SkSurface::RescaleGamma::kSrc, SkSurface::RescaleMode::kNearest,
                [](SkSurface::ReadPixelsContext c, std::unique_ptr<const SkSurface::AsyncReadResult> result) {
                    std::unique_ptr<Readback> r(static_cast<Readback*>(c));
                    VideoExportJob& job = *r->job;
                    job.readbacksInFlight--;
                    if(!result || result->count() != 1) {
                        job.readbackFailed = true;
                        return;
                    }
                    size_t tightRowBytes = static_cast<size_t>(job.width) * 4;
                    std::vector<uint8_t>& px = job.readyFrames[r->frame];
                    px.resize(tightRowBytes * job.height);
                    const uint8_t* src = static_cast<const uint8_t*>(result->data(0));
                    for(int y = 0; y < job.height; y++)
                        std::memcpy(px.data() + y * tightRowBytes, src + y * result->rowBytes(0), tightRowBytes);
                }, request);
            ctx->submit();
            frame++;
            continue;
        }
#endif
        if(!surface->readPixels(info, pixels.data(), static_cast<size_t>(width) * 4, 0, 0)) {
            fail("Couldn't read back a rendered frame");
            return false;
        }
        std::string e = encoder.add_frame(pixels.data(), static_cast<size_t>(width) * 4, frame_time_ns(frame) / 1000);
        if(!e.empty()) {
            fail(e);
            return false;
        }
        frame++;
        encodedFrames = frame;
    } while(std::chrono::steady_clock::now() - start < budget);
    return true;
}

void VideoExportJob::complete() {
    std::string e = encoder.finish();
    if(!e.empty()) {
        fail(e);
        return;
    }
    done = true;
#ifdef __ANDROID__
    std::error_code ec;
    if(AndroidJNICalls::saveVideoToGallery(utf8(tempPath), displayName + ".mp4")) {
        std::filesystem::remove(tempPath, ec);
        Logger::get().log(Logger::LogType::USERINFO, "Video saved to the Gallery (Movies/EncxdedCanvas)");
    }
    else {
        // Older Android: offer it through the share sheet instead
        std::filesystem::path rel = std::filesystem::relative(tempPath, main.conf.configPath, ec);
        AndroidJNICalls::shareInternalFiles({utf8(rel.generic_u8string())}, "video/mp4", true);
    }
#elif defined(_WIN32)
    std::filesystem::path saved = tempPath;
    PWSTR videos = nullptr;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos))) {
        std::filesystem::path dir = std::filesystem::path(videos) / "EncxdedCanvas";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path dest = unique_child_path(dir, displayName, ".mp4");
        std::filesystem::rename(tempPath, dest, ec);
        if(ec) { // Different drive: copy instead
            ec.clear();
            if(std::filesystem::copy_file(tempPath, dest, ec))
                std::filesystem::remove(tempPath, ec);
        }
        if(!ec)
            saved = dest;
    }
    CoTaskMemFree(videos);
    Logger::get().log(Logger::LogType::USERINFO, "Video saved to " + utf8(saved));
#else
    Logger::get().log(Logger::LogType::USERINFO, "Video saved to " + utf8(tempPath));
#endif
}

}
