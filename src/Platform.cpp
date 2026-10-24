#include "Platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace platform
{
namespace
{
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#ifdef _WIN32
HWND g_dropWindow = nullptr;
WNDPROC g_originalProc = nullptr;
std::optional<fs::path> g_dropped;

LRESULT CALLBACK dropProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DROPFILES)
    {
        auto drop = reinterpret_cast<HDROP>(wParam);
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        if (length > 0)
        {
            std::wstring name(length, L'\0');
            DragQueryFileW(drop, 0, name.data(), length + 1);
            g_dropped = fs::path(name);
        }
        DragFinish(drop);
        return 0;
    }
    return CallWindowProcW(g_originalProc, hwnd, msg, wParam, lParam);
}
#endif
} // namespace

bool isAudioFile(const fs::path& path)
{
    const std::string ext = lower(path.extension().u8string());
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac";
}

std::optional<fs::path> commandLineFile([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#ifdef _WIN32
    // Use the wide command line so song names with any characters work.
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    std::optional<fs::path> result;
    if (args && count > 1)
        result = fs::path(args[1]);
    if (args)
        LocalFree(args);
    return result;
#else
    if (argc > 1)
        return fs::path(argv[1]);
    return std::nullopt;
#endif
}

std::optional<fs::path> openFileDialog([[maybe_unused]] sf::WindowHandle owner)
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Audio files (*.mp3, *.wav, *.ogg, *.flac)\0*.mp3;*.wav;*.ogg;*.flac\0All files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = L"Open a song";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        return fs::path(buffer);
#endif
    return std::nullopt;
}

void enableFileDrop([[maybe_unused]] sf::WindowHandle window)
{
#ifdef _WIN32
    if (!window || window == g_dropWindow)
        return;
    DragAcceptFiles(window, TRUE);
    g_originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&dropProc)));
    g_dropWindow = window;
#endif
}

std::optional<fs::path> takeDroppedFile()
{
#ifdef _WIN32
    auto result = g_dropped;
    g_dropped.reset();
    return result;
#else
    return std::nullopt;
#endif
}

std::vector<fs::path> fontCandidates()
{
    std::vector<fs::path> fonts;
#ifdef _WIN32
    fs::path dir = "C:\\Windows\\Fonts";
    if (const char* windir = std::getenv("WINDIR"))
        dir = fs::path(windir) / "Fonts";
    for (const char* name : {"segoeui.ttf", "seguisb.ttf", "arial.ttf", "tahoma.ttf"})
        fonts.push_back(dir / name);
#elif defined(__APPLE__)
    fonts.emplace_back("/System/Library/Fonts/SFNS.ttf");
    fonts.emplace_back("/Library/Fonts/Arial Unicode.ttf");
#else
    fonts.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    fonts.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
#endif
    return fonts;
}

std::optional<fs::path> siblingAudioFile(const fs::path& current, int direction)
{
    std::error_code ec;
    const fs::path dir = current.parent_path();
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (entry.is_regular_file(ec) && isAudioFile(entry.path()))
            files.push_back(entry.path());
    if (files.empty())
        return std::nullopt;

    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b)
              { return lower(a.filename().u8string()) < lower(b.filename().u8string()); });

    auto it = std::find_if(files.begin(), files.end(), [&](const fs::path& p)
                           { return fs::equivalent(p, current, ec); });
    if (it == files.end())
        return files.front();

    const auto count = static_cast<long long>(files.size());
    const long long index = ((it - files.begin()) + direction % count + count) % count;
    return files[static_cast<std::size_t>(index)];
}
} // namespace platform
