#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

// Turns raw audio samples into what the renderer draws:
//   - bands():    log-spaced frequency bars, 0..1, smoothed so they rise fast and fall gracefully
//   - peaks():    the little "caps" that hang above each bar and drop slowly
//   - waveform(): the current chunk of audio as a mono line, -1..1
//   - bass(), beat(), energy(): single numbers for pulsing backgrounds, particles, etc.
class Spectrum
{
public:
    static constexpr std::size_t FftSize = 4096;      // ~93 ms at 44.1 kHz: good bass resolution
    static constexpr std::size_t WaveformSize = 1024;
    static constexpr std::size_t StereoSize = 1600;   // ~36 ms of left/right audio for the XY scope

    explicit Spectrum(std::size_t bandCount = 96);

    // samples: interleaved 16-bit PCM for the whole song.
    // frame:   the frame (sample per channel) currently being heard.
    void update(const std::int16_t* samples,
                std::uint64_t frameCount,
                unsigned channels,
                unsigned sampleRate,
                std::uint64_t frame,
                float dt,
                bool playing);

    void setBandCount(std::size_t count);

    // Reactivity settings.
    void setSensitivity(float s) { m_sensitivity = s; }   // 1 = normal, higher = bars jump more
    void setSmoothing(float s) { m_smoothing = s; }       // 0 = snappy, 1 = slow and floaty
    void setKick(float k) { m_kick = k; }                 // how hard beats hit

    [[nodiscard]] const std::vector<float>& bands() const { return m_bands; }
    [[nodiscard]] const std::vector<float>& peaks() const { return m_peaks; }
    [[nodiscard]] const std::vector<float>& waveform() const { return m_wave; }
    [[nodiscard]] const std::vector<float>& left() const { return m_left; }
    [[nodiscard]] const std::vector<float>& right() const { return m_right; }
    [[nodiscard]] float treble() const { return m_treble; }
    [[nodiscard]] float bass() const { return m_bass; }
    [[nodiscard]] float beat() const { return m_beat; }
    [[nodiscard]] float energy() const { return m_energy; }

private:
    void buildBandEdges(unsigned sampleRate);
    void fft(std::vector<std::complex<float>>& data) const;

    std::size_t m_bandCount;
    unsigned m_edgesForRate = 0;

    std::vector<float> m_window;                  // Hann window
    std::vector<std::complex<float>> m_buffer;    // FFT working buffer
    std::vector<std::complex<float>> m_twiddles;
    std::vector<std::size_t> m_bitReverse;
    std::vector<float> m_magnitudes;              // per FFT bin

    std::vector<float> m_edges;                   // band edges in Hz (bandCount + 1)
    std::vector<float> m_bands;
    std::vector<float> m_peaks;
    std::vector<float> m_peakHold;                // seconds left before a cap starts falling
    std::vector<float> m_peakVelocity;
    std::vector<float> m_wave;
    std::vector<float> m_left;
    std::vector<float> m_right;
    float m_treble = 0.f;
    float m_sensitivity = 1.f;
    float m_smoothing = 0.5f;
    float m_kick = 1.f;

    float m_bass = 0.f;
    float m_bassAverage = 0.f;
    float m_beat = 0.f;
    float m_energy = 0.f;
};
