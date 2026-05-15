#define WEBVIEW_IMPLEMENTATION
#include "webview.h"

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrevInst, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInst;
    (void)hPrevInst;
    (void)lpCmdLine;
    (void)nCmdShow;
#else
int main() {
#endif
    auto w = webview_create(0, nullptr);
    //auto a = webview_create(0, nullptr);
    webview_set_title(w, "WebView2窗口bilibili.com");
    webview_set_size(w, 1366, 768, WEBVIEW_HINT_NONE);
    
    //webview_set_title(a, "WebView2窗口bilibili.com");
    // webview_set_size(a, 1366, 768, WEBVIEW_HINT_NONE);

    webview_navigate(w, "https://www.bilibili.com/video/BV1GJ411x7h7");
    // webview_navigate(a, "https://live.bilibili.com/1883358196");

    webview_run(w);
    webview_destroy(w);

    //webview_run(a);
    //webview_destroy(a);

    return 0;
}