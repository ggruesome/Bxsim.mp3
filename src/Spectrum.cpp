#include "Spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float Pi = 3.14159265358979f;

constexpr float MinFrequency = 30.f;     // lowest bar (Hz)
constexpr float MaxFrequency = 16000.f;  // highest bar (Hz)
constexpr float FloorDb = -66.f;         // anything quieter than this is an empty bar
constexpr float CeilingDb = -9.f;        // this loud or louder is a full bar
constexpr float TiltDbPerOctave = 3.f;   // lift the highs so the right side isn't always tiny

float smoothTowards(float current, float target, float rate, float dt)
{
    return current + (target - current) * (1.f - std::exp(-rate * dt));
}
} // namespace

Spectrum::Spectrum(std::size_t bandCount) : m_bandCount(bandCount)
{
    m_window.resize(FftSize);
    for (std::size_t i = 0; i < FftSize; ++i)
        m_window[i] = 0.5f - 0.5f * std::cos(2.f * Pi * static_cast<float>(i) / static_cast<float>(FftSize - 1));

    m_buffer.resize(FftSize);
    m_magnitudes.resize(FftSize / 2 + 1);

    m_twiddles.resize(FftSize / 2);
    for (std::size_t i = 0; i < FftSize / 2; ++i)
    {
        const float angle = -2.f * Pi * static_cast<float>(i) / static_cast<float>(FftSize);
        m_twiddles[i] = {std::cos(angle), std::sin(angle)};
    }

    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < FftSize)
        ++bits;
    m_bitReverse.resize(FftSize);
    for (std::size_t i = 0; i < FftSize; ++i)
    {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b)
            if (i & (std::size_t{1} << b))
                r |= std::size_t{1} << (bits - 1 - b);
        m_bitReverse[i] = r;
    }

    m_wave.assign(WaveformSize, 0.f);
    setBandCount(bandCount);
}

void Spectrum::setBandCount(std::size_t count)
{
    m_bandCount = std::max<std::size_t>(count, 8);
    m_bands.assign(m_bandCount, 0.f);
    m_peaks.assign(m_bandCount, 0.f);
    m_peakHold.assign(m_bandCount, 0.f);
    m_peakVelocity.assign(m_bandCount, 0.f);
    m_edgesForRate = 0; // force rebuild
}

void Spectrum::buildBandEdges(unsigned sampleRate)
{
    const float top = std::min(MaxFrequency, static_cast<float>(sampleRate) * 0.5f * 0.95f);
    m_edges.resize(m_bandCount + 1);
    const float ratio = top / MinFrequency;
    for (std::size_t i = 0; i <= m_bandCount; ++i)
        m_edges[i] = MinFrequency * std::pow(ratio, static_cast<float>(i) / static_cast<float>(m_bandCount));
    m_edgesForRate = sampleRate;
}

void Spectrum::fft(std::vector<std::complex<float>>& data) const
{
    const std::size_t n = data.size();
    for (std::size_t i = 0; i < n; ++i)
        if (i < m_bitReverse[i])
            std::swap(data[i], data[m_bitReverse[i]]);

    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const std::size_t half = len / 2;
        const std::size_t step = n / len;
        for (std::size_t start = 0; start < n; start += len)
        {
            for (std::size_t k = 0; k < half; ++k)
            {
                const std::complex<float> t = m_twiddles[k * step] * data[start + k + half];
                const std::complex<float> u = data[start + k];
                data[start + k] = u + t;
                data[start + k + half] = u - t;
            }
        }
    }
}

void Spectrum::update(const std::int16_t* samples,
                      std::uint64_t frameCount,
                      unsigned channels,
                      unsigned sampleRate,
                      std::uint64_t frame,
                      float dt,
                      bool playing)
{
    dt = std::clamp(dt, 0.f, 0.1f);
    const bool haveAudio = playing && samples && frameCount > 0 && channels > 0 && sampleRate > 0;

    std::vector<float> target(m_bandCount, 0.f);
    float rawBass = 0.f;

    if (haveAudio)
    {
        if (m_edgesForRate != sampleRate)
            buildBandEdges(sampleRate);

        // Mono mix of the samples around the playhead.
        auto monoAt = [&](std::int64_t f) -> float
        {
            if (f < 0 || static_cast<std::uint64_t>(f) >= frameCount)
                return 0.f;
            const std::int16_t* s = samples + static_cast<std::uint64_t>(f) * channels;
            float sum = 0.f;
            for (unsigned c = 0; c < channels; ++c)
                sum += static_cast<float>(s[c]);
            return sum / (32768.f * static_cast<float>(channels));
        };

        const std::int64_t center = static_cast<std::int64_t>(frame);
        const std::int64_t fftStart = center - static_cast<std::int64_t>(FftSize / 2);
        for (std::size_t i = 0; i < FftSize; ++i)
            m_buffer[i] = {monoAt(fftStart + static_cast<std::int64_t>(i)) * m_window[i], 0.f};

        const std::int64_t waveStart = center - static_cast<std::int64_t>(WaveformSize / 2);
        for (std::size_t i = 0; i < WaveformSize; ++i)
            m_wave[i] = monoAt(waveStart + static_cast<std::int64_t>(i));

        fft(m_buffer);

        // A full-scale sine through a Hann window peaks at FftSize/4.
        const float norm = 4.f / static_cast<float>(FftSize);
        for (std::size_t i = 0; i < m_magnitudes.size(); ++i)
            m_magnitudes[i] = std::abs(m_buffer[i]) * norm;

        const float binHz = static_cast<float>(sampleRate) / static_cast<float>(FftSize);
        auto magnitudeAt = [&](float hz)
        {
            const float pos = hz / binHz;
            const auto i = static_cast<std::size_t>(pos);
            if (i + 1 >= m_magnitudes.size())
                return m_magnitudes.back();
            const float frac = pos - static_cast<float>(i);
            return m_magnitudes[i] * (1.f - frac) + m_magnitudes[i + 1] * frac;
        };

        int bassBands = 0;
        for (std::size_t b = 0; b < m_bandCount; ++b)
        {
            const float lo = m_edges[b];
            const float hi = m_edges[b + 1];
            const float centerHz = std::sqrt(lo * hi);

            float mag = 0.f;
            const auto first = static_cast<std::size_t>(std::ceil(lo / binHz));
            const auto last = static_cast<std::size_t>(std::floor(hi / binHz));
            if (last >= first + 1)
            {
                for (std::size_t i = first; i <= last && i < m_magnitudes.size(); ++i)
                    mag = std::max(mag, m_magnitudes[i]);
            }
            else
            {
                mag = magnitudeAt(centerHz); // narrower than one bin: interpolate
            }

            float db = 20.f * std::log10(mag + 1e-9f);
            db += TiltDbPerOctave * std::log2(centerHz / 1000.f);
            float v = std::clamp((db - FloorDb) / (CeilingDb - FloorDb), 0.f, 1.f);
            v = std::pow(v, 1.2f);
            target[b] = v;

            if (centerHz < 140.f)
            {
                rawBass += v;
                ++bassBands;
            }
        }
        if (bassBands > 0)
            rawBass /= static_cast<float>(bassBands);
    }
    else
    {
        for (float& w : m_wave)
            w = smoothTowards(w, 0.f, 10.f, dt);
    }

    // Bars: snap up quickly, fall smoothly.
    float total = 0.f;
    for (std::size_t b = 0; b < m_bandCount; ++b)
    {
        const float rate = target[b] > m_bands[b] ? 32.f : 7.f;
        m_bands[b] = smoothTowards(m_bands[b], target[b], rate, dt);
        total += m_bands[b];

        // Peak caps: stick for a moment, then fall with gravity.
        if (m_bands[b] >= m_peaks[b])
        {
            m_peaks[b] = m_bands[b];
            m_peakHold[b] = 0.35f;
            m_peakVelocity[b] = 0.f;
        }
        else if (m_peakHold[b] > 0.f)
        {
            m_peakHold[b] -= dt;
        }
        else
        {
            m_peakVelocity[b] += 2.2f * dt;
            m_peaks[b] = std::max(m_bands[b], m_peaks[b] - m_peakVelocity[b] * dt);
        }
    }
    m_energy = total / static_cast<float>(m_bandCount);

    // Bass + a simple beat detector (bass jumping above its recent average).
    m_bass = smoothTowards(m_bass, rawBass, rawBass > m_bass ? 30.f : 8.f, dt);
    m_bassAverage = smoothTowards(m_bassAverage, m_bass, 1.5f, dt);
    const float kick = std::clamp((m_bass - m_bassAverage) * 4.f, 0.f, 1.f);
    m_beat = std::max(m_beat * std::exp(-6.f * dt), kick);
}
