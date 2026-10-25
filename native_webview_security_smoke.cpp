// Use an isolated WebView2 profile and permission metadata, never clipboard data.
#define WinMain BrowserWinMainForPermissionSmoke
#include "main.cpp"
#undef WinMain
#include <wrl.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <cstdlib>

static bool PumpUntil(const std::function<bool()> &finished) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!finished() && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
    return finished();
}

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    RoInitialize(RO_INIT_SINGLETHREADED);
    const auto directory = std::filesystem::temp_directory_path() /
        (L"webview-permission-smoke-" + ActivityStore::NewId());
    std::filesystem::create_directories(directory / L"site");
    const auto profilePath = directory / L"profile";
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", profilePath.c_str());
    g_activityStore = ActivityStore(directory / L"activity.dat");
    std::ofstream(directory / L"site" / L"check.html") <<
        "<!doctype html><title>Permission metadata only</title><script>"
        "navigator.permissions.query({name:'clipboard-read'}).then(p=>"
        "chrome.webview.postMessage(p.state));</script>";
    webview_t view = webview_create(0, nullptr);
    assert(view);
    std::cerr << "Created isolated WebView2\n";
    auto *controller = static_cast<ICoreWebView2Controller *>(webview_get_native_handle(
        view, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER));
    Microsoft::WRL::ComPtr<ICoreWebView2> core;
    assert(SUCCEEDED(controller->get_CoreWebView2(&core)));
    Microsoft::WRL::ComPtr<ICoreWebView2_3> core3;
    assert(SUCCEEDED(core.As(&core3)));
    assert(SUCCEEDED(core3->SetVirtualHostNameToFolderMapping(L"security-test.invalid",
        (directory / L"site").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)));
    Microsoft::WRL::ComPtr<ICoreWebView2_13> core13;
    Microsoft::WRL::ComPtr<ICoreWebView2Profile> profile;
    Microsoft::WRL::ComPtr<ICoreWebView2Profile4> profile4;
    assert(SUCCEEDED(core.As(&core13)));
    assert(SUCCEEDED(core13->get_Profile(&profile)));
    assert(SUCCEEDED(profile.As(&profile4)));
    bool seeded = false;
    HRESULT seedResult = E_FAIL;
    auto seedCallback = Microsoft::WRL::Callback<ICoreWebView2SetPermissionStateCompletedHandler>(
        [&](HRESULT result) -> HRESULT { seedResult = result; seeded = true; return S_OK; });
    assert(SUCCEEDED(profile4->SetPermissionState(COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ,
        L"https://security-test.invalid", COREWEBVIEW2_PERMISSION_STATE_ALLOW, seedCallback.Get())));
    assert(PumpUntil([&] { return seeded; }) && SUCCEEDED(seedResult));
    std::cerr << "Seeded test-origin clipboard permission\n";
    std::wstring observed;
    auto messageCallback = Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [&](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
            LPWSTR text = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&text)) && text) {
                observed = text;
                CoTaskMemFree(text);
            }
            return S_OK;
        });
    EventRegistrationToken token{};
    core->add_WebMessageReceived(messageCallback.Get(), &token);
    assert(SUCCEEDED(core->Navigate(L"https://security-test.invalid/check.html?before")));
    assert(PumpUntil([&] { return !observed.empty(); }));
    std::wcerr << L"Before reset: " << observed << L"\n";
    assert(observed == L"granted" && "the test must first reproduce a cached ALLOW state");
    observed.clear();
    Tab tab;
    tab.w = view;
    tab.url = L"https://security-test.invalid/check.html?after";
    g_tabs.push_back(tab);
    PreparePermissionSettings(view, core.Get());
    assert(PumpUntil([&] { return !observed.empty(); }));
    std::wcerr << L"After reset: " << observed << L"\n";
    assert(g_tabs[0].permissionsReady);
    assert(observed == L"prompt" || observed == L"denied");
    core->remove_WebMessageReceived(token);
    webview_destroy(view);
    g_tabs.clear();
    SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", nullptr);
    std::cout << "native_webview_security_smoke passed (real cached ALLOW revoked before navigation; no clipboard read)\n";
    RoUninitialize();
    CoUninitialize();
}
