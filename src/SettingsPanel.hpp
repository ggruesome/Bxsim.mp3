#pragma once

#include "Settings.hpp"

#include <SFML/Graphics.hpp>

#include <string>
#include <vector>

// The slide-out panel (press S) with sliders and switches for the current mode.
// It's drawn on the window, on top of the visuals, so it never ends up in exported videos.
class SettingsPanel
{
public:
    static constexpr float Width = 340.f;

    void toggle() { m_visible = !m_visible; m_dragging.clear(); }
    [[nodiscard]] bool visible() const { return m_visible; }

    // Mouse handling. Returns true when the panel used the event (so nothing else should).
    bool handleEvent(const sf::Event& event, Settings& settings);

    void draw(sf::RenderTarget& target, const sf::Font* font, const Settings& settings, int mode, const char* modeName);

    // True once after any value changed (so the caller can save).
    bool takeChanged()
    {
        const bool c = m_changed;
        m_changed = false;
        return c;
    }

private:
    struct Row
    {
        std::string key;
        bool toggle = false;
        sf::FloatRect hit;    // whole row
        sf::FloatRect track;  // slider track or switch
    };
    struct ResetButton
    {
        const SettingSection* section = nullptr;
        sf::FloatRect hit;
    };

    void layout(const Settings& settings, int mode, sf::Vector2u size);
    void setFromX(Settings& settings, const Row& row, float x);
    [[nodiscard]] bool inside(sf::Vector2f p) const { return m_visible && m_panel.contains(p); }

    bool m_visible = false;
    bool m_changed = false;
    float m_scroll = 0.f;
    float m_contentHeight = 0.f;
    sf::FloatRect m_panel;
    std::vector<Row> m_rows;
    std::vector<ResetButton> m_resets;
    std::string m_dragging;
};
