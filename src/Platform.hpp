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
enum class FileKind
{
    Audio,
    Image
};

// Files passed on the command line, e.g. a song (and image) dragged onto Bxsim.exe.
[[nodiscard]] std::vector<std::filesystem::path> commandLineFiles(int argc, char** argv);

[[nodiscard]] std::optional<std::filesystem::path> openFileDialog(sf::WindowHandle owner, FileKind kind);

// "Save as" for exported videos (.mp4). suggestedName is shown in the dialog.
[[nodiscard]] std::optional<std::filesystem::path> saveVideoDialog(sf::WindowHandle owner, const std::filesystem::path& suggestedName);

// Call after every window (re)create.
void enableFileDrop(sf::WindowHandle window);

// Open a normal window maximized, so it fills the screen but keeps its title bar and taskbar.
void maximizeWindow(sf::WindowHandle window);
[[nodiscard]] std::vector<std::filesystem::path> takeDroppedFiles();

[[nodiscard]] std::vector<std::filesystem::path> fontCandidates();
[[nodiscard]] std::vector<std::filesystem::path> monoFontCandidates();   // terminal-style text
[[nodiscard]] std::vector<std::filesystem::path> boldFontCandidates();   // chunky headline text

// Where the settings file lives (%APPDATA%\Bxsim\settings.txt on Windows).
[[nodiscard]] std::filesystem::path settingsFile();

[[nodiscard]] bool isAudioFile(const std::filesystem::path& path);
[[nodiscard]] bool isImageFile(const std::filesystem::path& path);

// Next/previous audio file in the same folder (wraps around).
[[nodiscard]] std::optional<std::filesystem::path> siblingAudioFile(const std::filesystem::path& current, int direction);
} // namespace platform
