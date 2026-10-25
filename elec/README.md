# WebView Browser（Electron 版）

这是根目录 C++/WebView2 浏览器的 Electron 实现，保留了原程序的主要行为：

- 多标签浏览：新建、切换、关闭标签页
- 网页标题和地址栏自动同步
- 地址输入、域名补全和 Bing 搜索
- 网页弹窗在新标签页中打开
- `Ctrl+T`、`Ctrl+W`、`Ctrl+Tab`、`Ctrl+Shift+Tab`、`Ctrl+L` 快捷键
- 浏览历史：成功页面记录、Today/Yesterday 分组、搜索、打开、批量删除与清空（90 天 / 10,000 条）
- 下载记录：进度、暂停/继续/取消/重试、打开文件/文件夹、筛选、移入回收站（365 天 / 5,000 条）
- 独立 Activity Center：工具栏、Activity 菜单或 `Ctrl+H` / `Ctrl+J` 打开；关闭窗口不影响下载
- Windows 下载通知：完成可打开文件，失败可重试；安装版支持通知中心激活，便携版仅保证运行期间尽力通知
- Help → About 菜单
- 关闭最后一个标签页时退出程序

## 运行

需要 Node.js 22 或更高版本。

```powershell
cd elec
npm install
npm start
```

## 验证

```powershell
npm run check
```

## 打包 Windows 可执行文件

```powershell
npm run build:win
```

构建完成后，可直接双击运行：

```text
dist/WebView-Browser-Electron-Setup-1.3.1.exe
dist/WebView-Browser-Electron-1.3.1-portable.exe
```

安装版按当前用户安装，不需要管理员权限；开始菜单快捷方式必建，桌面快捷方式默认不勾选。覆盖升级保留活动记录。卸载时可选删除历史与下载记录（默认不删除），且不会删除已下载文件。

活动记录保存在 Electron 的用户数据目录：`%APPDATA%\WebView Browser Electron\activity.json`。程序退出时会提示仍在进行的下载；未完成记录在下次启动时标记为 interrupted，可手动重试。

## 结构

```text
elec/
├── main.js              # Electron 主进程与标签页管理
├── preload.js           # 最小权限的界面通信桥
├── renderer/            # 浏览器界面与 Activity Center
├── src/activity-store.js # 历史与下载记录持久化
├── src/url.js           # 地址规范化逻辑
├── test/                # 地址与活动记录测试
└── build/installer.nsh  # 安装/卸载自定义选项
```

安装版与便携版均未进行代码签名，Windows 可能显示发布者未知提示。

## v1.3.1 安全修复

- 所有内部消息校验发送窗口、主框架和完整本地页面地址；浏览器工具栏与活动中心使用独立的最小权限通信桥。
- 子框架跳转不再改写主地址栏；网页弹窗只允许 HTTP/HTTPS，手动打开本地文件仍可用。
- 内部界面启用严格内容安全策略，拦截导航、重定向及子框架导航。
- 删除下载文件由主进程弹出确认；文件操作拒绝目录、链接、NUL、相对路径和 UNC 网络路径。
- 损坏的活动记录会校验结构、保留证据并恢复。

运行真实 Electron 进程中的安全集成测试：

```powershell
node node_modules/electron/cli.js verification/security-smoke.cjs
```

测试使用临时资料目录和本地 HTTP 页面；回收站操作被测试替身拦截，不会删除真实下载。详细审查结论见根目录 `security_best_practices_report.md`。

项目继续遵循仓库根目录的 AGPL-3.0 许可证。
