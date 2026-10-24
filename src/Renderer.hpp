#pragma once

#include "Spectrum.hpp"

#include <SFML/Graphics.hpp>

#include <random>
#include <vector>

enum class Mode
{
    Bars,
    Ring,
    Scope,
    Count
};

[[nodiscard]] const char* modeName(Mode mode);

// Everything the overlay needs to know about playback.
struct UiState
{
    bool loaded = false;
    bool loading = false;
    bool playing = false;
    sf::String title;
    float position = 0.f;  // seconds
    float duration = 0.f;  // seconds
    float uiAlpha = 1.f;   // overlay fades out when idle
    sf::String message;    // short toast, e.g. "Volume 80%"
    float messageAlpha = 0.f;
};

class Renderer
{
public:
    Renderer();

    void setFont(const sf::Font* font) { m_font = font; }
    void resize(sf::Vector2u size);
    void draw(sf::RenderWindow& window, const Spectrum& spectrum, Mode mode, float time, float dt, const UiState& ui);

    // Screen area of the seek bar (a bit taller than drawn, so it's easy to click).
    [[nodiscard]] sf::FloatRect seekBarHitBox() const;
    [[nodiscard]] float seekFractionAt(float x) const;

private:
    struct Particle
    {
        sf::Vector2f position;
        sf::Vector2f velocity;
        float life = 0.f;
        float maxLife = 1.f;
        float size = 2.f;
        float hue = 0.f;
    };

    void drawBackground(sf::RenderTarget& target, const Spectrum& spectrum);
    void drawBars(sf::VertexArray& out, const Spectrum& spectrum);
    void drawRing(sf::VertexArray& out, const Spectrum& spectrum);
    void drawScope(sf::VertexArray& out, const Spectrum& spectrum);
    void updateParticles(sf::VertexArray& out, const Spectrum& spectrum, Mode mode, float dt);
    void applyBloom(sf::RenderTarget& target, float strength);
    void drawOverlay(sf::RenderTarget& target, const UiState& ui, Mode mode);

    [[nodiscard]] sf::Color bandColor(float t, float brightness, float saturation = 0.85f) const;

    sf::Vector2f m_size{1280.f, 720.f};
    const sf::Font* m_font = nullptr;

    sf::RenderTexture m_scene;
    sf::RenderTexture m_blurA;
    sf::RenderTexture m_blurB;
    sf::Shader m_blur;
    bool m_bloomReady = false;

    std::vector<Particle> m_particles;
    std::mt19937 m_rng{1337};
    float m_spawnAccumulator = 0.f;

    float m_hue = 0.62f;       // slowly drifts over time
    float m_time = 0.f;
    float m_ringSpin = 0.f;
};
