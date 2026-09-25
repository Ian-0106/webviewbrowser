# WebView Browser

一个用 C++ 手写的 Windows 多标签浏览器：界面用 **Win32 原生 API 自绘**（标签栏、地址栏、工具栏），网页渲染交给 **WebView2**（Microsoft Edge 内核）。

- 版本：**1.2.0**
- 平台：Windows 10 / 11 x64
- 许可证：**AGPL-3.0**（见 LICENSE.txt）

## 功能

- **多标签浏览**：`+` 新建标签、点击切换、`×` 关闭（带悬停与按下反馈）
- **键盘快捷键**：`Ctrl+T` / `Ctrl+W` / `Ctrl+Tab` / `Ctrl+Shift+Tab` / `Ctrl+L`，**在网页内部同样生效**
- **标签标题自动同步**：跟随网页标题更新（WebView2 `DocumentTitleChanged`）
- **地址栏自动同步**：页面跳转后自动更新（WebView2 `SourceChanged`）
- **跳转**：地址栏内按回车，或点击「转到」按钮
- **新窗口请求开新标签**：页面内 `window.open` 等请求由标签接管（WebView2 `NewWindowRequested`）
- **自绘浅色主题**：标签栏配色集中在源码的一组 `COLORREF` 常量中，便于改主题
- 帮助菜单 → About

## 环境要求

| 依赖 | 说明 |
|---|---|
| Windows 10 1809+ / 11（x64） | WebView2 运行时在 Win11 内置；Win10 需安装 Evergreen Runtime |
| Visual Studio 2026（v18） | 需勾选「使用 C++ 的桌面开发」工作负载（含 MSVC 工具集与 Windows SDK） |
| CMake 3.15+ | 推荐 4.x |

**无需额外下载依赖**：`webview.h`（WebView2 的 C++ 封装）与 `WebView2.h`（SDK 头文件）已随仓库提供。

## 构建

推荐使用 CMake Presets（生成器为 `Visual Studio 18 2026`）：

    cmake --preset windows-x64-debug
    cmake --build --preset debug

Release 构建：

    cmake --preset windows-x64-release
    cmake --build --preset release

如果使用其它版本的 Visual Studio，可手动指定生成器：

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Debug

## 运行

    build\debug\Debug\webview_example.exe
    build\release\Release\webview_example.exe

也可以直接用 Visual Studio 打开本目录（文件 → 打开 → 文件夹），按 `F5` 调试运行。

## 快捷键

| 按键 | 作用 |
|---|---|
| `Ctrl + T` | 新建标签页 |
| `Ctrl + W` | 关闭当前标签页 |
| `Ctrl + Tab` | 下一个标签页 |
| `Ctrl + Shift + Tab` | 上一个标签页 |
| `Ctrl + L` | 聚焦地址栏并全选内容 |
| `Enter`（地址栏内） | 打开输入的地址 |

## 目录结构

    .
    ├── main.cpp            # 主程序：窗口、标签栏自绘、WebView2 事件绑定、快捷键
    ├── webview.h           # WebView2 的 C++ 封装（第三方）
    ├── WebView2.h          # Microsoft WebView2 SDK 头文件
    ├── CMakeLists.txt      # 构建脚本（C++17 + /utf-8）
    ├── CMakePresets.json   # 预设：windows-x64-debug / windows-x64-release
    ├── LICENSE.txt         # AGPL-3.0
    └── README.md

## 实现要点

- **Win32 自绘界面**：标签栏与工具栏通过 `WM_PAINT` + GDI 绘制，配色由一组 `COLORREF` 常量定义；鼠标交互通过 `HitKind` 枚举（含 `HIT_CLOSE` 等）配合 `g_tabGeom` 中的命中矩形判断
- **WebView2 集成**：`webview_create()` 为每个标签创建实例，`webview_get_native_handle()` 取出原生窗口句柄后嵌入自绘的标签区域
- **事件绑定**：`NewWindowRequested` → 新标签；`DocumentTitleChanged` → 标签标题；`SourceChanged` → 地址栏同步
- **快捷键的两条通路**：
  - 焦点在自绘控件（标签栏 / 地址栏）时，由 `HACCEL` + `TranslateAccelerator` 在主消息循环中拦截；
  - 焦点在网页内部时，按键不会进入本进程的消息队列，改由 WebView2 的 `AcceleratorKeyPressed` 事件接管；
  - 两条通路最终都 `PostMessage(WM_COMMAND)` 到主窗口，动作只有一份实现

## 已知行为

- 关闭**最后一个**标签页会退出程序（如需改成自动新建空白页，见 `CloseTab()`）

## TODO

- [ ] 前进 / 后退 / 刷新按钮
- [ ] 标签拖拽排序
- [ ] 深色主题

## 许可证

本项目采用 **GNU Affero General Public License v3.0**，详见 [LICENSE.txt](LICENSE.txt)。

> 注意：AGPL 是强传染性协议 —— 修改后分发，或以网络服务形式提供，都需要公开源码。
