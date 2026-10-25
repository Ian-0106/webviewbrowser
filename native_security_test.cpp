// Exercise the real native handlers with COM doubles; no user's clipboard is read.
#define WinMain BrowserWinMainForSecurityTests
#include "main.cpp"
#undef WinMain

#include <cassert>
#include <fstream>
#include <iostream>

class PermissionRequest final : public ComHandlerBase<ICoreWebView2PermissionRequestedEventArgs> {
public:
    COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ;
    COREWEBVIEW2_PERMISSION_STATE state = COREWEBVIEW2_PERMISSION_STATE_DEFAULT;
    HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_PermissionKind(COREWEBVIEW2_PERMISSION_KIND *value) override { *value = kind; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL *value) override { *value = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_State(COREWEBVIEW2_PERMISSION_STATE *value) override { *value = state; return S_OK; }
    HRESULT STDMETHODCALLTYPE put_State(COREWEBVIEW2_PERMISSION_STATE value) override { state = value; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDeferral(ICoreWebView2Deferral **) override { return E_NOTIMPL; }
};

class DownloadOperation final : public ComHandlerBase<ICoreWebView2DownloadOperation> {
public:
    std::wstring id;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
    ~DownloadOperation() override { *alive = false; }
    HRESULT STDMETHODCALLTYPE add_BytesReceivedChanged(ICoreWebView2BytesReceivedChangedEventHandler *, EventRegistrationToken *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE remove_BytesReceivedChanged(EventRegistrationToken) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE add_EstimatedEndTimeChanged(ICoreWebView2EstimatedEndTimeChangedEventHandler *, EventRegistrationToken *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE remove_EstimatedEndTimeChanged(EventRegistrationToken) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE add_StateChanged(ICoreWebView2StateChangedEventHandler *, EventRegistrationToken *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE remove_StateChanged(EventRegistrationToken) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_ContentDisposition(LPWSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_MimeType(LPWSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_TotalBytesToReceive(INT64 *value) override { *value = 100; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_BytesReceived(INT64 *value) override { *value = 10; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_EstimatedEndTime(LPWSTR *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_ResultFilePath(LPWSTR *value) override { *value = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_State(COREWEBVIEW2_DOWNLOAD_STATE *value) override { *value = state; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_InterruptReason(COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON *value) override { *value = reason; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_CanResume(BOOL *value) override { *value = TRUE; return S_OK; }
    HRESULT STDMETHODCALLTYPE Pause() override {
        state = COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED;
        reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED;
        UpdateDownloadOperation(id, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Resume() override {
        state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
        reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
        UpdateDownloadOperation(id, this);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Cancel() override {
        auto lifetime = alive;
        const std::wstring ownedId = id;
        state = COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED;
        reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED;
        UpdateDownloadOperation(ownedId, this);
        assert(*lifetime && "COM operation must stay alive until Cancel returns");
        return S_OK;
    }
};

static DownloadOperation *AddOperation() {
    DownloadEntry entry;
    entry.state = L"progressing";
    entry.url = L"https://example.com/test";
    auto operation = new DownloadOperation();
    operation->id = g_activityStore.AddDownload(entry);
    auto runtime = std::make_unique<DownloadRuntime>();
    runtime->operation = operation;
    runtime->id = operation->id;
    g_downloadRuntime[operation->id] = std::move(runtime);
    return operation;
}

int main() {
    auto permissionHandler = new webview::detail::webview2_com_handler(nullptr,
        [](const std::string &) {}, [](ICoreWebView2Controller *, ICoreWebView2 *) {});
    for (const auto kind : {COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ,
                           COREWEBVIEW2_PERMISSION_KIND_CAMERA,
                           COREWEBVIEW2_PERMISSION_KIND_MICROPHONE,
                           COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION}) {
        auto args = new PermissionRequest();
        args->kind = kind;
        assert(SUCCEEDED(permissionHandler->Invoke(static_cast<ICoreWebView2 *>(nullptr), args)));
        assert(args->state == COREWEBVIEW2_PERMISSION_STATE_DENY);
        args->Release();
    }
    permissionHandler->Release();

    Tab deferredTab;
    deferredTab.w = reinterpret_cast<webview_t>(1);
    g_tabs.push_back(deferredTab);
    CompletePermissionPreparation(deferredTab.w, E_ABORT);
    assert(!g_tabs[0].permissionsReady && "failed permission reset must not permit navigation");
    CompletePermissionPreparation(deferredTab.w, S_OK);
    assert(g_tabs[0].permissionsReady);
    g_tabs.clear();

    const auto directory = std::filesystem::temp_directory_path() / (L"webview-native-security-" + ActivityStore::NewId());
    std::filesystem::create_directories(directory);
    g_activityStore = ActivityStore(directory / L"activity.dat");
    auto operation = AddOperation();
    const std::wstring id = operation->id;
    PerformDownloadAction(id, DownloadAction::PauseResume);
    assert(g_activityStore.FindDownload(id)->state == L"paused");
    assert(FindDownloadRuntime(id));
    PerformDownloadAction(id, DownloadAction::PauseResume);
    assert(g_activityStore.FindDownload(id)->state == L"progressing");
    PerformDownloadAction(id, DownloadAction::PauseResume);
    PerformDownloadAction(id, DownloadAction::Cancel);
    assert(g_activityStore.FindDownload(id)->state == L"cancelled");
    assert(!FindDownloadRuntime(id));
    AddOperation();
    AddOperation();
    CancelActiveDownloads();
    assert(g_downloadRuntime.empty());
    assert(g_activityStore.ActiveDownloadCount() == 0);

    const auto file = directory / L"safe.txt";
    std::ofstream(file) << "harmless fixture";
    assert(FileExists(file.wstring()));
    assert(!FileExists(directory.wstring()));
    assert(!FileExists(L"\\\\server\\share\\file.txt"));
    auto invalid = file.wstring();
    invalid.push_back(L'\0');
    invalid += L"other-file";
    assert(!FileExists(invalid));
    std::filesystem::remove_all(directory);
    std::cout << "native_security_test passed (permission denial, reentrant callbacks, pause/cancel, file scope)\n";
}
