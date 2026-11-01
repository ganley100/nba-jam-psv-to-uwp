// NBA Jam Mobile – UWP shell for Xbox Dev Mode
// Looks for game files on D:\DevelopmentFiles\NBA Jam Mobile

#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "so_util_win.h"
}

using Microsoft::WRL::ComPtr;
namespace UI = Windows::UI::Xaml;

static std::mutex g_mu;
static std::wstring g_log;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static UI::Controls::TextBlock^ g_tb;
static Windows::UI::Core::CoreDispatcher^ g_disp;

// Primary path the user should use on Xbox Dev Mode
static const wchar_t* kGameDir = L"D:\\DevelopmentFiles\\NBA Jam Mobile";

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[900];
    va_list a; va_start(a, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, a); va_end(a);
    std::wstring text;
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_log += buf;
        g_log += L"\r\n";
        if (g_file != INVALID_HANDLE_VALUE) {
            std::string u;
            for (const wchar_t* p = buf; *p; ++p)
                u += (*p < 128) ? (char)*p : '?';
            u += "\r\n";
            DWORD w; WriteFile(g_file, u.data(), (DWORD)u.size(), &w, nullptr);
            FlushFileBuffers(g_file);
        }
        text = g_log;
    }
    if (g_disp && g_tb) {
        auto s = ref new Platform::String(text.c_str());
        g_disp->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([s]() { g_tb->Text = s; }));
    }
}

static bool PathExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES;
}

static bool FileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void ProbeGameFolder(const wchar_t* dir) {
    Log(L"Checking: %s", dir);
    if (!PathExists(dir)) {
        Log(L"  [ ] folder not found");
        return;
    }
    Log(L"  [x] folder exists");

    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\') base += L'\\';

    struct Item { const wchar_t* name; const wchar_t* note; };
    Item items[] = {
        { L"libnbajam.so", L"required (from APK lib/armeabi-v7a or arm64)" },
        { L"main.obb", L"or main.*.obb / data.psarc" },
        { L"*.apk", L"optional – keep a copy of the APK here" },
        { L"*.obb", L"optional – OBB package" },
    };

    // Check known filenames
    const wchar_t* known[] = {
        L"libnbajam.so",
        L"libNimble.so",
        L"main.obb",
        L"data.psarc",
        L"main.40080.com.eamobile.nbajam_row_wf.obb",
    };
    for (const wchar_t* name : known) {
        std::wstring full = base + name;
        if (FileExists(full.c_str()))
            Log(L"  [x] %s", name);
        else
            Log(L"  [ ] %s", name);
    }

    // List a few files in the directory (best-effort)
    std::wstring pattern = base + L"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        int shown = 0;
        do {
            if (fd.cFileName[0] == L'.') continue;
            if (shown == 0) Log(L"  Files in folder:");
            Log(L"    - %s", fd.cFileName);
            if (++shown >= 20) {
                Log(L"    ...");
                break;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (shown == 0) Log(L"  (folder is empty)");
    }
}

static void RunProbe() {
    Log(L"========================================");
    Log(L"  NBA Jam Mobile");
    Log(L"  Xbox Dev Mode loader");
    Log(L"========================================");
    Log(L"");
    Log(L"PUT THE OBB AND APK HERE:");
    Log(L"");
    Log(L"  D:\\DevelopmentFiles\\NBA Jam Mobile\\");
    Log(L"");
    Log(L"Recommended contents:");
    Log(L"  1. Your NBA Jam .apk  (keep a copy)");
    Log(L"  2. Your .obb file(s)");
    Log(L"  3. libnbajam.so extracted from the APK");
    Log(L"     (lib/armeabi-v7a or lib/arm64-v8a)");
    Log(L"");
    Log(L"Create the folder in Device Portal / File explorer");
    Log(L"or from a PC over SMB if you have it set up.");
    Log(L"");

    // Probe D: and a few fallbacks
    const wchar_t* candidates[] = {
        L"D:\\DevelopmentFiles\\NBA Jam Mobile",
        L"D:\\DevelopmentFiles\\NBAJamMobile",
        L"D:\\NBA Jam Mobile",
        L"E:\\DevelopmentFiles\\NBA Jam Mobile",
    };

    bool any = false;
    for (const wchar_t* c : candidates) {
        if (PathExists(c)) {
            any = true;
            ProbeGameFolder(c);
            Log(L"");
        }
    }
    if (!any) {
        Log(L"No game folder found yet.");
        Log(L"Create this folder and copy your files:");
        Log(L"  %s", kGameDir);
        Log(L"");
    }

    // Also note LocalState as secondary location
    try {
        auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
        Log(L"App LocalState (fallback):");
        Log(L"  %s", local->Data());
    } catch (...) {
        Log(L"(could not read LocalState path)");
    }

    Log(L"");
    Log(L"so_util_win linked OK");
    Log(L"AppMemoryUsageLimit = %llu MB",
        Windows::System::MemoryManager::AppMemoryUsageLimit >> 20);
    Log(L"AppMemoryUsage      = %llu MB",
        Windows::System::MemoryManager::AppMemoryUsage >> 20);
    Log(L"");
    Log(L"== DONE ==");
    Log(L"Next build will load libnbajam.so from that folder.");
}

namespace NbaJamLoader {
    ref class App sealed : public UI::Application {
    protected:
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override {
            auto tb = ref new UI::Controls::TextBlock();
            tb->FontFamily = ref new UI::Media::FontFamily(L"Consolas");
            tb->FontSize = 20;
            tb->TextWrapping = UI::TextWrapping::Wrap;
            tb->Margin = UI::ThicknessHelper::FromUniformLength(36);
            tb->Foreground = ref new UI::Media::SolidColorBrush(Windows::UI::Colors::White);
            tb->Text = L"NBA Jam Mobile — starting...";

            auto sv = ref new UI::Controls::ScrollViewer();
            sv->Content = tb;
            sv->IsTabStop = true;

            auto grid = ref new UI::Controls::Grid();
            grid->Background = ref new UI::Media::SolidColorBrush(
                Windows::UI::ColorHelper::FromArgb(255, 0, 0, 0));
            grid->Children->Append(sv);

            UI::Window::Current->Content = grid;
            UI::Window::Current->Activate();
            sv->Focus(UI::FocusState::Programmatic);

            g_tb = tb;
            g_disp = UI::Window::Current->Dispatcher;

            std::wstring path =
                std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) +
                L"\\loader_log.txt";
            g_file = CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);

            std::thread(RunProbe).detach();
        }
    };
}

int main(Platform::Array<Platform::String^>^) {
    UI::Application::Start(ref new UI::ApplicationInitializationCallback(
        [](UI::ApplicationInitializationCallbackParams^) {
            ref new NbaJamLoader::App();
        }));
    return 0;
}
