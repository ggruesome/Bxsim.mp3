#include "VideoWriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace
{
constexpr std::uint32_t VideoBitrate = 20'000'000; // 20 Mbps: plenty for busy, trippy 1080p60
constexpr std::uint32_t AudioBytesPerSecond = 24000; // 192 kbps AAC

// The AAC encoder only takes 44.1/48 kHz, mono or stereo. Convert anything else.
std::vector<std::int16_t> prepareAudio(const std::int16_t* samples,
                                       std::uint64_t frames,
                                       unsigned channels,
                                       unsigned rate,
                                       unsigned& outChannels,
                                       unsigned& outRate)
{
    outChannels = channels >= 2 ? 2 : 1;
    outRate = (rate == 44100 || rate == 48000) ? rate : 48000;
    if (!samples || frames == 0 || channels == 0)
        return {};

    const double step = static_cast<double>(rate) / static_cast<double>(outRate);
    const auto outFrames = static_cast<std::uint64_t>(static_cast<double>(frames) / step);
    std::vector<std::int16_t> out(outFrames * outChannels);
    for (std::uint64_t i = 0; i < outFrames; ++i)
    {
        const double pos = static_cast<double>(i) * step;
        const auto i0 = std::min(static_cast<std::uint64_t>(pos), frames - 1);
        const auto i1 = std::min(i0 + 1, frames - 1);
        const double frac = pos - static_cast<double>(i0);
        for (unsigned c = 0; c < outChannels; ++c)
        {
            // Stereo keeps left/right (extra channels are dropped); mono stays mono.
            const double a = samples[i0 * channels + c];
            const double b = samples[i1 * channels + c];
            out[i * outChannels + c] = static_cast<std::int16_t>(std::lround(a + (b - a) * frac));
        }
    }
    return out;
}

#ifdef _WIN32
// RGBA (top-down) -> NV12 with BT.709 video-range colors: a full-res Y plane followed by
// interleaved U/V at half resolution.
void rgbaToNv12(const std::uint8_t* rgba, unsigned srcWidth, unsigned width, unsigned height, std::uint8_t* out)
{
    std::uint8_t* yPlane = out;
    std::uint8_t* uvPlane = out + static_cast<std::size_t>(width) * height;
    auto clamp8 = [](float v) { return static_cast<std::uint8_t>(std::clamp(v + 0.5f, 0.f, 255.f)); };
    for (unsigned y = 0; y < height; ++y)
    {
        const std::uint8_t* row = rgba + static_cast<std::size_t>(y) * srcWidth * 4;
        std::uint8_t* yRow = yPlane + static_cast<std::size_t>(y) * width;
        for (unsigned x = 0; x < width; ++x)
        {
            const float r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            yRow[x] = clamp8(16.f + 0.1826f * r + 0.6142f * g + 0.0620f * b);
        }
    }
    for (unsigned y = 0; y < height; y += 2)
    {
        const std::uint8_t* row0 = rgba + static_cast<std::size_t>(y) * srcWidth * 4;
        const std::uint8_t* row1 = rgba + static_cast<std::size_t>(y + 1) * srcWidth * 4;
        std::uint8_t* uvRow = uvPlane + static_cast<std::size_t>(y / 2) * width;
        for (unsigned x = 0; x < width; x += 2)
        {
            const float r = (row0[x * 4] + row0[x * 4 + 4] + row1[x * 4] + row1[x * 4 + 4]) * 0.25f;
            const float g = (row0[x * 4 + 1] + row0[x * 4 + 5] + row1[x * 4 + 1] + row1[x * 4 + 5]) * 0.25f;
            const float b = (row0[x * 4 + 2] + row0[x * 4 + 6] + row1[x * 4 + 2] + row1[x * 4 + 6]) * 0.25f;
            uvRow[x] = clamp8(128.f - 0.1006f * r - 0.3386f * g + 0.4392f * b);
            uvRow[x + 1] = clamp8(128.f + 0.4392f * r - 0.3989f * g - 0.0403f * b);
        }
    }
}
#endif
} // namespace

#ifdef _WIN32

void shutdownMediaFoundation();

struct VideoWriter::Impl
{
    // ffmpeg fallback
    FILE* pipe = nullptr;
    std::filesystem::path wavPath;
    // Media Foundation
    IMFSinkWriter* writer = nullptr;
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    bool comStarted = false;
    bool mfStarted = false;

    ~Impl() { close(); }

    void closePipe();

    void close()
    {
        closePipe();
        if (writer)
        {
            writer->Release();
            writer = nullptr;
        }
        if (mfStarted)
        {
            shutdownMediaFoundation();
            mfStarted = false;
        }
        if (comStarted)
        {
            CoUninitialize();
            comStarted = false;
        }
    }
};

namespace
{
template <typename T>
void release(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

std::string hrText(const char* what, HRESULT hr)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

// Media Foundation is loaded only when exporting, so Bxsim still starts on Windows "N" editions
// that don't ship it (export then explains what's missing instead of the app failing to open).
struct MfApi
{
    HMODULE plat = nullptr;
    HMODULE readwrite = nullptr;
    HRESULT(WINAPI* startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* shutdown)() = nullptr;
    HRESULT(WINAPI* createAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* createMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* createMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* createSample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* createSinkWriterFromURL)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**) = nullptr;

    bool load()
    {
        if (startup)
            return true;
        plat = LoadLibraryW(L"mfplat.dll");
        readwrite = LoadLibraryW(L"mfreadwrite.dll");
        if (!plat || !readwrite)
            return false;
        auto get = [](HMODULE m, const char* name) { return reinterpret_cast<void*>(GetProcAddress(m, name)); };
        startup = reinterpret_cast<decltype(startup)>(get(plat, "MFStartup"));
        shutdown = reinterpret_cast<decltype(shutdown)>(get(plat, "MFShutdown"));
        createAttributes = reinterpret_cast<decltype(createAttributes)>(get(plat, "MFCreateAttributes"));
        createMediaType = reinterpret_cast<decltype(createMediaType)>(get(plat, "MFCreateMediaType"));
        createMemoryBuffer = reinterpret_cast<decltype(createMemoryBuffer)>(get(plat, "MFCreateMemoryBuffer"));
        createSample = reinterpret_cast<decltype(createSample)>(get(plat, "MFCreateSample"));
        createSinkWriterFromURL = reinterpret_cast<decltype(createSinkWriterFromURL)>(get(readwrite, "MFCreateSinkWriterFromURL"));
        return startup && shutdown && createAttributes && createMediaType && createMemoryBuffer && createSample &&
               createSinkWriterFromURL;
    }
};
MfApi mf;
} // namespace

void shutdownMediaFoundation()
{
    if (mf.shutdown)
        mf.shutdown();
}


bool VideoWriter::openMediaFoundation(const std::filesystem::path& path, std::string& error)
{
    Impl& im = *m_impl;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    im.comStarted = SUCCEEDED(hr); // RPC_E_CHANGED_MODE means COM is already up; that's fine too
    if (!mf.load())
    {
        error = "Video export needs Windows Media Foundation (install the Media Feature Pack on Windows N editions)";
        return false;
    }
    hr = mf.startup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr))
    {
        error = hrText("Starting Media Foundation", hr);
        return false;
    }
    im.mfStarted = true;

    IMFAttributes* attributes = nullptr;
    mf.createAttributes(&attributes, 1);
    if (attributes)
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE); // use the GPU encoder when there is one
    hr = mf.createSinkWriterFromURL(path.wstring().c_str(), nullptr, attributes, &im.writer);
    release(attributes);
    if (FAILED(hr))
    {
        error = hrText("Creating the MP4 file", hr);
        return false;
    }

    // ---- Video: H.264 out, 32-bit RGB in ----
    auto addVideoStream = [&](bool highProfile) -> HRESULT
    {
        IMFMediaType* out = nullptr;
        mf.createMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, VideoBitrate);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, m_fps, 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (highProfile)
            out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        const HRESULT r = im.writer->AddStream(out, &im.videoStream);
        release(out);
        return r;
    };
    hr = addVideoStream(true);
    if (FAILED(hr))
        hr = addVideoStream(false); // older encoders: fall back to the default profile
    if (FAILED(hr))
    {
        error = hrText("Setting up the H.264 encoder", hr);
        return false;
    }

    IMFMediaType* videoIn = nullptr;
    mf.createMediaType(&videoIn);
    videoIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    videoIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12); // what H.264 encoders take natively
    videoIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(videoIn, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(videoIn, MF_MT_FRAME_RATE, m_fps, 1);
    MFSetAttributeRatio(videoIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    videoIn->SetUINT32(MF_MT_DEFAULT_STRIDE, m_width);
    hr = im.writer->SetInputMediaType(im.videoStream, videoIn, nullptr);
    release(videoIn);
    if (FAILED(hr))
    {
        error = hrText("Setting the video input format", hr);
        return false;
    }

    // ---- Audio: AAC out, 16-bit PCM in ----
    if (!m_audio.empty())
    {
        IMFMediaType* audioOut = nullptr;
        mf.createMediaType(&audioOut);
        audioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        audioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_rate);
        audioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
        audioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioBytesPerSecond);
        hr = im.writer->AddStream(audioOut, &im.audioStream);
        release(audioOut);
        if (FAILED(hr))
        {
            error = hrText("Setting up the AAC encoder", hr);
            return false;
        }

        IMFMediaType* audioIn = nullptr;
        mf.createMediaType(&audioIn);
        audioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        audioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        audioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        audioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_rate);
        audioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
        audioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, m_channels * 2);
        audioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_rate * m_channels * 2);
        hr = im.writer->SetInputMediaType(im.audioStream, audioIn, nullptr);
        release(audioIn);
        if (FAILED(hr))
        {
            error = hrText("Setting the audio input format", hr);
            return false;
        }
    }

    hr = im.writer->BeginWriting();
    if (FAILED(hr))
    {
        error = hrText("Starting the export", hr);
        return false;
    }
    m_scratch.resize(static_cast<std::size_t>(m_width) * m_height * 3 / 2);
    return true;
}

bool VideoWriter::addFrameMediaFoundation(const std::uint8_t* rgba)
{
    Impl& im = *m_impl;
    if (!im.writer)
        return false;

    const DWORD size = static_cast<DWORD>(m_scratch.size());
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(mf.createMemoryBuffer(size, &buffer)))
        return false;
    BYTE* data = nullptr;
    buffer->Lock(&data, nullptr, nullptr);
    rgbaToNv12(rgba, m_srcWidth, m_width, m_height, data);
    buffer->Unlock();
    buffer->SetCurrentLength(size);

    IMFSample* sample = nullptr;
    mf.createSample(&sample);
    sample->AddBuffer(buffer);
    const LONGLONG duration = 10'000'000LL / m_fps;
    sample->SetSampleTime(static_cast<LONGLONG>(m_frame) * 10'000'000LL / m_fps);
    sample->SetSampleDuration(duration);
    const HRESULT hr = im.writer->WriteSample(im.videoStream, sample);
    release(sample);
    release(buffer);
    if (FAILED(hr))
        return false;

    ++m_frame;
    return writeAudioUpTo(m_frame * m_rate / m_fps);
}

bool VideoWriter::writeAudioUpTo(std::uint64_t sampleEnd)
{
    Impl& im = *m_impl;
    const std::uint64_t total = m_audio.size() / m_channels;
    sampleEnd = std::min(sampleEnd, total);
    if (m_audio.empty() || sampleEnd <= m_audioWritten)
        return true;

    const std::uint64_t count = sampleEnd - m_audioWritten;
    const DWORD bytes = static_cast<DWORD>(count * m_channels * 2);
    IMFMediaBuffer* buffer = nullptr;
    if (FAILED(mf.createMemoryBuffer(bytes, &buffer)))
        return false;
    BYTE* data = nullptr;
    buffer->Lock(&data, nullptr, nullptr);
    std::copy_n(reinterpret_cast<const BYTE*>(m_audio.data() + m_audioWritten * m_channels), bytes, data);
    buffer->Unlock();
    buffer->SetCurrentLength(bytes);

    IMFSample* sample = nullptr;
    mf.createSample(&sample);
    sample->AddBuffer(buffer);
    sample->SetSampleTime(static_cast<LONGLONG>(m_audioWritten * 10'000'000ULL / m_rate));
    sample->SetSampleDuration(static_cast<LONGLONG>(count * 10'000'000ULL / m_rate));
    const HRESULT hr = im.writer->WriteSample(im.audioStream, sample);
    release(sample);
    release(buffer);
    m_audioWritten = sampleEnd;
    return SUCCEEDED(hr);
}

bool VideoWriter::finishMediaFoundation()
{
    Impl& im = *m_impl;
    if (!im.writer)
        return false;
    writeAudioUpTo(m_audio.size() / std::max(1u, m_channels));
    const HRESULT hr = im.writer->Finalize();
    im.close();
    return SUCCEEDED(hr);
}

#endif

// ---------------------------------------------------------------- shared + ffmpeg

namespace
{
void writeWav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm, unsigned channels, unsigned rate)
{
#ifdef _WIN32
    FILE* f = _wfopen(path.wstring().c_str(), L"wb");
#else
    FILE* f = std::fopen(path.string().c_str(), "wb");
#endif
    if (!f)
        return;
    auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const auto dataBytes = static_cast<std::uint32_t>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(static_cast<std::uint16_t>(channels));
    u32(rate);
    u32(rate * channels * 2);
    u16(static_cast<std::uint16_t>(channels * 2));
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}

FILE* openPipe(const std::filesystem::path::string_type& command, bool forWriting)
{
#ifdef _WIN32
    // cmd.exe strips the outer quotes of the whole line, so wrap it once more to keep the
    // quotes around file names intact.
    const std::wstring wrapped = L"\"" + command + L"\"";
    return _wpopen(wrapped.c_str(), forWriting ? L"wb" : L"r");
#else
    return popen(command.c_str(), forWriting ? "w" : "r");
#endif
}

int closePipeHandle(FILE* pipe)
{
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

bool ffmpegAvailable()
{
    FILE* p = openPipe(std::filesystem::path("ffmpeg -version").native(), false);
    if (!p)
        return false;
    char line[64] = {};
    const bool ok = std::fgets(line, sizeof(line), p) && std::string(line).rfind("ffmpeg", 0) == 0;
    closePipeHandle(p);
    return ok;
}
} // namespace

#ifndef _WIN32
struct VideoWriter::Impl
{
    FILE* pipe = nullptr;
    std::filesystem::path wavPath;
    void closePipe();
    void close() { closePipe(); }
    ~Impl() { close(); }
};
#endif

void VideoWriter::Impl::closePipe()
{
    if (pipe)
    {
        closePipeHandle(pipe);
        pipe = nullptr;
    }
    if (!wavPath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(wavPath, ec);
        wavPath.clear();
    }
}

VideoWriter::VideoWriter() : m_impl(std::make_unique<Impl>()) {}
VideoWriter::~VideoWriter() { cancel(); }

bool VideoWriter::open(const std::filesystem::path& path,
                       unsigned width,
                       unsigned height,
                       unsigned fps,
                       const std::int16_t* samples,
                       std::uint64_t frameCount,
                       unsigned channels,
                       unsigned sampleRate,
                       std::string& error)
{
    m_srcWidth = width;
    m_width = width & ~1u; // H.264 needs even sizes
    m_height = height & ~1u;
    m_fps = fps;
    m_frame = 0;
    m_audioWritten = 0;
    m_audio = prepareAudio(samples, frameCount, channels, sampleRate, m_channels, m_rate);

#ifdef _WIN32
    if (openMediaFoundation(path, error))
        return true;
    m_impl->close();
    std::string ffmpegError;
    if (openFfmpeg(path, ffmpegError))
        return true;
    error += ". Installing ffmpeg also makes export work.";
    return false;
#else
    return openFfmpeg(path, error);
#endif
}

bool VideoWriter::openFfmpeg(const std::filesystem::path& path, std::string& error)
{
    if (!ffmpegAvailable())
    {
        error = "ffmpeg wasn't found";
        return false;
    }
    m_impl->wavPath = std::filesystem::path(path).replace_extension(".bxsim-audio.wav");
    writeWav(m_impl->wavPath, m_audio, m_channels, m_rate);

    const std::filesystem::path command = std::filesystem::u8path(
        "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(m_srcWidth) + "x" +
        std::to_string(m_height) + " -r " + std::to_string(m_fps) + " -i - -i \"" + m_impl->wavPath.u8string() +
        "\" -vf crop=" + std::to_string(m_width) + ":" + std::to_string(m_height) +
        ":0:0 -c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -c:a aac -b:a 192k -shortest \"" +
        path.u8string() + "\"");
    m_impl->pipe = openPipe(command.native(), true);
    if (!m_impl->pipe)
    {
        error = "Couldn't start ffmpeg";
        return false;
    }
    return true;
}

bool VideoWriter::addFrame(const std::uint8_t* rgba)
{
    if (m_impl->pipe)
    {
        const std::size_t bytes = static_cast<std::size_t>(m_srcWidth) * m_height * 4;
        if (std::fwrite(rgba, 1, bytes, m_impl->pipe) != bytes)
            return false;
        ++m_frame;
        return true;
    }
#ifdef _WIN32
    return addFrameMediaFoundation(rgba);
#else
    return false;
#endif
}

bool VideoWriter::finish()
{
    if (m_impl->pipe)
    {
        const int status = closePipeHandle(m_impl->pipe);
        m_impl->pipe = nullptr;
        m_impl->closePipe(); // removes the temporary audio file
        return status == 0;
    }
#ifdef _WIN32
    return finishMediaFoundation();
#else
    return false;
#endif
}

void VideoWriter::cancel()
{
    if (m_impl)
        m_impl->close();
}
