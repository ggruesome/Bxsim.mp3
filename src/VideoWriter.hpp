#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Writes an MP4 (H.264 video + AAC audio).
// On Windows this uses Media Foundation, the encoder built into Windows 10/11, so nothing
// extra has to be installed. If that isn't available (or elsewhere), it falls back to piping
// frames into ffmpeg, if ffmpeg is installed.
class VideoWriter
{
public:
    VideoWriter();
    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    // samples: the whole song, interleaved 16-bit PCM.
    bool open(const std::filesystem::path& path,
              unsigned width,
              unsigned height,
              unsigned fps,
              const std::int16_t* samples,
              std::uint64_t frameCount,
              unsigned channels,
              unsigned sampleRate,
              std::string& error);

    // One video frame, top-down RGBA (exactly what sf::Image gives you). The matching slice of
    // audio is written along with it.
    bool addFrame(const std::uint8_t* rgba);

    // Writes any remaining audio and closes the file properly.
    bool finish();

    // Stops and closes without finishing (the partial file is left behind).
    void cancel();

    [[nodiscard]] std::uint64_t framesWritten() const { return m_frame; }

private:
    bool openMediaFoundation(const std::filesystem::path& path, std::string& error);
    bool addFrameMediaFoundation(const std::uint8_t* rgba);
    bool writeAudioUpTo(std::uint64_t sampleEnd);
    bool finishMediaFoundation();
    bool openFfmpeg(const std::filesystem::path& path, std::string& error);

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    unsigned m_srcWidth = 0; // width of the frames passed in (the video may be 1px narrower to stay even)
    unsigned m_width = 0;
    unsigned m_height = 0;
    unsigned m_fps = 60;
    unsigned m_channels = 2;
    unsigned m_rate = 48000;
    std::vector<std::int16_t> m_audio; // converted to what the AAC encoder accepts
    std::uint64_t m_audioWritten = 0;   // in sample frames
    std::uint64_t m_frame = 0;
    std::vector<std::uint8_t> m_scratch;
};
