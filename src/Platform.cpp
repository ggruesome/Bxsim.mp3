#include "Platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
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
std::vector<fs::path> g_dropped;

LRESULT CALLBACK dropProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DROPFILES)
    {
        auto drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0)
                continue;
            std::wstring name(length, L'\0');
            DragQueryFileW(drop, i, name.data(), length + 1);
            g_dropped.emplace_back(name);
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

bool isImageFile(const fs::path& path)
{
    const std::string ext = lower(path.extension().u8string());
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga" ||
           ext == ".gif" || ext == ".psd" || ext == ".hdr";
}

std::vector<fs::path> commandLineFiles([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    std::vector<fs::path> files;
#ifdef _WIN32
    // Use the wide command line so file names with any characters work.
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args)
    {
        for (int i = 1; i < count; ++i)
            files.emplace_back(args[i]);
        LocalFree(args);
    }
#else
    for (int i = 1; i < argc; ++i)
        files.emplace_back(argv[i]);
#endif
    return files;
}

std::optional<fs::path> openFileDialog([[maybe_unused]] sf::WindowHandle owner, [[maybe_unused]] FileKind kind)
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    const bool image = kind == FileKind::Image;
    ofn.lpstrFilter = image
        ? L"Images (*.png, *.jpg, *.bmp, *.gif, *.tga)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tga;*.psd;*.hdr\0All files\0*.*\0"
        : L"Audio files (*.mp3, *.wav, *.ogg, *.flac)\0*.mp3;*.wav;*.ogg;*.flac\0All files\0*.*\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = image ? L"Choose an image" : L"Open a song";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        return fs::path(buffer);
#endif
    return std::nullopt;
}

std::optional<fs::path> saveVideoDialog([[maybe_unused]] sf::WindowHandle owner,
                                        [[maybe_unused]] const fs::path& suggestedName)
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4] = L"";
    const std::wstring suggestion = suggestedName.wstring();
    suggestion.copy(buffer, std::min<std::size_t>(suggestion.size(), std::size(buffer) - 1));
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"MP4 video (*.mp4)\0*.mp4\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = static_cast<DWORD>(std::size(buffer));
    ofn.lpstrTitle = L"Export video";
    ofn.lpstrDefExt = L"mp4";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn))
        return fs::path(buffer);
    return std::nullopt;
#else
    // No native dialog here: save next to the working directory.
    return fs::current_path() / suggestedName;
#endif
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

void maximizeWindow([[maybe_unused]] sf::WindowHandle window)
{
#ifdef _WIN32
    if (window)
        ShowWindow(window, SW_MAXIMIZE);
#endif
}

std::vector<fs::path> takeDroppedFiles()
{
#ifdef _WIN32
    std::vector<fs::path> result;
    result.swap(g_dropped);
    return result;
#else
    return {};
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

fs::path settingsFile()
{
#ifdef _WIN32
    if (const wchar_t* appData = _wgetenv(L"APPDATA"))
        return fs::path(appData) / "Bxsim" / "settings.txt";
#else
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".config" / "bxsim" / "settings.txt";
#endif
    return fs::path("bxsim-settings.txt");
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
