// Bxsim — a real-time audio visualizer.
//
// Open a song (drop it on the window, drag it onto Bxsim.exe, or press O) and it
// plays while the spectrum, waveform and beat drive the visuals.

#include "Platform.hpp"
#include "Renderer.hpp"
#include "Spectrum.hpp"

#include <SFML/Audio.hpp>
#include <SFML/Graphics.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
const sf::Vector2u WindowedSize{1280, 720};

sf::Image makeIcon()
{
    // A tiny procedurally drawn icon: five glowing bars.
    constexpr unsigned size = 64;
    sf::Image icon({size, size}, sf::Color(14, 12, 28));
    const float heights[] = {0.45f, 0.8f, 0.6f, 0.95f, 0.5f};
    for (unsigned b = 0; b < 5; ++b)
    {
        const unsigned x0 = 8 + b * 10;
        const auto top = static_cast<unsigned>(size - 8 - heights[b] * 44.f);
        for (unsigned y = top; y < size - 8; ++y)
        {
            const float t = static_cast<float>(y - top) / static_cast<float>(size - 8 - top);
            const sf::Color c(static_cast<std::uint8_t>(120 + 135 * (1.f - t)),
                              static_cast<std::uint8_t>(80 + 120 * (1.f - t)),
                              255);
            for (unsigned x = x0; x < x0 + 7; ++x)
                icon.setPixel({x, y}, c);
        }
    }
    return icon;
}

struct App
{
    sf::RenderWindow window;
    bool fullscreen = false;
    sf::Image icon = makeIcon();

    Renderer* renderer = nullptr;

    std::unique_ptr<sf::SoundBuffer> buffer;
    std::optional<sf::Sound> sound;
    fs::path currentPath;
    bool wantPlaying = false;
    bool looping = false;
    float volume = 80.f;

    UiState ui;
    float messageTimer = 0.f;

    void createWindow()
    {
        sf::ContextSettings settings;
        settings.antiAliasingLevel = 4;
        if (fullscreen)
        {
            // Borderless window covering the screen: instant alt-tab, no mode switch.
            window.create(sf::VideoMode::getDesktopMode(), "Bxsim", sf::Style::None, sf::State::Windowed, settings);
            window.setPosition({0, 0});
        }
        else
        {
            window.create(sf::VideoMode(WindowedSize), "Bxsim", sf::Style::Default, sf::State::Windowed, settings);
        }
        window.setVerticalSyncEnabled(true);
        window.setKeyRepeatEnabled(true);
        window.setIcon(icon);
        platform::enableFileDrop(window.getNativeHandle());
        onResize(window.getSize());
    }

    void onResize(sf::Vector2u size)
    {
        window.setView(sf::View(sf::FloatRect({0.f, 0.f}, sf::Vector2f(size))));
        if (renderer)
            renderer->resize(size);
    }

    void toast(const sf::String& text)
    {
        ui.message = text;
        messageTimer = 1.8f;
    }

    bool load(const fs::path& path)
    {
        if (!platform::isAudioFile(path))
        {
            toast("That isn't a supported audio file");
            return false;
        }

        auto next = std::make_unique<sf::SoundBuffer>();
        if (!next->loadFromFile(path))
        {
            toast("Couldn't open that file");
            return false;
        }

        sound.reset(); // detach from the old buffer before replacing it
        buffer = std::move(next);
        sound.emplace(*buffer);
        sound->setVolume(volume);
        sound->setLooping(looping);
        sound->play();

        currentPath = path;
        wantPlaying = true;
        const std::string name = path.stem().u8string();
        ui.title = sf::String::fromUtf8(name.begin(), name.end());
        ui.loaded = true;
        window.setTitle(ui.title + " - Bxsim");
        return true;
    }

    // Draw one "Loading..." frame, since decoding a long song takes a moment.
    void loadWithFeedback(const fs::path& path, Spectrum& spectrum, float time)
    {
        const bool wasLoaded = ui.loaded;
        ui.loading = true;
        ui.loaded = false;
        window.clear();
        renderer->draw(window, spectrum, Mode::Bars, time, 0.f, ui);
        window.display();
        ui.loading = false;
        if (!load(path))
            ui.loaded = wasLoaded;
    }

    void togglePlay()
    {
        if (!sound)
            return;
        if (sound->getStatus() == sf::SoundSource::Status::Playing)
        {
            sound->pause();
            wantPlaying = false;
        }
        else
        {
            sound->play();
            wantPlaying = true;
        }
    }

    void seekTo(float seconds)
    {
        if (!sound || !buffer)
            return;
        const float duration = buffer->getDuration().asSeconds();
        sound->setPlayingOffset(sf::seconds(std::clamp(seconds, 0.f, std::max(0.f, duration - 0.05f))));
    }

    void setVolume(float v)
    {
        volume = std::clamp(v, 0.f, 100.f);
        if (sound)
            sound->setVolume(volume);
        toast("Volume " + std::to_string(static_cast<int>(std::round(volume))) + "%");
    }
};
} // namespace

int main(int argc, char** argv)
{
    App app;
    app.createWindow();

    Renderer renderer;
    app.renderer = &renderer;
    renderer.resize(app.window.getSize());

    sf::Font font;
    for (const auto& candidate : platform::fontCandidates())
        if (font.openFromFile(candidate))
        {
            renderer.setFont(&font);
            break;
        }

    Spectrum spectrum(96);
    Mode mode = Mode::Bars;

    sf::Clock clock;
    float time = 0.f;
    float lastActivity = 0.f;
    bool seeking = false;

    if (const auto file = platform::commandLineFile(argc, argv))
        app.loadWithFeedback(*file, spectrum, time);

    while (app.window.isOpen())
    {
        std::optional<fs::path> toLoad;

        while (const std::optional event = app.window.pollEvent())
        {
            if (event->is<sf::Event::Closed>())
            {
                app.window.close();
            }
            else if (const auto* resized = event->getIf<sf::Event::Resized>())
            {
                app.onResize(resized->size);
            }
            else if (const auto* key = event->getIf<sf::Event::KeyPressed>())
            {
                lastActivity = time;
                using K = sf::Keyboard::Key;
                const float position = app.sound ? app.sound->getPlayingOffset().asSeconds() : 0.f;
                switch (key->code)
                {
                    case K::Space: app.togglePlay(); break;
                    case K::Left: app.seekTo(position - (key->shift ? 15.f : 5.f)); break;
                    case K::Right: app.seekTo(position + (key->shift ? 15.f : 5.f)); break;
                    case K::Up: app.setVolume(app.volume + 5.f); break;
                    case K::Down: app.setVolume(app.volume - 5.f); break;
                    case K::M:
                    {
                        const int count = static_cast<int>(Mode::Count);
                        const int step = key->shift ? count - 1 : 1;
                        mode = static_cast<Mode>((static_cast<int>(mode) + step) % count);
                        app.toast(sf::String(modeName(mode)));
                        break;
                    }
                    case K::L:
                        app.looping = !app.looping;
                        if (app.sound)
                            app.sound->setLooping(app.looping);
                        app.toast(app.looping ? "Loop on" : "Loop off");
                        break;
                    case K::N:
                    case K::P:
                        if (!app.currentPath.empty())
                            if (auto other = platform::siblingAudioFile(app.currentPath, key->code == K::N ? 1 : -1))
                                toLoad = *other;
                        break;
                    case K::O:
                        if (auto picked = platform::openFileDialog(app.window.getNativeHandle()))
                            toLoad = *picked;
                        clock.restart(); // the dialog blocks; don't count that as one huge frame
                        break;
                    case K::F11:
                    case K::F:
                        app.fullscreen = !app.fullscreen;
                        app.createWindow();
                        break;
                    case K::Escape:
                        if (app.fullscreen)
                        {
                            app.fullscreen = false;
                            app.createWindow();
                        }
                        else
                        {
                            app.window.close();
                        }
                        break;
                    default: break;
                }
            }
            else if (const auto* wheel = event->getIf<sf::Event::MouseWheelScrolled>())
            {
                lastActivity = time;
                app.setVolume(app.volume + wheel->delta * 5.f);
            }
            else if (const auto* press = event->getIf<sf::Event::MouseButtonPressed>())
            {
                lastActivity = time;
                const sf::Vector2f p(press->position);
                if (press->button == sf::Mouse::Button::Left && app.ui.loaded &&
                    renderer.seekBarHitBox().contains(p) && app.buffer)
                {
                    seeking = true;
                    app.seekTo(renderer.seekFractionAt(p.x) * app.buffer->getDuration().asSeconds());
                }
                else if (press->button == sf::Mouse::Button::Left && !app.ui.loaded)
                {
                    if (auto picked = platform::openFileDialog(app.window.getNativeHandle()))
                        toLoad = *picked;
                    clock.restart();
                }
            }
            else if (event->is<sf::Event::MouseButtonReleased>())
            {
                seeking = false;
            }
            else if (const auto* move = event->getIf<sf::Event::MouseMoved>())
            {
                lastActivity = time;
                if (seeking && app.buffer)
                    app.seekTo(renderer.seekFractionAt(static_cast<float>(move->position.x)) *
                               app.buffer->getDuration().asSeconds());
            }
        }

        if (!app.window.isOpen())
            break;

        if (auto dropped = platform::takeDroppedFile())
            toLoad = *dropped;
        if (toLoad)
            app.loadWithFeedback(*toLoad, spectrum, time);

        const float dt = clock.restart().asSeconds();
        time += dt;

        // When a song finishes, move on to the next one in its folder.
        bool playing = false;
        if (app.sound)
        {
            const auto status = app.sound->getStatus();
            playing = status == sf::SoundSource::Status::Playing;
            if (status == sf::SoundSource::Status::Stopped && app.wantPlaying)
            {
                app.wantPlaying = false;
                auto next = platform::siblingAudioFile(app.currentPath, 1);
                std::error_code ec;
                if (next && !fs::equivalent(*next, app.currentPath, ec))
                    app.loadWithFeedback(*next, spectrum, time);
                else
                    app.sound->setPlayingOffset(sf::Time::Zero);
            }
        }

        // Analyze the audio right at the playhead.
        if (app.sound && app.buffer)
        {
            const auto& buf = *app.buffer;
            const unsigned channels = buf.getChannelCount();
            const unsigned rate = buf.getSampleRate();
            const std::uint64_t frames = channels ? buf.getSampleCount() / channels : 0;
            const auto frame = static_cast<std::uint64_t>(app.sound->getPlayingOffset().asSeconds() * static_cast<float>(rate));
            spectrum.update(buf.getSamples(), frames, channels, rate, frame, dt, playing);

            app.ui.position = app.sound->getPlayingOffset().asSeconds();
            app.ui.duration = buf.getDuration().asSeconds();
        }
        else
        {
            spectrum.update(nullptr, 0, 0, 0, 0, dt, false);
        }
        app.ui.playing = playing;

        // Overlay fades away while you're just watching, comes back on any input or pause.
        const bool showUi = !playing || (time - lastActivity) < 2.5f || seeking;
        const float uiTarget = showUi ? 1.f : 0.f;
        app.ui.uiAlpha += (uiTarget - app.ui.uiAlpha) * (1.f - std::exp(-(showUi ? 12.f : 3.f) * dt));
        app.window.setMouseCursorVisible(app.ui.uiAlpha > 0.05f || !app.ui.loaded);

        app.messageTimer = std::max(0.f, app.messageTimer - dt);
        app.ui.messageAlpha = std::clamp(app.messageTimer / 0.4f, 0.f, 1.f);

        app.window.clear();
        renderer.draw(app.window, spectrum, mode, time, dt, app.ui);
        app.window.display();
    }
    return 0;
}
