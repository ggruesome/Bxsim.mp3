// Bxsim — a real-time audio visualizer.
//
// Open a song (drop it on the window, drag it onto Bxsim.exe, or press O) and it
// plays while the spectrum, waveform and beat drive the visuals.

#include "Platform.hpp"
#include "Renderer.hpp"
#include "Settings.hpp"
#include "SettingsPanel.hpp"
#include "Spectrum.hpp"
#include "VideoWriter.hpp"

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

// Canvas shapes for content creation. "Fit window" uses the whole window; the others are
// letterboxed inside it so you can screen-record exactly that shape.
struct Aspect
{
    const char* name;
    unsigned w;
    unsigned h;
};
constexpr Aspect Aspects[] = {
    {"Fit window", 0, 0},
    {"16:9 (YouTube)", 16, 9},
    {"9:16 (TikTok, Shorts, Reels)", 9, 16},
    {"1:1 (square)", 1, 1},
    {"4:5 (Instagram post)", 4, 5},
};
constexpr int AspectCount = static_cast<int>(sizeof(Aspects) / sizeof(Aspects[0]));

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
    bool fullscreen = false; // starts as a normal window, maximized to fill the screen
    sf::Image icon = makeIcon();

    Renderer* renderer = nullptr;

    // Everything is drawn into this canvas, then placed (letterboxed if needed) in the window.
    sf::RenderTexture canvas;
    sf::FloatRect canvasRect;
    int aspect = 0;
    const sf::Font* font = nullptr;

    Settings settings;
    SettingsPanel panel;
    fs::path settingsPath = platform::settingsFile();

    void applyReactivity(Spectrum& s) const
    {
        s.setSensitivity(settings.get("global.sensitivity"));
        s.setSmoothing(settings.get("global.smoothing"));
        s.setKick(settings.get("global.kick"));
    }

    void saveSettings() const { settings.save(settingsPath); }

    // ---- Video export: renders every frame offline at full resolution, then encodes an MP4 ----
    struct Export
    {
        std::unique_ptr<VideoWriter> writer;
        sf::RenderTexture target;
        Spectrum spectrum{96};
        std::uint64_t frame = 0;
        std::uint64_t total = 0;
        unsigned fps = 60;
        float startTime = 0.f;
        Mode mode = Mode::Bars;
        fs::path path;
    };
    std::unique_ptr<Export> exporting;

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
            platform::maximizeWindow(window.getNativeHandle());
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
        layoutCanvas();
    }

    // Fit the chosen aspect ratio inside the window, centered, as large as possible.
    void layoutCanvas()
    {
        const sf::Vector2u win = window.getSize();
        sf::Vector2u size = win;
        const Aspect& a = Aspects[aspect];
        if (a.w && a.h)
        {
            if (static_cast<unsigned long long>(win.x) * a.h > static_cast<unsigned long long>(win.y) * a.w)
                size = {win.y * a.w / a.h, win.y};
            else
                size = {win.x, win.x * a.h / a.w};
        }
        size.x = std::max(size.x, 64u);
        size.y = std::max(size.y, 64u);
        canvasRect = {{std::round((static_cast<float>(win.x) - static_cast<float>(size.x)) * 0.5f),
                       std::round((static_cast<float>(win.y) - static_cast<float>(size.y)) * 0.5f)},
                      sf::Vector2f(size)};
        if (canvas.getSize() != size)
        {
            if (!canvas.resize(size))
                return;
            canvas.setSmooth(true);
            if (renderer && !exporting) // during an export the renderer is busy at video size
                renderer->resize(size);
        }
    }

    void cycleAspect(int step)
    {
        aspect = (aspect + step + AspectCount) % AspectCount;
        layoutCanvas();
        toast(sf::String("Aspect: ") + Aspects[aspect].name);
    }

    // Window coordinates -> canvas coordinates (for clicks on the seek bar).
    [[nodiscard]] sf::Vector2f toCanvas(sf::Vector2f p) const { return p - canvasRect.position; }

    void present(const Spectrum& spectrum, Mode mode, float time, float dt)
    {
        canvas.clear();
        renderer->draw(canvas, spectrum, mode, time, dt, ui);
        canvas.display();
        window.clear(sf::Color::Black);
        sf::Sprite frame(canvas.getTexture());
        frame.setPosition(canvasRect.position);
        window.draw(frame);
        panel.draw(window, font, settings, static_cast<int>(mode), modeName(mode)); // never part of exports
        window.display();
    }

    // Video size for the current canvas shape (16:9 is used for "Fit window").
    [[nodiscard]] sf::Vector2u exportSize() const
    {
        switch (aspect)
        {
            case 2: return {1080, 1920};
            case 3: return {1080, 1080};
            case 4: return {1080, 1350};
            default: return {1920, 1080};
        }
    }

    void startExport(Mode mode, float time)
    {
        if (!buffer || !sound)
        {
            toast("Open a song first, then press E to export it");
            return;
        }
        const std::string stem = currentPath.stem().u8string();
        const auto path = platform::saveVideoDialog(window.getNativeHandle(), fs::u8path(stem + " - Bxsim.mp4"));
        if (!path)
            return;

        sound->pause();
        wantPlaying = false;

        auto job = std::make_unique<Export>();
        const sf::Vector2u size = exportSize();
        if (!job->target.resize(size))
        {
            toast("Couldn't create a canvas that big on this graphics card");
            return;
        }
        const unsigned channels = buffer->getChannelCount();
        const unsigned rate = buffer->getSampleRate();
        const std::uint64_t frames = channels ? buffer->getSampleCount() / channels : 0;
        std::string error;
        job->writer = std::make_unique<VideoWriter>();
        if (!job->writer->open(*path, size.x, size.y, job->fps, buffer->getSamples(), frames, channels, rate, error))
        {
            toast("Export failed: " + error);
            return;
        }
        job->total = static_cast<std::uint64_t>(std::ceil(buffer->getDuration().asSeconds() * static_cast<float>(job->fps)));
        job->startTime = time;
        job->mode = mode;
        job->path = *path;
        exporting = std::move(job);
        renderer->resize(size);
        renderer->restartImageEffect();
    }

    // Render and encode frames for a short slice of time, so the window stays responsive.
    void stepExport()
    {
        Export& job = *exporting;
        const auto& buf = *buffer;
        const unsigned channels = buf.getChannelCount();
        const unsigned rate = buf.getSampleRate();
        const std::uint64_t frames = channels ? buf.getSampleCount() / channels : 0;
        const float dt = 1.f / static_cast<float>(job.fps);

        UiState clean; // no text or controls in the video (the car radio still shows the song)
        clean.loaded = true;
        clean.playing = true;
        clean.uiAlpha = 0.f;
        clean.title = ui.title;
        clean.duration = buf.getDuration().asSeconds();

        sf::Clock budget;
        while (job.frame < job.total && budget.getElapsedTime() < sf::milliseconds(40))
        {
            const double seconds = static_cast<double>(job.frame) / job.fps;
            clean.position = static_cast<float>(seconds);
            const auto position = static_cast<std::uint64_t>(seconds * rate);
            applyReactivity(job.spectrum);
            job.spectrum.update(buf.getSamples(), frames, channels, rate, position, dt, true);
            job.target.clear();
            renderer->draw(job.target, job.spectrum, job.mode, job.startTime + static_cast<float>(seconds), dt, clean);
            job.target.display();
            const sf::Image image = job.target.getTexture().copyToImage();
            if (!job.writer->addFrame(image.getPixelsPtr()))
            {
                finishExport(false);
                return;
            }
            ++job.frame;
        }
        if (job.frame >= job.total)
            finishExport(true);
    }

    void finishExport(bool ok)
    {
        ok = exporting->writer->finish() && ok;
        const std::string name = exporting->path.filename().u8string();
        exporting.reset();
        renderer->resize(canvas.getSize());
        window.setTitle(ui.title + " - Bxsim");
        if (ok)
            toast("Saved " + name);
        else
            toast("Export failed while encoding the video");
    }

    void cancelExport()
    {
        exporting->writer->cancel();
        exporting.reset();
        renderer->resize(canvas.getSize());
        window.setTitle(ui.title + " - Bxsim");
        toast("Export canceled");
    }

    // Progress screen: a live preview of the frames being rendered, plus a progress bar.
    void presentExport()
    {
        const Export& job = *exporting;
        const sf::Vector2f win(window.getSize());
        const sf::Vector2f tex(job.target.getSize());
        const float fraction = job.total ? static_cast<float>(job.frame) / static_cast<float>(job.total) : 0.f;
        const int percent = static_cast<int>(fraction * 100.f);

        window.clear(sf::Color(8, 8, 14));
        const float scale = std::max(0.01f, std::min((win.x - 80.f) / tex.x, (win.y - 170.f) / tex.y));
        sf::Sprite preview(job.target.getTexture());
        preview.setScale({scale, scale});
        preview.setPosition({std::round((win.x - tex.x * scale) * 0.5f), 40.f});
        window.draw(preview);

        const float barWidth = std::min(win.x * 0.6f, 700.f);
        const sf::Vector2f barPos{std::round((win.x - barWidth) * 0.5f), win.y - 90.f};
        sf::RectangleShape track({barWidth, 6.f});
        track.setPosition(barPos);
        track.setFillColor(sf::Color(255, 255, 255, 40));
        window.draw(track);
        sf::RectangleShape fill({barWidth * fraction, 6.f});
        fill.setPosition(barPos);
        fill.setFillColor(sf::Color(190, 120, 255));
        window.draw(fill);

        if (font)
        {
            const sf::Vector2u size = job.target.getSize();
            sf::Text text(*font,
                          "Exporting " + std::to_string(size.x) + " x " + std::to_string(size.y) + " video   " +
                              std::to_string(percent) + "%",
                          18);
            text.setPosition({std::round((win.x - text.getLocalBounds().size.x) * 0.5f), win.y - 70.f});
            window.draw(text);
            sf::Text hint(*font, "Esc cancels", 14);
            hint.setFillColor(sf::Color(160, 165, 190));
            hint.setPosition({std::round((win.x - hint.getLocalBounds().size.x) * 0.5f), win.y - 40.f});
            window.draw(hint);
        }
        window.display();
        window.setTitle("Exporting " + std::to_string(percent) + "% - Bxsim");
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

    bool loadImage(const fs::path& path)
    {
        if (renderer->setImage(path))
        {
            toast(U"Image added  \u00B7  Delete removes it");
            return true;
        }
        toast("Couldn't open that image");
        return false;
    }

    // Draw one "Loading..." frame, since decoding a long song takes a moment.
    void loadWithFeedback(const fs::path& path, Spectrum& spectrum, float time)
    {
        const bool wasLoaded = ui.loaded;
        ui.loading = true;
        ui.loaded = false;
        present(spectrum, Mode::Bars, time, 0.f);
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

    sf::Font font;
    for (const auto& candidate : platform::fontCandidates())
        if (font.openFromFile(candidate))
        {
            app.font = &font;
            break;
        }

    // Loading screen while the shaders compile.
    sf::Clock loadingClock;
    auto drawLoading = [&](float fraction, const char* what)
    {
        while (const std::optional event = app.window.pollEvent())
            if (const auto* resized = event->getIf<sf::Event::Resized>())
                app.window.setView(sf::View(sf::FloatRect({0.f, 0.f}, sf::Vector2f(resized->size))));
        const sf::Vector2f win(app.window.getSize());
        const float t = loadingClock.getElapsedTime().asSeconds();
        app.window.clear(sf::Color(8, 7, 16));

        // Five little bars bouncing, like the app icon.
        const float barW = 14.f, gap = 8.f;
        const float startX = std::round(win.x * 0.5f - (barW * 5.f + gap * 4.f) * 0.5f);
        for (int i = 0; i < 5; ++i)
        {
            const float h = 18.f + 34.f * (0.5f + 0.5f * std::sin(t * 6.f + static_cast<float>(i) * 0.9f));
            sf::RectangleShape bar({barW, h});
            bar.setPosition({startX + static_cast<float>(i) * (barW + gap), std::round(win.y * 0.5f - 60.f - h)});
            bar.setFillColor(sf::Color(static_cast<std::uint8_t>(150 + 20 * i), static_cast<std::uint8_t>(110 + 10 * i), 255));
            app.window.draw(bar);
        }

        const float barWidth = std::min(win.x * 0.5f, 420.f);
        const sf::Vector2f barPos{std::round((win.x - barWidth) * 0.5f), std::round(win.y * 0.5f + 34.f)};
        sf::RectangleShape track({barWidth, 4.f});
        track.setPosition(barPos);
        track.setFillColor(sf::Color(255, 255, 255, 35));
        app.window.draw(track);
        sf::RectangleShape fill({barWidth * fraction, 4.f});
        fill.setPosition(barPos);
        fill.setFillColor(sf::Color(190, 120, 255));
        app.window.draw(fill);

        if (app.font)
        {
            sf::Text title(*app.font, "Bxsim", 54);
            title.setPosition({std::round((win.x - title.getLocalBounds().size.x) * 0.5f), std::round(win.y * 0.5f - 46.f)});
            app.window.draw(title);
            const std::string label = (fraction >= 1.f ? std::string("Starting") : std::string("Loading ") + what) +
                                      "\xE2\x80\xA6"; // "…" in UTF-8
            sf::Text status(*app.font, sf::String::fromUtf8(label.begin(), label.end()), 15);
            status.setFillColor(sf::Color(150, 155, 185));
            status.setPosition({std::round((win.x - status.getLocalBounds().size.x) * 0.5f), barPos.y + 16.f});
            app.window.draw(status);
        }
        app.window.display();
    };
    drawLoading(0.f, "");

    Renderer renderer;
    app.renderer = &renderer;
    renderer.setFont(app.font);
    renderer.compileShaders(drawLoading);
    app.layoutCanvas();
    renderer.resize(app.canvas.getSize());

    Spectrum spectrum(96);
    Mode mode = Mode::Bars;
    app.settings.load(app.settingsPath);
    renderer.setSettings(&app.settings);

    sf::Clock clock;
    float time = 0.f;
    float lastActivity = 0.f;
    bool seeking = false;
    float lastClickTime = -10.f;
    sf::Vector2f lastClickPosition;

    // Songs and images can arrive together: dragged onto the exe, or dropped on the window.
    auto openFiles = [&](const std::vector<fs::path>& files)
    {
        bool songLoaded = false;
        for (const auto& file : files)
        {
            if (platform::isImageFile(file))
                app.loadImage(file);
            else if (!songLoaded && platform::isAudioFile(file))
            {
                app.loadWithFeedback(file, spectrum, time);
                songLoaded = true;
            }
            else if (!platform::isAudioFile(file))
                app.toast("That file type isn't supported");
        }
    };
    openFiles(platform::commandLineFiles(argc, argv));
    bool showFps = false;
    bool settingsDirty = false;
    float fps = 60.f;

    while (app.window.isOpen())
    {
        if (app.exporting)
        {
            while (const std::optional event = app.window.pollEvent())
            {
                if (event->is<sf::Event::Closed>())
                {
                    app.cancelExport();
                    app.window.close();
                }
                else if (const auto* resized = event->getIf<sf::Event::Resized>())
                    app.onResize(resized->size);
                else if (const auto* key = event->getIf<sf::Event::KeyPressed>();
                         key && key->code == sf::Keyboard::Key::Escape && app.exporting)
                    app.cancelExport();
            }
            if (!app.window.isOpen())
                break;
            if (app.exporting)
                app.stepExport();
            if (app.exporting)
                app.presentExport();
            clock.restart();
            continue;
        }

        std::optional<fs::path> toLoad;

        while (const std::optional event = app.window.pollEvent())
        {
            if (app.panel.handleEvent(*event, app.settings))
            {
                lastActivity = time;
                continue; // the settings panel used this click/drag/scroll
            }
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
                    case K::Hyphen:
                        mode = Mode::DarkForest;
                        app.toast(sf::String(modeName(mode)));
                        break;
                    case K::Num0:
                        mode = Mode::NightDrive;
                        app.toast(sf::String(modeName(mode)));
                        break;
                    case K::Num1:
                    case K::Num2:
                    case K::Num3:
                    case K::Num4:
                    case K::Num5:
                    case K::Num6:
                    case K::Num7:
                    case K::Num8:
                    case K::Num9:
                        mode = static_cast<Mode>(static_cast<int>(key->code) - static_cast<int>(K::Num1));
                        app.toast(sf::String(modeName(mode)));
                        break;
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
                        if (auto picked = platform::openFileDialog(app.window.getNativeHandle(), platform::FileKind::Audio))
                            toLoad = *picked;
                        clock.restart(); // the dialog blocks; don't count that as one huge frame
                        break;
                    case K::I:
                        // I adds a picture, or takes the current one away to get the plain effects back.
                        if (renderer.hasImage())
                        {
                            renderer.clearImage();
                            app.toast(U"Image removed  \u00B7  I to add one");
                        }
                        else
                        {
                            if (auto picked = platform::openFileDialog(app.window.getNativeHandle(), platform::FileKind::Image))
                                app.loadImage(*picked);
                            clock.restart();
                        }
                        break;
                    case K::Delete:
                    case K::Backspace:
                        if (renderer.hasImage())
                        {
                            renderer.clearImage();
                            app.toast("Image removed");
                        }
                        break;
                    case K::Tab:
                        if (mode != Mode::Oscilloscope)
                        {
                            mode = Mode::Oscilloscope;
                            app.toast("Oscilloscope");
                        }
                        else
                        {
                            app.toast(renderer.toggleOscilloscopeSweep() ? "Oscilloscope: sweep" : "Oscilloscope: XY");
                        }
                        break;
                    case K::A:
                        app.cycleAspect(key->shift ? -1 : 1);
                        break;
                    case K::G:
                        renderer.setFilter(renderer.filter() + (key->shift ? -1 : 1));
                        app.toast(sf::String("Filter: ") + Renderer::filterName(renderer.filter()));
                        break;
                    case K::S:
                        app.panel.toggle();
                        if (!app.panel.visible() && settingsDirty)
                        {
                            app.saveSettings();
                            settingsDirty = false;
                        }
                        break;
                    case K::E:
                        app.startExport(mode, time);
                        clock.restart();
                        break;
                    case K::F3:
                        showFps = !showFps;
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
                const sf::Vector2f p = app.toCanvas(sf::Vector2f(press->position));
                if (press->button == sf::Mouse::Button::Left && app.ui.loaded &&
                    renderer.seekBarHitBox().contains(p) && app.buffer)
                {
                    seeking = true;
                    app.seekTo(renderer.seekFractionAt(p.x) * app.buffer->getDuration().asSeconds());
                }
                else if (press->button == sf::Mouse::Button::Left && !app.ui.loaded)
                {
                    if (auto picked = platform::openFileDialog(app.window.getNativeHandle(), platform::FileKind::Audio))
                        toLoad = *picked;
                    clock.restart();
                }
                else if (press->button == sf::Mouse::Button::Left)
                {
                    // Double-click anywhere (except the seek bar) toggles fullscreen.
                    const sf::Vector2f moved = p - lastClickPosition;
                    if (time - lastClickTime < 0.35f && moved.x * moved.x + moved.y * moved.y < 100.f)
                    {
                        app.fullscreen = !app.fullscreen;
                        app.createWindow();
                        lastClickTime = -10.f;
                    }
                    else
                    {
                        lastClickTime = time;
                        lastClickPosition = p;
                    }
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
                    app.seekTo(renderer.seekFractionAt(app.toCanvas(sf::Vector2f(move->position)).x) *
                               app.buffer->getDuration().asSeconds());
            }
        }

        if (!app.window.isOpen())
            break;

        if (const auto dropped = platform::takeDroppedFiles(); !dropped.empty())
            openFiles(dropped);
        if (toLoad)
            app.loadWithFeedback(*toLoad, spectrum, time);

        const float dt = clock.restart().asSeconds();
        time += dt;
        if (dt > 0.f)
            fps += (1.f / dt - fps) * 0.05f;
        app.ui.showFps = showFps;
        app.ui.fps = fps;

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
            app.applyReactivity(spectrum);
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
        app.window.setMouseCursorVisible(app.ui.uiAlpha > 0.05f || !app.ui.loaded || app.panel.visible());
        if (app.panel.takeChanged())
            settingsDirty = true;

        app.messageTimer = std::max(0.f, app.messageTimer - dt);
        app.ui.messageAlpha = std::clamp(app.messageTimer / 0.4f, 0.f, 1.f);

        app.present(spectrum, mode, time, dt);
    }
    if (settingsDirty)
        app.saveSettings();
    return 0;
}
