#include "VideoEncoder.hpp"
#include "FsUtil.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <SDL3/SDL_iostream.h>

#ifdef __ANDROID__
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#endif

namespace Encxded {

namespace {
    constexpr int AUDIO_SAMPLE_RATE = 48000;
    constexpr int AUDIO_BITRATE = 128000;

    // BT.709, limited range
    inline uint8_t y_of(int r, int g, int b) { return static_cast<uint8_t>(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16); }
    inline uint8_t u_of(int r, int g, int b) { return static_cast<uint8_t>(std::clamp(((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128, 0, 255)); }
    inline uint8_t v_of(int r, int g, int b) { return static_cast<uint8_t>(std::clamp(((112 * r - 102 * g - 10 * b + 128) >> 8) + 128, 0, 255)); }

    // RGBA to 4:2:0 YUV: NV12 (interleaved UV) or I420 (separate U and V planes). Planes are stride bytes per row, and the chroma
    // starts after sliceHeight rows of luma.
    [[maybe_unused]] void rgba_to_yuv420(const uint8_t* rgba, size_t rowBytes, int w, int h, uint8_t* dst, size_t stride, size_t sliceHeight, bool nv12) {
        for(int y = 0; y < h; y++) {
            const uint8_t* src = rgba + static_cast<size_t>(y) * rowBytes;
            uint8_t* yRow = dst + static_cast<size_t>(y) * stride;
            for(int x = 0; x < w; x++)
                yRow[x] = y_of(src[x * 4], src[x * 4 + 1], src[x * 4 + 2]);
        }
        uint8_t* chroma = dst + stride * sliceHeight;
        size_t chromaStride = nv12 ? stride : stride / 2;
        uint8_t* vPlane = chroma + (stride / 2) * (sliceHeight / 2);
        for(int y = 0; y < h / 2; y++) {
            const uint8_t* r0 = rgba + static_cast<size_t>(y * 2) * rowBytes;
            const uint8_t* r1 = r0 + rowBytes;
            for(int x = 0; x < w / 2; x++) {
                int i = x * 8;
                int r = (r0[i] + r0[i + 4] + r1[i] + r1[i + 4] + 2) >> 2;
                int g = (r0[i + 1] + r0[i + 5] + r1[i + 1] + r1[i + 5] + 2) >> 2;
                int b = (r0[i + 2] + r0[i + 6] + r1[i + 2] + r1[i + 6] + 2) >> 2;
                if(nv12) {
                    chroma[y * chromaStride + x * 2] = u_of(r, g, b);
                    chroma[y * chromaStride + x * 2 + 1] = v_of(r, g, b);
                }
                else {
                    chroma[y * chromaStride + x] = u_of(r, g, b);
                    vPlane[y * chromaStride + x] = v_of(r, g, b);
                }
            }
        }
    }

    // The 16-bit mono PCM samples of a WAV written by the recorder (fixed 44 byte header). Empty if it's missing or not a WAV.
    [[maybe_unused]] std::vector<uint8_t> load_wav_pcm(const std::filesystem::path& p) {
        size_t size = 0;
        void* wavData = SDL_LoadFile(utf8(p).c_str(), &size);
        if(!wavData)
            return {};
        std::vector<uint8_t> pcm;
        if(size >= 44 && std::memcmp(wavData, "RIFF", 4) == 0)
            pcm.assign(static_cast<uint8_t*>(wavData) + 44, static_cast<uint8_t*>(wavData) + 44 + ((size - 44) & ~static_cast<size_t>(1)));
        SDL_free(wavData);
        return pcm;
    }
}

#ifdef __ANDROID__

namespace {
    constexpr int COLOR_FORMAT_NV12 = 21; // COLOR_FormatYUV420SemiPlanar
    constexpr int COLOR_FORMAT_I420 = 19; // COLOR_FormatYUV420Planar
    constexpr uint32_t FLAG_CODEC_CONFIG = 2;
    constexpr uint32_t FLAG_END_OF_STREAM = 4;
    constexpr int64_t DEQUEUE_TIMEOUT_US = 10000;

    typedef AMediaFormat* (*GetInputFormatFunc)(AMediaCodec*);
}

struct VideoEncoder::Impl {
    Options o;
    int fd = -1;
    AMediaMuxer* muxer = nullptr;
    AMediaCodec* video = nullptr;
    int colorFormat = COLOR_FORMAT_NV12;
    size_t stride = 0;
    size_t sliceHeight = 0;
    ssize_t videoTrack = -1;
    ssize_t audioTrack = -1;
    bool muxerStarted = false;
    bool finished = false;

    struct Sample {
        std::vector<uint8_t> data;
        int64_t pts;
    };
    std::vector<Sample> audioSamples;
    size_t audioSamplesWritten = 0;
    AMediaFormat* audioFormat = nullptr;

    ~Impl() {
        if(video) {
            AMediaCodec_stop(video);
            AMediaCodec_delete(video);
        }
        if(audioFormat)
            AMediaFormat_delete(audioFormat);
        if(muxer) {
            if(muxerStarted)
                AMediaMuxer_stop(muxer);
            AMediaMuxer_delete(muxer);
        }
        if(fd >= 0)
            close(fd);
        if(!finished && !o.outPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(o.outPath, ec);
        }
    }

    // The whole soundtrack is encoded up front (it's small), so both tracks exist before the muxer starts
    std::string encode_audio() {
        std::vector<uint8_t> pcmData = load_wav_pcm(o.wavPath);
        if(pcmData.empty())
            return ""; // No audio: video only
        const uint8_t* pcm = pcmData.data();
        size_t pcmBytes = pcmData.size();

        AMediaCodec* a = AMediaCodec_createEncoderByType("audio/mp4a-latm");
        if(!a)
            return "This device has no AAC encoder";
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, AUDIO_SAMPLE_RATE);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 1);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, AUDIO_BITRATE);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_AAC_PROFILE, 2); // AAC LC
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 16384);
        media_status_t s = AMediaCodec_configure(a, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(f);
        if(s != AMEDIA_OK || AMediaCodec_start(a) != AMEDIA_OK) {
            AMediaCodec_delete(a);
            return "Couldn't start the audio encoder";
        }

        size_t inPos = 0;
        bool inputDone = false;
        bool outputDone = false;
        while(!outputDone) {
            if(!inputDone) {
                ssize_t i = AMediaCodec_dequeueInputBuffer(a, DEQUEUE_TIMEOUT_US);
                if(i >= 0) {
                    size_t cap = 0;
                    uint8_t* buf = AMediaCodec_getInputBuffer(a, i, &cap);
                    size_t n = std::min(cap & ~static_cast<size_t>(1), pcmBytes - inPos);
                    int64_t pts = static_cast<int64_t>(inPos / 2) * 1000000 / AUDIO_SAMPLE_RATE;
                    if(n == 0) {
                        AMediaCodec_queueInputBuffer(a, i, 0, 0, pts, FLAG_END_OF_STREAM);
                        inputDone = true;
                    }
                    else {
                        std::memcpy(buf, pcm + inPos, n);
                        AMediaCodec_queueInputBuffer(a, i, 0, n, pts, 0);
                        inPos += n;
                    }
                }
            }
            AMediaCodecBufferInfo info;
            ssize_t out = AMediaCodec_dequeueOutputBuffer(a, &info, DEQUEUE_TIMEOUT_US);
            if(out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                if(audioFormat)
                    AMediaFormat_delete(audioFormat);
                audioFormat = AMediaCodec_getOutputFormat(a);
            }
            else if(out >= 0) {
                size_t cap = 0;
                uint8_t* buf = AMediaCodec_getOutputBuffer(a, out, &cap);
                if(!(info.flags & FLAG_CODEC_CONFIG) && info.size > 0)
                    audioSamples.push_back({std::vector<uint8_t>(buf + info.offset, buf + info.offset + info.size), info.presentationTimeUs});
                if(info.flags & FLAG_END_OF_STREAM)
                    outputDone = true;
                AMediaCodec_releaseOutputBuffer(a, out, false);
            }
        }
        AMediaCodec_stop(a);
        AMediaCodec_delete(a);
        return "";
    }

    void write_audio_until(int64_t pts) {
        if(audioTrack < 0)
            return;
        while(audioSamplesWritten < audioSamples.size() && audioSamples[audioSamplesWritten].pts <= pts) {
            Sample& s = audioSamples[audioSamplesWritten];
            AMediaCodecBufferInfo info{0, static_cast<int32_t>(s.data.size()), s.pts, 0};
            AMediaMuxer_writeSampleData(muxer, audioTrack, s.data.data(), &info);
            audioSamplesWritten++;
        }
    }

    std::string drain_video(bool untilEnd) {
        int waits = 0;
        for(;;) {
            AMediaCodecBufferInfo info;
            ssize_t out = AMediaCodec_dequeueOutputBuffer(video, &info, untilEnd ? DEQUEUE_TIMEOUT_US : 0);
            if(out == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                if(!untilEnd)
                    return "";
                if(++waits > 500) // 5 seconds without output
                    return "The video encoder stopped responding";
                continue;
            }
            if(out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* f = AMediaCodec_getOutputFormat(video);
                videoTrack = AMediaMuxer_addTrack(muxer, f);
                AMediaFormat_delete(f);
                if(audioFormat)
                    audioTrack = AMediaMuxer_addTrack(muxer, audioFormat);
                if(videoTrack < 0 || AMediaMuxer_start(muxer) != AMEDIA_OK)
                    return "Couldn't start writing the MP4";
                muxerStarted = true;
                continue;
            }
            if(out < 0)
                continue; // Output buffers changed (old API behaviour)
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(video, out, &cap);
            if(!(info.flags & FLAG_CODEC_CONFIG) && info.size > 0 && muxerStarted) {
                write_audio_until(info.presentationTimeUs);
                AMediaMuxer_writeSampleData(muxer, videoTrack, buf, &info);
            }
            bool end = info.flags & FLAG_END_OF_STREAM;
            AMediaCodec_releaseOutputBuffer(video, out, false);
            if(end)
                return "";
        }
    }

    void convert(const uint8_t* rgba, size_t rowBytes, uint8_t* dst) {
        rgba_to_yuv420(rgba, rowBytes, o.width, o.height, dst, stride, sliceHeight, colorFormat == COLOR_FORMAT_NV12);
    }
};

bool VideoEncoder::is_supported() {
    return true;
}

VideoEncoder::VideoEncoder() {}
VideoEncoder::~VideoEncoder() {}

std::string VideoEncoder::start(const Options& options) {
    impl = std::make_unique<Impl>();
    Impl& d = *impl;
    d.o = options;

    if(!d.o.wavPath.empty()) {
        std::string e = d.encode_audio();
        if(!e.empty())
            return e;
    }

    d.fd = open(utf8(d.o.outPath).c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if(d.fd < 0)
        return "Couldn't create the video file";
    d.muxer = AMediaMuxer_new(d.fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    if(!d.muxer)
        return "Couldn't create the MP4 writer";

    d.video = AMediaCodec_createEncoderByType("video/avc");
    if(!d.video)
        return "This device has no H.264 encoder";
    bool configured = false;
    for(int colorFormat : {COLOR_FORMAT_NV12, COLOR_FORMAT_I420}) {
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, d.o.width);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, d.o.height);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, d.o.bitrate);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, d.o.fps);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, colorFormat);
        AMediaFormat_setInt32(f, "color-standard", 1); // BT.709
        AMediaFormat_setInt32(f, "color-range", 2);    // Limited
        AMediaFormat_setInt32(f, "color-transfer", 3); // SDR video
        media_status_t s = AMediaCodec_configure(d.video, f, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(f);
        if(s == AMEDIA_OK) {
            d.colorFormat = colorFormat;
            configured = true;
            break;
        }
    }
    if(!configured)
        return "This device can't encode video at " + std::to_string(d.o.width) + "x" + std::to_string(d.o.height) + ", " + std::to_string(d.o.fps) + " fps. Try a lower resolution or frame rate.";
    if(AMediaCodec_start(d.video) != AMEDIA_OK)
        return "Couldn't start the video encoder";

    // Buffer layout. AMediaCodec_getInputFormat is API 28, newer than this build's minimum, so it's looked up at runtime.
    d.stride = d.o.width;
    d.sliceHeight = d.o.height;
    if(auto getInputFormat = reinterpret_cast<GetInputFormatFunc>(dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat"))) {
        if(AMediaFormat* f = getInputFormat(d.video)) {
            int32_t v = 0;
            if(AMediaFormat_getInt32(f, "stride", &v) && v >= d.o.width)
                d.stride = v;
            if(AMediaFormat_getInt32(f, "slice-height", &v) && v >= d.o.height)
                d.sliceHeight = v;
            AMediaFormat_delete(f);
        }
    }
    return "";
}

std::string VideoEncoder::add_frame(const uint8_t* rgba, size_t rowBytes, int64_t ptsUs) {
    Impl& d = *impl;
    size_t frameBytes = d.stride * d.sliceHeight * 3 / 2;
    for(;;) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(d.video, DEQUEUE_TIMEOUT_US);
        if(i >= 0) {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(d.video, i, &cap);
            if(!buf || cap < frameBytes)
                return "The video encoder's buffers are too small for this resolution";
            d.convert(rgba, rowBytes, buf);
            AMediaCodec_queueInputBuffer(d.video, i, 0, frameBytes, ptsUs, 0);
            break;
        }
        // No free input buffer: the encoder is waiting for its output to be taken
        std::string e = d.drain_video(false);
        if(!e.empty())
            return e;
    }
    return d.drain_video(false);
}

std::string VideoEncoder::finish() {
    Impl& d = *impl;
    for(;;) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(d.video, DEQUEUE_TIMEOUT_US);
        if(i >= 0) {
            AMediaCodec_queueInputBuffer(d.video, i, 0, 0, 0, FLAG_END_OF_STREAM);
            break;
        }
        std::string e = d.drain_video(false);
        if(!e.empty())
            return e;
    }
    std::string e = d.drain_video(true);
    if(!e.empty())
        return e;
    if(!d.muxerStarted)
        return "The video encoder didn't produce any frames";
    d.write_audio_until(INT64_MAX);
    AMediaMuxer_stop(d.muxer);
    d.muxerStarted = false;
    AMediaMuxer_delete(d.muxer);
    d.muxer = nullptr;
    close(d.fd);
    d.fd = -1;
    d.finished = true;
    return "";
}

#elif defined(_WIN32)

namespace {
    constexpr size_t AUDIO_CHUNK_SAMPLES = 4800; // 0.1 s per audio sample handed to the sink writer

    template <typename T> struct ComPtr {
        T* p = nullptr;
        ComPtr() = default;
        ComPtr(const ComPtr&) = delete;
        ComPtr& operator=(const ComPtr&) = delete;
        ~ComPtr() { reset(); }
        void reset() {
            if(p)
                p->Release();
            p = nullptr;
        }
        T** out() { reset(); return &p; }
        T* operator->() const { return p; }
        explicit operator bool() const { return p != nullptr; }
    };

    std::string hresult_string(HRESULT hr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        return buf;
    }
}

struct VideoEncoder::Impl {
    Options o;
    bool comInitialized = false;
    bool mfStarted = false;
    ComPtr<IMFSinkWriter> writer;
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    bool hasAudio = false;
    bool finished = false;

    std::vector<uint8_t> pcm;
    size_t pcmWritten = 0; // Bytes

    ~Impl() {
        writer.reset(); // Without Finalize, the file is left incomplete
        if(mfStarted)
            MFShutdown();
        if(comInitialized)
            CoUninitialize();
        if(!finished && !o.outPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(o.outPath, ec);
        }
    }

    std::string add_video_stream() {
        ComPtr<IMFMediaType> out;
        HRESULT hr = MFCreateMediaType(out.out());
        if(SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if(SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(o.bitrate));
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if(SUCCEEDED(hr)) hr = MFSetAttributeSize(out.p, MF_MT_FRAME_SIZE, o.width, o.height);
        if(SUCCEEDED(hr)) hr = MFSetAttributeRatio(out.p, MF_MT_FRAME_RATE, o.fps, 1);
        if(SUCCEEDED(hr)) hr = MFSetAttributeRatio(out.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        if(SUCCEEDED(hr)) hr = writer->AddStream(out.p, &videoStream);
        if(FAILED(hr))
            return "Couldn't set up H.264 encoding at " + std::to_string(o.width) + "x" + std::to_string(o.height) + " (" + hresult_string(hr) + ")";

        // NV12 is what the H.264 encoders take natively, so no color converter is inserted
        ComPtr<IMFMediaType> in;
        hr = MFCreateMediaType(in.out());
        if(SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if(SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(o.width));
        if(SUCCEEDED(hr)) hr = MFSetAttributeSize(in.p, MF_MT_FRAME_SIZE, o.width, o.height);
        if(SUCCEEDED(hr)) hr = MFSetAttributeRatio(in.p, MF_MT_FRAME_RATE, o.fps, 1);
        if(SUCCEEDED(hr)) hr = MFSetAttributeRatio(in.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        if(SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoStream, in.p, nullptr);
        if(FAILED(hr))
            return "This computer can't encode video at " + std::to_string(o.width) + "x" + std::to_string(o.height) + ", " + std::to_string(o.fps) + " fps. Try a lower resolution or frame rate. (" + hresult_string(hr) + ")";
        return "";
    }

    std::string add_audio_stream() {
        ComPtr<IMFMediaType> out;
        HRESULT hr = MFCreateMediaType(out.out());
        if(SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if(SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_SAMPLE_RATE);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        if(SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_BITRATE / 8);
        if(SUCCEEDED(hr)) hr = writer->AddStream(out.p, &audioStream);
        if(FAILED(hr))
            return "Couldn't set up AAC audio encoding (" + hresult_string(hr) + ")";

        ComPtr<IMFMediaType> in;
        hr = MFCreateMediaType(in.out());
        if(SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if(SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AUDIO_SAMPLE_RATE);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
        if(SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AUDIO_SAMPLE_RATE * 2);
        if(SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, in.p, nullptr);
        if(FAILED(hr))
            return "Couldn't set up AAC audio encoding (" + hresult_string(hr) + ")";
        return "";
    }

    // Times are in 100 ns units
    std::string write_sample(DWORD stream, const std::function<void(BYTE*)>& fill, DWORD bytes, LONGLONG time, LONGLONG duration) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(bytes, buffer.out());
        BYTE* data = nullptr;
        if(SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
        if(SUCCEEDED(hr)) {
            fill(data);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(bytes);
        }
        ComPtr<IMFSample> sample;
        if(SUCCEEDED(hr)) hr = MFCreateSample(sample.out());
        if(SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.p);
        if(SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
        if(SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
        if(SUCCEEDED(hr)) hr = writer->WriteSample(stream, sample.p);
        if(FAILED(hr))
            return "Couldn't encode the video (" + hresult_string(hr) + ")";
        return "";
    }

    // Audio is handed over alongside the video, so the writer can interleave it without buffering the whole soundtrack
    std::string write_audio_until(LONGLONG time) {
        if(!hasAudio)
            return "";
        while(pcmWritten < pcm.size()) {
            LONGLONG chunkTime = static_cast<LONGLONG>(pcmWritten / 2) * 10000000LL / AUDIO_SAMPLE_RATE;
            if(chunkTime > time)
                break;
            size_t bytes = std::min(AUDIO_CHUNK_SAMPLES * 2, pcm.size() - pcmWritten);
            LONGLONG duration = static_cast<LONGLONG>(bytes / 2) * 10000000LL / AUDIO_SAMPLE_RATE;
            const uint8_t* src = pcm.data() + pcmWritten;
            std::string e = write_sample(audioStream, [&](BYTE* dst) { std::memcpy(dst, src, bytes); }, static_cast<DWORD>(bytes), chunkTime, duration);
            if(!e.empty())
                return e;
            pcmWritten += bytes;
        }
        return "";
    }
};

bool VideoEncoder::is_supported() {
    return true;
}

VideoEncoder::VideoEncoder() {}
VideoEncoder::~VideoEncoder() {}

std::string VideoEncoder::start(const Options& options) {
    impl = std::make_unique<Impl>();
    Impl& d = *impl;
    d.o = options;

    // SDL may have set up COM on this thread already, in a different mode; Media Foundation works either way
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    d.comInitialized = SUCCEEDED(hr);
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if(FAILED(hr))
        return "Media Foundation isn't available on this computer (" + hresult_string(hr) + ")";
    d.mfStarted = true;

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(attributes.out(), 3);
    if(SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if(SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    if(SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if(SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(d.o.outPath.wstring().c_str(), nullptr, attributes.p, d.writer.out());
    if(FAILED(hr))
        return "Couldn't create the video file (" + hresult_string(hr) + ")";

    std::string e = d.add_video_stream();
    if(!e.empty())
        return e;
    if(!d.o.wavPath.empty()) {
        d.pcm = load_wav_pcm(d.o.wavPath);
        if(!d.pcm.empty()) {
            e = d.add_audio_stream();
            if(!e.empty())
                return e;
            d.hasAudio = true;
        }
    }
    hr = d.writer->BeginWriting();
    if(FAILED(hr))
        return "Couldn't start the video encoder (" + hresult_string(hr) + ")";
    return "";
}

std::string VideoEncoder::add_frame(const uint8_t* rgba, size_t rowBytes, int64_t ptsUs) {
    Impl& d = *impl;
    LONGLONG time = ptsUs * 10;
    std::string e = d.write_audio_until(time);
    if(!e.empty())
        return e;
    DWORD bytes = static_cast<DWORD>(static_cast<size_t>(d.o.width) * d.o.height * 3 / 2);
    return d.write_sample(d.videoStream, [&](BYTE* dst) {
        rgba_to_yuv420(rgba, rowBytes, d.o.width, d.o.height, dst, d.o.width, d.o.height, true);
    }, bytes, time, 10000000LL / d.o.fps);
}

std::string VideoEncoder::finish() {
    Impl& d = *impl;
    std::string e = d.write_audio_until(INT64_MAX);
    if(!e.empty())
        return e;
    HRESULT hr = d.writer->Finalize();
    d.writer.reset();
    if(FAILED(hr))
        return "Couldn't finish writing the MP4 (" + hresult_string(hr) + ")";
    d.finished = true;
    return "";
}

#else

struct VideoEncoder::Impl {};

bool VideoEncoder::is_supported() {
    return false;
}

VideoEncoder::VideoEncoder() {}
VideoEncoder::~VideoEncoder() {}

std::string VideoEncoder::start(const Options&) {
    return "Video export isn't available on this platform yet";
}

std::string VideoEncoder::add_frame(const uint8_t*, size_t, int64_t) {
    return "Video export isn't available on this platform yet";
}

std::string VideoEncoder::finish() {
    return "Video export isn't available on this platform yet";
}

#endif

}
