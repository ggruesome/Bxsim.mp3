#pragma once

#include <SFML/Window/WindowHandle.hpp>

#include <filesystem>
#include <optional>
#include <vector>

// The few things SFML doesn't do for us: a native "Open file" dialog, dropping
// files onto the window, and finding a nice system font. Implemented for
// Windows; other platforms get harmless fallbacks.
namespace platform
{
// File passed on the command line, e.g. when a song is dragged onto Bxsim.exe.
[[nodiscard]] std::optional<std::filesystem::path> commandLineFile(int argc, char** argv);

[[nodiscard]] std::optional<std::filesystem::path> openFileDialog(sf::WindowHandle owner);

// Call after every window (re)create.
void enableFileDrop(sf::WindowHandle window);
[[nodiscard]] std::optional<std::filesystem::path> takeDroppedFile();

[[nodiscard]] std::vector<std::filesystem::path> fontCandidates();

[[nodiscard]] bool isAudioFile(const std::filesystem::path& path);

// Next/previous audio file in the same folder (wraps around).
[[nodiscard]] std::optional<std::filesystem::path> siblingAudioFile(const std::filesystem::path& current, int direction);
} // namespace platform
