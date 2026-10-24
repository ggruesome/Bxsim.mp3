#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Every adjustable setting: a "Reactivity" section shared by all modes, plus one section per
// mode. Values are kept by key ("ring.size") and saved to a small text file between sessions.
struct SettingInfo
{
    const char* key;
    const char* label;
    float min;
    float max;
    float defaultValue;
    bool isToggle = false;   // on/off instead of a slider
    bool isInteger = false;  // whole numbers only (counts)
};

struct SettingSection
{
    const char* title;
    std::vector<SettingInfo> items;
};

class Settings
{
public:
    Settings();

    [[nodiscard]] float get(const std::string& key) const;
    [[nodiscard]] bool on(const std::string& key) const { return get(key) > 0.5f; }
    void set(const std::string& key, float value);

    // Sections shown in the panel for a mode index (0..6): Reactivity first, then the mode's own.
    [[nodiscard]] std::vector<const SettingSection*> sectionsFor(int mode) const;
    [[nodiscard]] const SettingInfo* info(const std::string& key) const;

    void resetSection(const SettingSection& section);

    void load(const std::filesystem::path& file);
    void save(const std::filesystem::path& file) const;

private:
    std::vector<SettingSection> m_sections; // [0] = reactivity, [1 + mode] = that mode
    std::unordered_map<std::string, float> m_values;
};
