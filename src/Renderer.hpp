#pragma once

#include "Settings.hpp"
#include "Spectrum.hpp"

#include <SFML/Graphics.hpp>

#include <filesystem>
#include <functional>
#include <random>
#include <vector>

enum class Mode
{
    Bars,
    Ring,
    Scope,
    XYScope,
    Tunnel,
    Liquid,
    Oscilloscope,
    DarkFantasy,
    Pop,
    NightDrive,
    DarkForest,
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
    bool showFps = false;
    float fps = 0.f;
};

class Renderer
{
public:
    Renderer();

    // Builds every shader (the slow part of starting up). progress(0..1, what) is called as it goes.
    void compileShaders(const std::function<void(float, const char*)>& progress);

    void setFont(const sf::Font* font) { m_font = font; }
    void setSettings(const Settings* settings) { m_settings = settings; }

    // An optional picture that reacts to the music in every mode.
    bool setImage(const std::filesystem::path& path);
    void clearImage() { m_hasImage = false; }
    void restartImageEffect() { m_imageAge = 0.f; } // the picture starts clear and melts again
    [[nodiscard]] bool hasImage() const { return m_hasImage; }

    // Post-processing filter over any mode (0 = none).
    void setFilter(int index) { m_filter = ((index % filterCount()) + filterCount()) % filterCount(); }
    [[nodiscard]] int filter() const { return m_filter; }
    [[nodiscard]] static int filterCount();
    [[nodiscard]] static const char* filterName(int index);

    // Oscilloscope: XY (left vs right) or a classic time sweep. Returns true for sweep.
    bool toggleOscilloscopeSweep() { return m_oscSweep = !m_oscSweep; }
    void resize(sf::Vector2u size);
    void draw(sf::RenderTarget& window, const Spectrum& spectrum, Mode mode, float time, float dt, const UiState& ui);

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
    void drawLiquid(const Spectrum& spectrum);
    void drawDarkFantasy(const Spectrum& spectrum, float dt);
    void drawPop(const Spectrum& spectrum, float dt);
    void drawNightDrive(const Spectrum& spectrum, float dt, const UiState& ui);
    void drawDarkForest(const Spectrum& spectrum, float dt);
    void drawRadioText(const UiState& ui);
    void drawFeedback(const Spectrum& spectrum, Mode mode, float dt);
    void drawXYCurve(sf::VertexArray& out, const Spectrum& spectrum);
    void drawTunnelSeed(sf::VertexArray& out, const Spectrum& spectrum);
    void drawOscilloscopeBeam(sf::VertexArray& out, const Spectrum& spectrum);
    void drawImageLaser(sf::VertexArray& out, const Spectrum& spectrum, sf::FloatRect screen);
    void drawGraticule(sf::VertexArray& out) const;
    [[nodiscard]] sf::FloatRect oscilloscopeScreen() const;
    void drawImageBackground(sf::RenderTarget& target, const Spectrum& spectrum, Mode mode);
    void updateAudioTexture(const Spectrum& spectrum);
    void buildOutline(const sf::Image& picture);
    [[nodiscard]] float imageAmount() const;
    [[nodiscard]] sf::Glsl::Vec2 imageFitScale() const;
    void applyBloom(sf::RenderTarget& target, float strength);
    void drawOverlay(sf::RenderTarget& target, const UiState& ui, Mode mode);

    [[nodiscard]] sf::Color bandColor(float t, float brightness, float saturation = 0.85f) const;
    [[nodiscard]] float cfg(const char* key) const;

    sf::Vector2f m_size{1280.f, 720.f};
    const sf::Font* m_font = nullptr;
    const Settings* m_settings = nullptr;

    sf::RenderTexture m_scene;
    sf::RenderTexture m_blurA;
    sf::RenderTexture m_blurB;
    sf::Shader m_blur;
    bool m_bloomReady = false;

    sf::RenderTexture m_post;     // finished frame, before the filter
    sf::Shader m_filterShader;
    bool m_filterReady = false;
    int m_filter = 0;

    // Trippy modes: a full-screen fluid shader, and a feedback loop that keeps
    // re-drawing the previous frame slightly zoomed/rotated/recolored.
    sf::Shader m_liquid;
    sf::Shader m_feedbackShader;
    sf::Shader m_finish;          // scanlines, grain, vignette for Liquid and Oscilloscope
    sf::Shader m_imageShader;     // the reacting picture behind the other modes
    sf::RenderTexture m_liquidTargets[2]; // Liquid renders at reduced resolution (two, for echo trails)
    int m_liquidIndex = 0;
    float m_liquidScale = 1.f;
    bool m_trippyReady = false;
    sf::RenderTexture m_feed[2];
    int m_feedIndex = 0;
    Mode m_lastMode = Mode::Count;
    float m_flow = 0.f;        // Liquid mode's clock; runs faster when the music is loud
    float m_xyGain = 2.f;      // auto-gain so quiet songs still fill the XY scope
    float m_oscGain = 1.f;
    bool m_oscSweep = false;

    sf::Texture m_image;
    bool m_hasImage = false;
    float m_imageAge = 0.f;                              // seconds since the picture opened
    sf::Texture m_audioTexture;                          // spectrum + waveform for the image shader
    std::vector<std::vector<sf::Vector2f>> m_outline;    // picture traced into strokes for the oscilloscope

    std::vector<Particle> m_particles;
    std::mt19937 m_rng{1337};
    float m_spawnAccumulator = 0.f;

    float m_hue = 0.62f;       // slowly drifts over time
    float m_time = 0.f;
    float m_ringSpin = 0.f;
    float m_ringAngle = 0.f;

    // Dark Fantasy
    struct Bat
    {
        sf::Vector2f position;
        sf::Vector2f velocity;
        float size = 8.f;
        float phase = 0.f;
    };
    sf::Shader m_fantasy;           // with a picture: the picture as the scene
    sf::RenderTexture m_fantasyTarget;
    sf::Shader m_fantasy3D;         // without: flying down a village street toward the castle
    sf::RenderTexture m_fantasy3DTarget;
    sf::RenderTexture m_overlayLayer;   // bats + lightning before the sky mask is applied
    sf::Shader m_skyMask;
    float m_camZ = 0.f;
    std::vector<Bat> m_bats;
    float m_batSpawn = 0.f;
    std::vector<std::vector<sf::Vector2f>> m_bolts;
    float m_boltLife = 0.f;
    float m_flash = 0.f;
    float m_lightningCooldown = 0.f;

    // Pop
    struct PopShape
    {
        int kind = 0; // 0 starburst, 1 ring, 2 confetti, 3 sparkle
        sf::Vector2f position;
        sf::Vector2f velocity;
        float size = 10.f;
        float angle = 0.f;
        float spin = 0.f;
        float life = 1.f;
        float maxLife = 1.f;
        float squash = 1.f;
        int sides = 4;
        sf::Color colors[3];
        std::vector<float> jitter;
    };
    sf::Shader m_pop;
    std::vector<PopShape> m_pops;
    float m_popLastBeat = 0.f;
    float m_popRingTimer = 0.f;
    float m_popConfetti = 0.f;

    // Night Drive
    sf::Shader m_drive;
    sf::RenderTexture m_driveTarget;
    float m_driveTravel = 0.f;
    float m_driveBass = 0.f;
    float m_wheel = 0.f;

    // Dark Forest
    sf::Shader m_forest;
    sf::RenderTexture m_forestTarget;
    float m_forestZ = 0.f;
    float m_forestBass = 0.f;
    sf::Vector2f m_forestLook;
    sf::Vector2f m_forestLookTarget;
    sf::Vector2f m_forestLookVel;
    float m_forestLookTimer = 0.f;
    float m_forestFlicker = 0.f;
    sf::RenderTexture m_radioText;   // the scrolling song title on the dashboard screen
};
