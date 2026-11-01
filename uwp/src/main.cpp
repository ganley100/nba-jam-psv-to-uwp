// NBA Jam Loader – minimal UWP shell for Xbox Dev Mode
// Links so_util_win and shows status. SO loading UI comes next.

#include <windows.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <string>
#include <thread>

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

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[800];
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

static void RunProbe() {
    Log(L"NBA Jam Loader 0.1 (UWP scaffold)");
    Log(L"so_util_win linked OK");
    Log(L"");
    Log(L"This build does not load a game yet.");
    Log(L"Next: file picker + so_file_load() on your libnbajam.so");
    Log(L"");
    Log(L"AppMemoryUsageLimit = %llu MB",
        Windows::System::MemoryManager::AppMemoryUsageLimit >> 20);
    Log(L"AppMemoryUsage      = %llu MB",
        Windows::System::MemoryManager::AppMemoryUsage >> 20);
    Log(L"");
    Log(L"== DONE ==");
}

namespace NbaJamLoader {
    ref class App sealed : public UI::Application {
    protected:
        virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override {
            auto tb = ref new UI::Controls::TextBlock();
            tb->FontFamily = ref new UI::Media::FontFamily(L"Consolas");
            tb->FontSize = 20;
            tb->TextWrapping = UI::TextWrapping::Wrap;
            tb->Margin = UI::ThicknessHelper::FromUniformLength(40);
            tb->Foreground = ref new UI::Media::SolidColorBrush(Windows::UI::Colors::White);
            tb->Text = L"Starting...";

            auto sv = ref new UI::Controls::ScrollViewer();
            sv->Content = tb;
            sv->IsTabStop = true;

            auto grid = ref new UI::Controls::Grid();
            grid->Background = ref new UI::Media::SolidColorBrush(
                Windows::UI::ColorHelper::FromArgb(255, 26, 26, 46));
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
