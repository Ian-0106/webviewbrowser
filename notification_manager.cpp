#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include "notification_manager.h"

#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <notificationactivationcallback.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

#include <filesystem>
#include <memory>

using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::UI::Notifications;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

namespace {
constexpr wchar_t kAppId[] = L"OpenCode.WebViewBrowser";
constexpr wchar_t kActivatorClsidText[] = L"{18B30D36-17EC-4DF4-A4D5-E92F11B37D62}";

EXTERN_C const PROPERTYKEY DECLSPEC_SELECTANY PKEY_AppUserModel_ToastActivatorCLSID = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 26};

HWND g_targetWindow = nullptr;
UINT g_activationMessage = 0;
bool g_registered = false;

std::wstring EscapeXml(const std::wstring &value) {
    std::wstring result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        switch (ch) {
        case L'&': result += L"&amp;"; break;
        case L'<': result += L"&lt;"; break;
        case L'>': result += L"&gt;"; break;
        case L'\"': result += L"&quot;"; break;
        case L'\'': result += L"&apos;"; break;
        default: result.push_back(ch); break;
        }
    }
    return result;
}

std::wstring FormatBytes(std::int64_t value) {
    if (value <= 0) return L"";
    const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1024.0 && unit < 4) {
        amount /= 1024.0;
        ++unit;
    }
    wchar_t buffer[64];
    swprintf_s(buffer, unit == 0 || amount >= 10.0 ? L"%.0f %s" : L"%.1f %s",
               amount, units[unit]);
    return buffer;
}

std::filesystem::path InstalledShortcutPath() {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return {};
    std::filesystem::path result(roaming);
    CoTaskMemFree(roaming);
    return result / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs" /
           L"WebView Browser.lnk";
}

std::filesystem::path ExecutablePath() {
    std::wstring buffer(32768, L'\0');
    DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count == buffer.size()) return {};
    buffer.resize(count);
    return std::filesystem::weakly_canonical(buffer);
}

bool ConfigureInstalledIdentity() {
    const auto shortcutPath = InstalledShortcutPath();
    const auto executablePath = ExecutablePath();
    if (shortcutPath.empty() || executablePath.empty() || !std::filesystem::exists(shortcutPath)) return false;

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link)))) return false;
    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist)) || FAILED(persist->Load(shortcutPath.c_str(), STGM_READWRITE))) return false;
    wchar_t target[32768]{};
    WIN32_FIND_DATAW data{};
    if (FAILED(link->GetPath(target, ARRAYSIZE(target), &data, SLGP_RAWPATH))) return false;
    std::error_code ec;
    if (!std::filesystem::equivalent(executablePath, std::filesystem::path(target), ec) || ec) return false;

    ComPtr<IPropertyStore> properties;
    if (FAILED(link.As(&properties))) return false;
    PROPVARIANT value{};
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<PWSTR>(kAppId);
    if (FAILED(properties->SetValue(PKEY_AppUserModel_ID, value))) return false;
    CLSID clsid{};
    if (FAILED(CLSIDFromString(kActivatorClsidText, &clsid))) return false;
    value = {};
    value.vt = VT_CLSID;
    value.puuid = &clsid;
    if (FAILED(properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, value)) ||
        FAILED(properties->Commit()) || FAILED(persist->Save(shortcutPath.c_str(), TRUE))) return false;

    const std::wstring registryPath =
        L"Software\\Classes\\CLSID\\" + std::wstring(kActivatorClsidText) + L"\\LocalServer32";
    const std::wstring command = L"\"" + executablePath.wstring() + L"\" -ToastActivated";
    if (RegSetKeyValueW(HKEY_CURRENT_USER, registryPath.c_str(), nullptr, REG_SZ,
                        command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) != ERROR_SUCCESS) {
        return false;
    }
    SetCurrentProcessExplicitAppUserModelID(kAppId);
    return true;
}

class DECLSPEC_UUID("18B30D36-17EC-4DF4-A4D5-E92F11B37D62")
NotificationActivator WrlSealed
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, INotificationActivationCallback> {
public:
    HRESULT STDMETHODCALLTYPE Activate(LPCWSTR, LPCWSTR invokedArgs,
        const NOTIFICATION_USER_INPUT_DATA *, ULONG) override {
        if (g_targetWindow && g_activationMessage) {
            auto arguments = std::make_unique<std::wstring>(invokedArgs ? invokedArgs : L"");
            if (PostMessageW(g_targetWindow, g_activationMessage, 0,
                             reinterpret_cast<LPARAM>(arguments.get()))) {
                arguments.release();
            }
        }
        return S_OK;
    }
};

CoCreatableClass(NotificationActivator);

bool BuildXmlDocument(const std::wstring &content, IXmlDocument **result) {
    *result = nullptr;
    ComPtr<IInspectable> inspectable;
    if (FAILED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
                                  &inspectable))) return false;
    ComPtr<IXmlDocument> document;
    ComPtr<IXmlDocumentIO> documentIo;
    if (FAILED(inspectable.As(&document)) || FAILED(inspectable.As(&documentIo)) ||
        FAILED(documentIo->LoadXml(HStringReference(content.c_str()).Get()))) return false;
    *result = document.Detach();
    return true;
}
}

bool InitializeNativeNotifications(HWND targetWindow, UINT activationMessage) {
    g_targetWindow = targetWindow;
    g_activationMessage = activationMessage;
    if (!ConfigureInstalledIdentity()) return false;
    Module<OutOfProc>::Create([] {});
    Module<OutOfProc>::GetModule().IncrementObjectCount();
    g_registered = SUCCEEDED(Module<OutOfProc>::GetModule().RegisterObjects());
    return g_registered;
}

void ShutdownNativeNotifications() {
    if (g_registered) {
        Module<OutOfProc>::GetModule().UnregisterObjects();
        Module<OutOfProc>::GetModule().DecrementObjectCount();
        g_registered = false;
    }
    g_targetWindow = nullptr;
    g_activationMessage = 0;
}

bool ShowNativeDownloadNotification(const std::wstring &id,
                                    const std::wstring &filename,
                                    std::int64_t totalBytes,
                                    bool completed,
                                    const std::wstring &failureReason) {
    if (!g_registered) return false;
    const std::wstring title = completed ? L"Download complete" : L"Download failed";
    std::wstring body = filename;
    if (completed && totalBytes > 0) body += L" (" + FormatBytes(totalBytes) + L")";
    if (!completed && !failureReason.empty()) body += L": " + failureReason;
    const std::wstring action = completed ? L"open" : L"retry";
    const std::wstring label = completed ? L"Open file" : L"Retry";
    const std::wstring xml = L"<toast launch='download:" + EscapeXml(id) +
        L":show'><visual><binding template='ToastGeneric'><text>" + EscapeXml(title) +
        L"</text><text>" + EscapeXml(body) +
        L"</text></binding></visual><actions><action content='" + label +
        L"' arguments='download:" + EscapeXml(id) + L":" + action +
        L"' activationType='foreground'/></actions></toast>";

    ComPtr<IXmlDocument> document;
    if (!BuildXmlDocument(xml, &document)) return false;
    ComPtr<IToastNotificationManagerStatics> manager;
    if (FAILED(Windows::Foundation::GetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
            &manager))) return false;
    ComPtr<IToastNotifier> notifier;
    if (FAILED(manager->CreateToastNotifierWithId(HStringReference(kAppId).Get(), &notifier))) return false;
    ComPtr<IToastNotificationFactory> factory;
    if (FAILED(Windows::Foundation::GetActivationFactory(
            HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
            &factory))) return false;
    ComPtr<IToastNotification> notification;
    if (FAILED(factory->CreateToastNotification(document.Get(), &notification))) return false;
    return SUCCEEDED(notifier->Show(notification.Get()));
}
