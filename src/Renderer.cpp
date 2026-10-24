#include "Renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
constexpr float Pi = 3.14159265358979f;

// Gaussian blur, 9 taps folded into 5 with linear filtering.
constexpr const char* BlurShader = R"(
uniform sampler2D source;
uniform vec2 direction;
void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec4 c = texture2D(source, uv) * 0.2270270270;
    c += texture2D(source, uv + direction * 1.3846153846) * 0.3162162162;
    c += texture2D(source, uv - direction * 1.3846153846) * 0.3162162162;
    c += texture2D(source, uv + direction * 3.2307692308) * 0.0702702703;
    c += texture2D(source, uv - direction * 3.2307692308) * 0.0702702703;
    gl_FragColor = c * gl_Color;
}
)";

sf::Color hsv(float h, float s, float v, float a = 1.f)
{
    h = h - std::floor(h);
    s = std::clamp(s, 0.f, 1.f);
    v = std::clamp(v, 0.f, 1.f);
    const float i = std::floor(h * 6.f);
    const float f = h * 6.f - i;
    const float p = v * (1.f - s);
    const float q = v * (1.f - f * s);
    const float t = v * (1.f - (1.f - f) * s);
    float r = 0.f, g = 0.f, b = 0.f;
    switch (static_cast<int>(i) % 6)
    {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
    }
    auto to8 = [](float x) { return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
    return {to8(r), to8(g), to8(b), to8(a)};
}

sf::Color scaled(sf::Color c, float k)
{
    auto s = [k](std::uint8_t x) { return static_cast<std::uint8_t>(std::clamp(static_cast<float>(x) * k, 0.f, 255.f)); };
    return {s(c.r), s(c.g), s(c.b), c.a};
}

sf::Color withAlpha(sf::Color c, float alpha)
{
    c.a = static_cast<std::uint8_t>(std::clamp(alpha, 0.f, 1.f) * 255.f);
    return c;
}

void quad(sf::VertexArray& out,
          sf::Vector2f a, sf::Vector2f b, sf::Vector2f c, sf::Vector2f d,
          sf::Color ca, sf::Color cb, sf::Color cc, sf::Color cd)
{
    out.append({a, ca});
    out.append({b, cb});
    out.append({c, cc});
    out.append({a, ca});
    out.append({c, cc});
    out.append({d, cd});
}

void rect(sf::VertexArray& out, float x0, float y0, float x1, float y1, sf::Color top, sf::Color bottom)
{
    quad(out, {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, top, top, bottom, bottom);
}

// A line with real thickness (SFML's own lines are always 1px).
void thickLine(sf::VertexArray& out, const std::vector<sf::Vector2f>& pts, const std::vector<sf::Color>& colors,
               float thickness, bool closed)
{
    const std::size_t n = pts.size();
    if (n < 2)
        return;

    auto normalAt = [&](std::size_t i)
    {
        const sf::Vector2f prev = (i == 0) ? (closed ? pts[n - 1] : pts[0]) : pts[i - 1];
        const sf::Vector2f next = (i == n - 1) ? (closed ? pts[0] : pts[n - 1]) : pts[i + 1];
        sf::Vector2f d = next - prev;
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1e-4f)
            return sf::Vector2f{0.f, 1.f};
        d /= len;
        return sf::Vector2f{-d.y, d.x};
    };

    const float half = thickness * 0.5f;
    const std::size_t segments = closed ? n : n - 1;
    for (std::size_t s = 0; s < segments; ++s)
    {
        const std::size_t i = s;
        const std::size_t j = (s + 1) % n;
        const sf::Vector2f ni = normalAt(i) * half;
        const sf::Vector2f nj = normalAt(j) * half;
        quad(out, pts[i] + ni, pts[j] + nj, pts[j] - nj, pts[i] - ni, colors[i], colors[j], colors[j], colors[i]);
    }
}

std::string formatTime(float seconds)
{
    const int total = std::max(0, static_cast<int>(seconds));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}
} // namespace

const char* modeName(Mode mode)
{
    switch (mode)
    {
        case Mode::Bars: return "Bars";
        case Mode::Ring: return "Ring";
        case Mode::Scope: return "Scope";
        default: return "";
    }
}

Renderer::Renderer()
{
    m_bloomReady = sf::Shader::isAvailable() && m_blur.loadFromMemory(BlurShader, sf::Shader::Type::Fragment);
    m_particles.reserve(800);
}

void Renderer::resize(sf::Vector2u size)
{
    size.x = std::max(size.x, 1u);
    size.y = std::max(size.y, 1u);
    m_size = {static_cast<float>(size.x), static_cast<float>(size.y)};

    const sf::Vector2u half{std::max(size.x / 2, 1u), std::max(size.y / 2, 1u)};
    sf::ContextSettings smoothEdges;
    smoothEdges.antiAliasingLevel = 4;
    const bool ok = m_scene.resize(size, smoothEdges) && m_blurA.resize(half) && m_blurB.resize(half);
    if (!ok)
        m_bloomReady = false;
    m_scene.setSmooth(true);
    m_blurA.setSmooth(true);
    m_blurB.setSmooth(true);
}

sf::Color Renderer::bandColor(float t, float brightness, float saturation) const
{
    return hsv(m_hue + t * 0.45f, saturation, brightness);
}

void Renderer::draw(sf::RenderWindow& window, const Spectrum& spectrum, Mode mode, float time, float dt, const UiState& ui)
{
    m_time = time;
    m_hue = 0.62f + time * 0.012f; // a slow drift through the color wheel
    m_ringSpin = 0.12f * std::sin(time * 0.35f); // gentle sway, keeps the ring symmetric

    // 1) The glowing part goes into an offscreen texture, drawn additively.
    sf::VertexArray verts(sf::PrimitiveType::Triangles);
    switch (mode)
    {
        case Mode::Bars: drawBars(verts, spectrum); break;
        case Mode::Ring: drawRing(verts, spectrum); break;
        case Mode::Scope: drawScope(verts, spectrum); break;
        default: break;
    }
    updateParticles(verts, spectrum, mode, dt);

    m_scene.clear(sf::Color::Black);
    m_scene.draw(verts, sf::RenderStates(sf::BlendAdd));
    m_scene.display();

    // 2) Compose: background, the scene, then its bloom on top.
    drawBackground(window, spectrum);
    sf::Sprite sceneSprite(m_scene.getTexture());
    window.draw(sceneSprite, sf::RenderStates(sf::BlendAdd));
    applyBloom(window, 0.85f + spectrum.beat() * 0.9f);

    // 3) Text and controls.
    drawOverlay(window, ui, mode);
}

void Renderer::drawBackground(sf::RenderTarget& target, const Spectrum& spectrum)
{
    const float pulse = spectrum.beat() * 0.05f + spectrum.energy() * 0.04f;
    const sf::Color top = hsv(m_hue + 0.05f, 0.65f, 0.09f + pulse);
    const sf::Color bottom = hsv(m_hue - 0.05f, 0.75f, 0.02f);
    sf::VertexArray bg(sf::PrimitiveType::Triangles);
    rect(bg, 0.f, 0.f, m_size.x, m_size.y, top, bottom);
    target.draw(bg);
}

void Renderer::drawBars(sf::VertexArray& out, const Spectrum& spectrum)
{
    const auto& bands = spectrum.bands();
    const auto& peaks = spectrum.peaks();
    const std::size_t n = bands.size();

    const float left = m_size.x * 0.06f;
    const float right = m_size.x * 0.94f;
    const float floorY = m_size.y * 0.66f;
    const float maxHeight = m_size.y * 0.52f;
    const float slot = (right - left) / static_cast<float>(n);
    const float barWidth = std::max(1.f, slot * 0.72f);

    for (std::size_t i = 0; i < n; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float v = bands[i];
        const float x0 = left + static_cast<float>(i) * slot + (slot - barWidth) * 0.5f;
        const float x1 = x0 + barWidth;
        const float top = floorY - std::max(2.f, v * maxHeight);

        const sf::Color cTop = bandColor(t, 0.55f + 0.45f * v, 0.8f);
        const sf::Color cBottom = bandColor(t, 0.16f, 0.95f);
        rect(out, x0, top, x1, floorY, cTop, cBottom);

        // Reflection on the "floor".
        const float reflection = (floorY - top) * 0.42f;
        rect(out, x0, floorY + 3.f, x1, floorY + 3.f + reflection, scaled(cBottom, 1.4f), sf::Color::Black);

        // Peak cap.
        const float py = floorY - peaks[i] * maxHeight - 7.f;
        rect(out, x0, py, x1, py + 3.f, bandColor(t, 1.f, 0.35f), bandColor(t, 0.9f, 0.45f));
    }

    // Faint floor line.
    const sf::Color line = hsv(m_hue, 0.5f, 0.25f + spectrum.beat() * 0.3f);
    rect(out, left, floorY + 1.f, right, floorY + 2.f, line, line);
}

void Renderer::drawRing(sf::VertexArray& out, const Spectrum& spectrum)
{
    const auto& bands = spectrum.bands();
    const auto& peaks = spectrum.peaks();
    const std::size_t n = bands.size();

    const sf::Vector2f center = m_size * 0.5f;
    const float unit = std::min(m_size.x, m_size.y);
    const float base = unit * 0.17f * (1.f + 0.10f * spectrum.beat());
    const float maxLength = unit * 0.25f;
    const float halfWidth = (Pi * base / static_cast<float>(n)) * 0.42f;

    // Bass at the top, mirrored left and right so it's symmetric.
    for (int side : {-1, 1})
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float v = bands[i];
            const float angle = -Pi * 0.5f + static_cast<float>(side) * ((static_cast<float>(i) + 0.5f) / static_cast<float>(n)) * Pi + m_ringSpin;
            const sf::Vector2f dir{std::cos(angle), std::sin(angle)};
            const sf::Vector2f perp{-dir.y, dir.x};

            const float r0 = base;
            const float r1 = base + 4.f + v * maxLength;
            const sf::Color cIn = bandColor(t, 0.25f, 0.95f);
            const sf::Color cOut = bandColor(t, 0.6f + 0.4f * v, 0.8f);
            quad(out,
                 center + dir * r0 + perp * halfWidth,
                 center + dir * r1 + perp * (halfWidth * 1.2f),
                 center + dir * r1 - perp * (halfWidth * 1.2f),
                 center + dir * r0 - perp * halfWidth,
                 cIn, cOut, cOut, cIn);

            // Peak dot.
            const float rp = base + 10.f + peaks[i] * maxLength;
            const float s = halfWidth * 0.9f;
            const sf::Vector2f p = center + dir * rp;
            const sf::Color cp = bandColor(t, 1.f, 0.35f);
            quad(out, p + perp * s + dir * s, p - perp * s + dir * s, p - perp * s - dir * s, p + perp * s - dir * s, cp, cp, cp, cp);
        }
    }

    // The raw waveform wrapped into a circle inside the ring.
    const auto& wave = spectrum.waveform();
    constexpr std::size_t points = 200;
    std::vector<sf::Vector2f> pts(points);
    std::vector<sf::Color> colors(points);
    for (std::size_t k = 0; k < points; ++k)
    {
        const float a = static_cast<float>(k) / static_cast<float>(points) * 2.f * Pi - Pi * 0.5f;
        const float sample = std::clamp(wave[k * wave.size() / points] * 1.5f, -1.f, 1.f);
        const float r = base * 0.78f + sample * base * 0.3f;
        pts[k] = center + sf::Vector2f{std::cos(a), std::sin(a)} * r;
        colors[k] = bandColor(0.15f + 0.3f * std::abs(std::sin(a)), 0.9f, 0.55f);
    }
    thickLine(out, pts, colors, 2.5f, true);
}

void Renderer::drawScope(sf::VertexArray& out, const Spectrum& spectrum)
{
    // Spectrum silhouette along the bottom.
    const auto& bands = spectrum.bands();
    const std::size_t n = bands.size();
    const float baseY = m_size.y;
    const float height = m_size.y * 0.32f;
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        const float t0 = static_cast<float>(i) / static_cast<float>(n - 1);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(n - 1);
        const float x0 = t0 * m_size.x;
        const float x1 = t1 * m_size.x;
        const float y0 = baseY - bands[i] * height;
        const float y1 = baseY - bands[i + 1] * height;
        quad(out, {x0, y0}, {x1, y1}, {x1, baseY}, {x0, baseY},
             bandColor(t0, 0.45f, 0.85f), bandColor(t1, 0.45f, 0.85f), sf::Color::Black, sf::Color::Black);
    }

    // Oscilloscope line, with a dim echo behind it.
    const auto& wave = spectrum.waveform();
    const std::size_t points = std::min<std::size_t>(wave.size(), 512);
    const float left = m_size.x * 0.04f;
    const float width = m_size.x * 0.92f;
    const float midY = m_size.y * 0.42f;
    const float amp = m_size.y * 0.26f;

    std::vector<sf::Vector2f> pts(points), echo(points);
    std::vector<sf::Color> colors(points), echoColors(points);
    for (std::size_t k = 0; k < points; ++k)
    {
        const float t = static_cast<float>(k) / static_cast<float>(points - 1);
        const float sample = std::clamp(wave[k * wave.size() / points] * 1.6f, -1.f, 1.f);
        // Taper the ends so the line eases in and out instead of cutting off.
        const float taper = std::sin(t * Pi);
        pts[k] = {left + t * width, midY + sample * amp * taper};
        echo[k] = {left + t * width, midY - sample * amp * taper * 0.5f};
        colors[k] = bandColor(t, 0.95f, 0.6f);
        echoColors[k] = bandColor(t + 0.3f, 0.3f, 0.8f);
    }
    thickLine(out, echo, echoColors, 2.f, false);
    thickLine(out, pts, colors, 3.f, false);
}

void Renderer::updateParticles(sf::VertexArray& out, const Spectrum& spectrum, Mode mode, float dt)
{
    std::uniform_real_distribution<float> uni(0.f, 1.f);

    // Spawn more on loud parts and kicks.
    const float rate = 6.f + spectrum.energy() * 60.f + spectrum.beat() * 260.f;
    m_spawnAccumulator += rate * dt;
    while (m_spawnAccumulator >= 1.f && m_particles.size() < 400)
    {
        m_spawnAccumulator -= 1.f;
        Particle p;
        p.maxLife = p.life = 1.f + uni(m_rng) * 1.6f;
        p.size = 1.f + uni(m_rng) * 2.5f;
        p.hue = uni(m_rng) * 0.45f;
        const float boost = 1.f + spectrum.beat() * 2.f;

        if (mode == Mode::Ring)
        {
            const float a = uni(m_rng) * 2.f * Pi;
            const float r = std::min(m_size.x, m_size.y) * 0.17f;
            const sf::Vector2f dir{std::cos(a), std::sin(a)};
            p.position = m_size * 0.5f + dir * r;
            p.velocity = dir * ((40.f + uni(m_rng) * 120.f) * boost);
        }
        else
        {
            const float y = (mode == Mode::Bars) ? m_size.y * 0.66f : m_size.y;
            p.position = {m_size.x * (0.06f + uni(m_rng) * 0.88f), y};
            p.velocity = {(uni(m_rng) - 0.5f) * 30.f, -(30.f + uni(m_rng) * 110.f) * boost};
        }
        m_particles.push_back(p);
    }
    m_spawnAccumulator = std::min(m_spawnAccumulator, 1.f);

    const float drag = std::exp(-0.5f * dt);
    for (auto& p : m_particles)
    {
        p.position += p.velocity * dt;
        p.velocity *= drag;
        p.life -= dt;
    }
    m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
                                     [](const Particle& p) { return p.life <= 0.f; }),
                      m_particles.end());

    for (const auto& p : m_particles)
    {
        const float fade = std::sin(std::clamp(p.life / p.maxLife, 0.f, 1.f) * Pi); // fade in and out
        const sf::Color c = bandColor(p.hue, 0.7f * fade, 0.5f);
        const float s = p.size;
        rect(out, p.position.x - s, p.position.y - s, p.position.x + s, p.position.y + s, c, c);
    }
}

void Renderer::applyBloom(sf::RenderTarget& target, float strength)
{
    if (!m_bloomReady)
        return;

    const sf::Vector2f halfSize{static_cast<float>(m_blurA.getSize().x), static_cast<float>(m_blurA.getSize().y)};

    // Downsample the scene to half size.
    m_blurA.clear(sf::Color::Black);
    sf::Sprite down(m_scene.getTexture());
    down.setScale({halfSize.x / m_size.x, halfSize.y / m_size.y});
    m_blurA.draw(down);
    m_blurA.display();

    // Blur back and forth, widening each pass.
    m_blur.setUniform("source", sf::Shader::CurrentTexture);
    for (int pass = 0; pass < 3; ++pass)
    {
        const float spread = 1.f + static_cast<float>(pass) * 1.5f;

        m_blur.setUniform("direction", sf::Glsl::Vec2{spread / halfSize.x, 0.f});
        m_blurB.clear(sf::Color::Black);
        m_blurB.draw(sf::Sprite(m_blurA.getTexture()), sf::RenderStates(&m_blur));
        m_blurB.display();

        m_blur.setUniform("direction", sf::Glsl::Vec2{0.f, spread / halfSize.y});
        m_blurA.clear(sf::Color::Black);
        m_blurA.draw(sf::Sprite(m_blurB.getTexture()), sf::RenderStates(&m_blur));
        m_blurA.display();
    }

    sf::Sprite glow(m_blurA.getTexture());
    glow.setScale({m_size.x / halfSize.x, m_size.y / halfSize.y});
    glow.setColor(withAlpha(sf::Color::White, std::min(strength, 1.f)));
    sf::RenderStates states(sf::BlendAdd);
    target.draw(glow, states);
    if (strength > 1.f) // extra punch on big kicks
    {
        glow.setColor(withAlpha(sf::Color::White, strength - 1.f));
        target.draw(glow, states);
    }
}

sf::FloatRect Renderer::seekBarHitBox() const
{
    const float margin = 32.f;
    return {{margin, m_size.y - 66.f}, {m_size.x - margin * 2.f, 24.f}};
}

float Renderer::seekFractionAt(float x) const
{
    const sf::FloatRect box = seekBarHitBox();
    return std::clamp((x - box.position.x) / box.size.x, 0.f, 1.f);
}

void Renderer::drawOverlay(sf::RenderTarget& target, const UiState& ui, Mode mode)
{
    if (!m_font)
        return;

    auto makeText = [&](const sf::String& str, unsigned size, sf::Color color)
    {
        sf::Text text(*m_font, str, size);
        text.setFillColor(color);
        return text;
    };
    auto centered = [&](sf::Text& text, float y)
    {
        const sf::FloatRect b = text.getLocalBounds();
        text.setPosition({std::round((m_size.x - b.size.x) * 0.5f - b.position.x), std::round(y)});
    };

    // Toast message (volume, errors, mode changes).
    if (ui.messageAlpha > 0.01f && !ui.message.isEmpty())
    {
        sf::Text msg = makeText(ui.message, 18, withAlpha(sf::Color::White, ui.messageAlpha));
        centered(msg, 22.f);
        target.draw(msg);
    }

    if (!ui.loaded)
    {
        const float pulse = 0.75f + 0.25f * std::sin(m_time * 2.f);
        sf::Text title = makeText(U"Bxsim", 64, withAlpha(sf::Color::White, 0.95f));
        centered(title, m_size.y * 0.5f - 90.f);
        target.draw(title);

        const sf::String hint = ui.loading ? sf::String(U"Loading…") : sf::String(U"Drop a song here, or press O to open one");
        sf::Text sub = makeText(hint, 22, withAlpha(sf::Color(200, 210, 255), pulse));
        centered(sub, m_size.y * 0.5f + 4.f);
        target.draw(sub);

        sf::Text formats = makeText(U"MP3 · WAV · OGG · FLAC", 15, withAlpha(sf::Color(150, 160, 190), 0.8f));
        centered(formats, m_size.y * 0.5f + 44.f);
        target.draw(formats);
        return;
    }

    const float a = ui.uiAlpha;
    if (a <= 0.01f)
        return;

    // Song title and status.
    sf::Text title = makeText(ui.title, 24, withAlpha(sf::Color::White, a));
    title.setPosition({32.f, 24.f});
    target.draw(title);

    sf::String status = sf::String(modeName(mode));
    if (!ui.playing)
        status += U"  ·  Paused";
    sf::Text sub = makeText(status, 15, withAlpha(sf::Color(170, 180, 210), a));
    sub.setPosition({33.f, 58.f});
    target.draw(sub);

    // Seek bar.
    const sf::FloatRect box = seekBarHitBox();
    const float barY = box.position.y + box.size.y * 0.5f - 2.f;
    const float fraction = ui.duration > 0.f ? std::clamp(ui.position / ui.duration, 0.f, 1.f) : 0.f;

    sf::RectangleShape track({box.size.x, 4.f});
    track.setPosition({box.position.x, barY});
    track.setFillColor(withAlpha(sf::Color::White, 0.18f * a));
    target.draw(track);

    sf::RectangleShape fill({box.size.x * fraction, 4.f});
    fill.setPosition({box.position.x, barY});
    fill.setFillColor(withAlpha(bandColor(0.2f, 1.f, 0.6f), a));
    target.draw(fill);

    sf::CircleShape knob(6.f);
    knob.setOrigin({6.f, 6.f});
    knob.setPosition({box.position.x + box.size.x * fraction, barY + 2.f});
    knob.setFillColor(withAlpha(sf::Color::White, a));
    target.draw(knob);

    // Times.
    sf::Text now = makeText(formatTime(ui.position), 14, withAlpha(sf::Color(200, 205, 225), a));
    now.setPosition({box.position.x, barY + 12.f});
    target.draw(now);

    sf::Text total = makeText(formatTime(ui.duration), 14, withAlpha(sf::Color(200, 205, 225), a));
    total.setPosition({box.position.x + box.size.x - total.getLocalBounds().size.x, barY + 12.f});
    target.draw(total);

    // Controls hint.
    sf::Text help = makeText(U"Space play/pause   ← → seek   ↑ ↓ volume   M mode   N/P next/prev   O open   F11 fullscreen",
                             13, withAlpha(sf::Color(140, 150, 180), a * 0.9f));
    centered(help, m_size.y - 26.f);
    target.draw(help);
}
