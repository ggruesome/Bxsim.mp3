#include "Settings.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace
{
const char* const DystopiaColors[] = {"Toxic green", "Neon shift", "Cyber", "Red"};
}

Settings::Settings()
{
    // {key, label, min, max, default, toggle, integer}
    m_sections = {
        {"Reactivity",
         {
             {"global.sensitivity", "Sensitivity", 0.4f, 2.5f, 1.f},
             {"global.smoothing", "Smoothing", 0.f, 1.f, 0.5f},
             {"global.kick", "Kick strength", 0.f, 2.5f, 1.f},
             {"global.glow", "Glow", 0.f, 2.f, 1.f},
             {"global.particles", "Particles", 0.f, 3.f, 1.f},
         }},
        {"Bars",
         {
             {"bars.count", "Bar count", 16.f, 192.f, 96.f, false, true},
             {"bars.width", "Bar width", 0.15f, 1.f, 0.72f},
             {"bars.height", "Height", 0.3f, 1.6f, 1.f},
             {"bars.peaks", "Peak caps", 0.f, 1.f, 1.f, true},
             {"bars.reflection", "Reflection", 0.f, 1.f, 1.f, true},
         }},
        {"Ring",
         {
             {"ring.size", "Ring size", 0.4f, 1.8f, 1.f},
             {"ring.length", "Bar length", 0.3f, 2.2f, 1.f},
             {"ring.count", "Bar count", 16.f, 192.f, 96.f, false, true},
             {"ring.spin", "Spin speed", -2.f, 2.f, 0.f},
             {"ring.wave", "Inner waveform", 0.f, 1.f, 1.f, true},
         }},
        {"Scope",
         {
             {"scope.height", "Wave height", 0.3f, 2.f, 1.f},
             {"scope.thickness", "Line thickness", 1.f, 10.f, 2.5f},
             {"scope.silhouette", "Spectrum below", 0.f, 1.f, 1.f, true},
             {"scope.silhouetteHeight", "Spectrum height", 0.3f, 2.f, 1.f},
         }},
        {"Lissajous",
         {
             {"liss.size", "Size", 0.4f, 1.6f, 1.f},
             {"liss.thickness", "Line thickness", 1.f, 6.f, 2.f},
             {"liss.copies", "Copies", 1.f, 6.f, 2.f, false, true},
             {"liss.spin", "Spin speed", 0.f, 3.f, 1.f},
             {"liss.trails", "Trail length", 0.f, 1.f, 0.5f},
         }},
        {"Tunnel",
         {
             {"tunnel.speed", "Fly speed", 0.2f, 3.f, 1.f},
             {"tunnel.spin", "Spin speed", 0.f, 3.f, 1.f},
             {"tunnel.size", "Ring size", 0.4f, 2.5f, 1.f},
             {"tunnel.trails", "Trail length", 0.f, 1.f, 0.5f},
             {"tunnel.mirror", "Kaleidoscope", 0.f, 1.f, 1.f, true},
         }},
        {"Liquid",
         {
             {"liquid.speed", "Flow speed", 0.1f, 3.f, 1.f},
             {"liquid.warp", "Turbulence", 0.f, 2.f, 1.f},
             {"liquid.scale", "Zoom", 0.4f, 2.5f, 1.f},
             {"liquid.gloss", "Gloss", 0.f, 2.f, 1.f},
             {"liquid.kaleido", "Kaleidoscope", 0.f, 1.f, 1.f, true},
         }},
        {"Oscilloscope",
         {
             {"osc.size", "Screen size", 0.4f, 1.15f, 1.f},
             {"osc.thickness", "Beam thickness", 0.5f, 5.f, 1.6f},
             {"osc.brightness", "Brightness", 0.3f, 2.f, 1.f},
             {"osc.persistence", "Persistence", 0.f, 1.f, 0.4f},
             {"osc.grid", "Grid", 0.f, 1.f, 1.f, true},
         }},
        {"Dark Fantasy",
         {
             {"fantasy.speed", "Fly speed", 0.f, 3.f, 1.f},
             {"fantasy.quality", "Render quality", 0.5f, 1.6f, 1.f},
             {"fantasy.windows", "Window glow", 0.f, 2.5f, 1.f},
             {"fantasy.castle", "Castle height", 0.4f, 2.f, 1.f},
             {"fantasy.towers", "Towers", 5.f, 28.f, 13.f, false, true},
             {"fantasy.moon", "Moon size", 0.4f, 2.2f, 1.f},
             {"fantasy.lightning", "Lightning", 0.f, 2.f, 1.f},
             {"fantasy.bats", "Bats", 0.f, 3.f, 1.f},
             {"fantasy.fog", "Fog", 0.f, 2.f, 1.f},
             {"fantasy.clouds", "Cloud speed", 0.f, 3.f, 1.f},
         }},
        {"Pop",
         {
             {"pop.bursts", "Starbursts", 0.f, 1.f, 0.8f},
             {"pop.warp", "Picture warp", 0.f, 2.f, 1.f},
             {"pop.rings", "Rings", 0.f, 3.f, 1.f},
             {"pop.confetti", "Confetti", 0.f, 3.f, 1.f},
             {"pop.archOn", "Spectrum arch", 0.f, 1.f, 1.f, true},
             {"pop.arch", "Arch size", 0.f, 2.f, 1.f},
             {"pop.outline", "Outline thickness", 0.f, 3.f, 1.f},
             {"pop.clouds", "Cloud speed", 0.f, 3.f, 1.f},
         }},
        {"Night Drive",
         {
             {"drive.speed", "Speed", 0.2f, 3.f, 1.f},
             {"drive.quality", "Render quality", 0.5f, 1.6f, 1.f},
             {"drive.rain", "Rain", 0.f, 1.f, 0.5f},
             {"drive.traffic", "Traffic", 0.f, 8.f, 6.f, false, true},
             {"drive.lamps", "Streetlights", 0.f, 2.f, 1.f},
             {"drive.city", "City skyline", 0.f, 1.6f, 1.f},
             {"drive.dash", "Show dashboard", 0.f, 1.f, 1.f, true},
         }},
        {"Dark Forest",
         {
             {"forest.speed", "Walk speed", 0.f, 3.f, 1.f},
             {"forest.quality", "Render quality", 0.5f, 1.6f, 1.f},
             {"forest.light", "Flashlight", 0.3f, 2.f, 1.f},
             {"forest.fog", "Fog", 0.2f, 2.f, 1.f},
             {"forest.static", "Static", 0.f, 2.f, 1.f},
             {"forest.trees", "Tree density", 0.2f, 1.f, 0.75f},
             {"forest.pages", "Pages", 0.f, 1.f, 1.f, true},
         }},
        {"Hijack",
         {
             {"hijack.glitch", "Glitch", 0.f, 2.f, 1.f},
             {"hijack.popups", "Popups", 0.f, 3.f, 1.f},
             {"hijack.stickers", "Stickers", 0.f, 3.f, 1.f},
             {"hijack.dots", "Halftone size", 0.4f, 2.5f, 1.f},
             {"hijack.words", "Big words", 0.f, 1.f, 1.f, true},
             {"hijack.terminal", "Terminal", 0.f, 1.f, 1.f, true},
             {"hijack.bars", "Spectrum blocks", 0.f, 1.f, 1.f, true},
         }},
        {"Rage",
         {
             {"rage.quality", "Render quality", 0.5f, 1.6f, 1.f},
             {"rage.strobe", "Strobes", 0.f, 2.f, 1.f},
             {"rage.shake", "Camera shake", 0.f, 2.f, 1.f},
             {"rage.beams", "Light beams", 2.f, 14.f, 8.f, false, true},
             {"rage.crowd", "Crowd", 0.3f, 1.5f, 1.f},
             {"rage.faces", "Glowing faces", 0.f, 1.f, 1.f, true},
             {"rage.bells", "Bell sparkles", 0.f, 3.f, 1.f},
             {"rage.grain", "Grain", 0.f, 2.f, 1.f},
             {"rage.words", "Drop words", 0.f, 1.f, 1.f, true},
         }},
        {"Dystopia",
         {
             {"dys.speed", "Fly speed", 0.f, 3.f, 1.f},
             {"dys.quality", "Render quality", 0.5f, 1.6f, 1.f},
             {"dys.colors", "Colors", 0.f, 3.f, 0.f, false, true, DystopiaColors},
             {"dys.lights", "Window lights", 0.f, 2.5f, 1.f},
             {"dys.fog", "Smog", 0.3f, 2.5f, 1.f},
             {"dys.flash", "Flashes", 0.f, 2.f, 1.f},
             {"dys.screens", "Giant screens", 0.f, 1.f, 1.f, true},
             {"dys.hud", "Surveillance HUD", 0.f, 1.f, 1.f, true},
         }},
    };
    for (const auto& section : m_sections)
        for (const auto& item : section.items)
            m_values[item.key] = item.defaultValue;
}

float Settings::get(const std::string& key) const
{
    const auto it = m_values.find(key);
    return it == m_values.end() ? 0.f : it->second;
}

const SettingInfo* Settings::info(const std::string& key) const
{
    for (const auto& section : m_sections)
        for (const auto& item : section.items)
            if (key == item.key)
                return &item;
    return nullptr;
}

void Settings::set(const std::string& key, float value)
{
    const SettingInfo* i = info(key);
    if (!i)
        return;
    value = std::clamp(value, i->min, i->max);
    if (i->isInteger || i->isToggle)
        value = std::round(value);
    m_values[key] = value;
}

std::vector<const SettingSection*> Settings::sectionsFor(int mode) const
{
    std::vector<const SettingSection*> out{&m_sections[0]};
    const auto index = static_cast<std::size_t>(mode + 1);
    if (index < m_sections.size())
        out.push_back(&m_sections[index]);
    return out;
}

void Settings::resetSection(const SettingSection& section)
{
    for (const auto& item : section.items)
        m_values[item.key] = item.defaultValue;
}

void Settings::load(const std::filesystem::path& file)
{
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::istringstream value(line.substr(eq + 1));
        float v = 0.f;
        if (value >> v)
            set(line.substr(0, eq), v); // unknown keys are ignored, values are clamped
    }
}

void Settings::save(const std::filesystem::path& file) const
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file);
    for (const auto& section : m_sections)
        for (const auto& item : section.items)
            out << item.key << '=' << get(item.key) << '\n';
}
