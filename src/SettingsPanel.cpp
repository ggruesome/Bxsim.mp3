#include "SettingsPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
constexpr float Pad = 22.f;
constexpr float SliderRow = 54.f;
constexpr float ToggleRow = 40.f;
constexpr float SectionGap = 46.f;

const sf::Color Accent(190, 120, 255);
const sf::Color Muted(150, 155, 180);

std::string formatValue(const SettingInfo& info, float v)
{
    char buf[32];
    if (info.isToggle)
        return v > 0.5f ? "On" : "Off";
    if (info.choices)
        return info.choices[std::max(0L, std::min(std::lround(v), std::lround(info.max)) - std::lround(info.min))];
    if (info.isInteger)
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
    else
        std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}
} // namespace

void SettingsPanel::layout(const Settings& settings, int mode, sf::Vector2u size)
{
    const float x0 = static_cast<float>(size.x) - Width;
    m_panel = {{x0, 0.f}, {Width, static_cast<float>(size.y)}};
    m_rows.clear();
    m_resets.clear();

    float y = Pad + 56.f - m_scroll; // below the title
    for (const SettingSection* section : settings.sectionsFor(mode))
    {
        m_resets.push_back({section, {{x0 + Width - Pad - 56.f, y - 2.f}, {56.f, 22.f}}});
        y += 34.f;
        for (const auto& item : section->items)
        {
            Row row;
            row.key = item.key;
            row.toggle = item.isToggle;
            if (item.isToggle)
            {
                row.hit = {{x0 + Pad, y}, {Width - Pad * 2.f, ToggleRow - 6.f}};
                row.track = {{x0 + Width - Pad - 42.f, y + 2.f}, {42.f, 22.f}};
                y += ToggleRow;
            }
            else
            {
                row.hit = {{x0 + Pad, y}, {Width - Pad * 2.f, SliderRow - 6.f}};
                row.track = {{x0 + Pad, y + 30.f}, {Width - Pad * 2.f, 4.f}};
                y += SliderRow;
            }
            m_rows.push_back(row);
        }
        y += SectionGap - 30.f;
    }
    m_contentHeight = y + m_scroll + Pad;
}

void SettingsPanel::setFromX(Settings& settings, const Row& row, float x)
{
    const SettingInfo* info = settings.info(row.key);
    if (!info)
        return;
    const float t = std::clamp((x - row.track.position.x) / row.track.size.x, 0.f, 1.f);
    settings.set(row.key, info->min + (info->max - info->min) * t);
    m_changed = true;
}

bool SettingsPanel::handleEvent(const sf::Event& event, Settings& settings)
{
    if (!m_visible)
        return false;

    if (const auto* press = event.getIf<sf::Event::MouseButtonPressed>())
    {
        const sf::Vector2f p(press->position);
        if (!inside(p))
            return false;
        if (press->button != sf::Mouse::Button::Left)
            return true;
        for (const auto& reset : m_resets)
            if (reset.hit.contains(p))
            {
                settings.resetSection(*reset.section);
                m_changed = true;
                return true;
            }
        for (const auto& row : m_rows)
            if (row.hit.contains(p))
            {
                if (row.toggle)
                {
                    settings.set(row.key, settings.on(row.key) ? 0.f : 1.f);
                    m_changed = true;
                }
                else
                {
                    m_dragging = row.key;
                    setFromX(settings, row, p.x);
                }
                return true;
            }
        return true;
    }
    if (const auto* move = event.getIf<sf::Event::MouseMoved>())
    {
        if (m_dragging.empty())
            return inside(sf::Vector2f(move->position));
        for (const auto& row : m_rows)
            if (row.key == m_dragging)
                setFromX(settings, row, static_cast<float>(move->position.x));
        return true;
    }
    if (event.is<sf::Event::MouseButtonReleased>())
    {
        const bool wasDragging = !m_dragging.empty();
        m_dragging.clear();
        return wasDragging;
    }
    if (const auto* wheel = event.getIf<sf::Event::MouseWheelScrolled>())
    {
        if (!inside(sf::Vector2f(wheel->position)))
            return false;
        const float maxScroll = std::max(0.f, m_contentHeight - m_panel.size.y);
        m_scroll = std::clamp(m_scroll - wheel->delta * 40.f, 0.f, maxScroll);
        return true;
    }
    return false;
}

void SettingsPanel::draw(sf::RenderTarget& target, const sf::Font* font, const Settings& settings, int mode, const char* modeName)
{
    if (!m_visible)
        return;
    layout(settings, mode, target.getSize());

    sf::RectangleShape background(m_panel.size);
    background.setPosition(m_panel.position);
    background.setFillColor(sf::Color(10, 10, 18, 232));
    target.draw(background);
    sf::RectangleShape edge({1.f, m_panel.size.y});
    edge.setPosition(m_panel.position);
    edge.setFillColor(sf::Color(255, 255, 255, 30));
    target.draw(edge);

    if (!font)
        return;
    const float x0 = m_panel.position.x;

    auto text = [&](const std::string& s, unsigned size, sf::Color color, sf::Vector2f pos)
    {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setFillColor(color);
        t.setPosition({std::round(pos.x), std::round(pos.y)});
        target.draw(t);
        return t.getLocalBounds().size.x;
    };
    auto rightText = [&](const std::string& s, unsigned size, sf::Color color, float right, float y)
    {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setFillColor(color);
        t.setPosition({std::round(right - t.getLocalBounds().size.x), std::round(y)});
        target.draw(t);
    };

    // Section titles and their reset buttons.
    for (const auto& reset : m_resets)
    {
        const float y = reset.hit.position.y + 2.f;
        if (y < 60.f || y > m_panel.size.y)
            continue;
        text(reset.section->title, 15, Accent, {x0 + Pad, y});
        rightText("Reset", 13, Muted, reset.hit.position.x + reset.hit.size.x, y + 1.f);
    }

    // Rows.
    for (const auto& row : m_rows)
    {
        if (row.hit.position.y < 60.f || row.hit.position.y > m_panel.size.y - 20.f)
            continue;
        const SettingInfo* info = settings.info(row.key);
        const float value = settings.get(row.key);
        text(info->label, 14, sf::Color(230, 232, 245), row.hit.position);

        if (row.toggle)
        {
            const bool on = value > 0.5f;
            sf::RectangleShape pill(row.track.size);
            pill.setPosition(row.track.position);
            pill.setFillColor(on ? Accent : sf::Color(255, 255, 255, 50));
            target.draw(pill);
            sf::CircleShape knob(8.f);
            knob.setOrigin({8.f, 8.f});
            knob.setPosition({row.track.position.x + (on ? row.track.size.x - 11.f : 11.f), row.track.position.y + 11.f});
            knob.setFillColor(sf::Color::White);
            target.draw(knob);
            continue;
        }

        rightText(formatValue(*info, value), 14, Muted, row.hit.position.x + row.hit.size.x, row.hit.position.y);
        const float t = (value - info->min) / (info->max - info->min);
        sf::RectangleShape track(row.track.size);
        track.setPosition(row.track.position);
        track.setFillColor(sf::Color(255, 255, 255, 40));
        target.draw(track);
        sf::RectangleShape fill({row.track.size.x * t, row.track.size.y});
        fill.setPosition(row.track.position);
        fill.setFillColor(Accent);
        target.draw(fill);
        sf::CircleShape knob(7.f);
        knob.setOrigin({7.f, 7.f});
        knob.setPosition({row.track.position.x + row.track.size.x * t, row.track.position.y + 2.f});
        knob.setFillColor(m_dragging == row.key ? Accent : sf::Color::White);
        target.draw(knob);
    }

    // Title last, over a solid strip so scrolled rows slide under it.
    sf::RectangleShape header({Width, 56.f});
    header.setPosition({x0, 0.f});
    header.setFillColor(sf::Color(10, 10, 18, 250));
    target.draw(header);
    text(std::string(modeName) + " settings", 20, sf::Color::White, {x0 + Pad, 16.f});
    rightText("S to close", 12, Muted, x0 + Width - Pad, 22.f);
}
