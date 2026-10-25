# 安全审查与修复报告

审查日期：2026-10-26（Asia/Shanghai）。审查基线：`16bef63b9d7fe90fc9ab24ed97b812017abb67cd` / v1.3.0。修复版本：v1.3.1。

## 结论

发现并修复 1 项高风险权限问题，以及地址栏伪装、弹窗协议检查和下载回调内存安全问题；另补强 IPC、文件操作和本地记录校验。初始结论为 **needs changes**；完成下述修复与回归后，在本报告所列范围内结论为 **pass**。没有确认可直接利用的远程代码执行链，也不应将本次源码审查理解为不存在其他漏洞的保证。

## 威胁模型与范围

- 入口：任意网站的 JavaScript、子框架、弹窗、下载事件、网页标题/地址、Electron 消息、Windows 通知激活参数和本地活动记录。
- 信任边界：不可信网页 → 浏览器地址栏/本地资料；Electron 渲染进程 → 可读写文件的主进程；WebView2 回调 → 原生对象生命周期；记录中的路径 → 系统文件操作。
- 保护对象：剪贴板、浏览历史、下载文件、界面显示的真实地址、应用完整性与可用性。
- 攻击者目标：未经授权读取敏感内容、伪装地址诱导用户、扩大本地文件访问、滥用文件操作或使应用崩溃。
- 范围包括 C++/Win32/WebView2、Electron 主进程与内部前端、安装脚本、锁定的 npm 依赖列表。没有对第三方浏览器引擎二进制做漏洞挖掘。

## 已修复问题

### SEC-01：原版自动批准网页剪贴板读取权限

- 严重性：**High**；置信度：**High**；状态：已修复。
- 位置：[webview.h:3662](webview.h#L3662)，`webview2_com_handler::Invoke(PermissionRequested)`；升级权限撤销在 [main.cpp:361](main.cpp#L361)。
- 证据：v1.3.0 针对 `COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ` 无条件设置 `COREWEBVIEW2_PERMISSION_STATE_ALLOW`，没有来源检查或用户确认。
- 影响：恶意网页在浏览器所要求的上下文条件满足时可获得剪贴板读取授权，暴露复制的敏感信息。Microsoft 将此权限定义为无需用户手势的系统剪贴板读取权限。
- 修复：网页权限请求统一返回 `DENY`，不再自动批准剪贴板、摄像头、麦克风或定位权限，并禁用权限持久化。考虑到旧版已保存的允许状态可能使事件不再触发，每个新标签首次导航前使用 Profile4 枚举已保存的权限，将允许项显式改为拒绝，全部完成后才开始导航；检查或撤销失败保持禁止导航。不清除 Cookie、密码、站点存储、浏览或下载历史。用户主动键盘粘贴不属于自动网页读取授权。
- 验证：原生测试直接调用真实权限处理器，对剪贴板、摄像头、麦克风和定位请求检查拒绝结果，并检查权限重置失败时导航保持关闭、成功后才允许导航。真实 WebView2 集成测试在独立临时 Profile 中创建测试来源的缓存允许权限，修复前查询为 `granted`，调用真实升级处理后查询为 `denied`；没有读取用户剪贴板。

### SEC-02：Electron 子框架页内导航可覆盖主地址栏

- 严重性：**Medium**；置信度：**High**；状态：已修复。
- 位置：[elec/main.js:209](elec/main.js#L209)，`did-navigate-in-page`。
- 证据：原处理器在判断 `isMainFrame` 之前调用 `syncUrl`，子框架的地址也会写入标签状态和地址栏。
- 影响：不可信嵌入框架可使主地址栏显示与当前主文档不一致的地址，破坏用户对页面身份的判断。
- 修复：仅在 `isMainFrame` 为真时更新地址及历史。
- 验证：真实 Electron 集成测试加载本地页面及 iframe，由 iframe 修改自身片段，确认主地址保持主文档地址。

### SEC-03：网页弹窗被转换成缺少协议限制的宿主导航

- 严重性：**Medium**；置信度：**High**；状态：已修复。
- 位置：[elec/main.js:216](elec/main.js#L216)、[main.cpp:317](main.cpp#L317)，两端的新窗口处理器。
- 证据：原逻辑直接将网站请求的地址传给 `createTab` / `CreateTab`，其中包括宿主原本只应由用户主动打开的本地文件地址。
- 影响：将网页请求提升为宿主主动导航，扩大不可信页面的导航范围；未确认可由此直接读取任意本地文件或执行原生代码。
- 修复：网站弹窗仅接受 HTTP/HTTPS。地址栏中的手动本地文件打开保留。重试下载及通知重试同样限制来源协议和记录状态。
- 验证：Electron 集成测试确认本地文件弹窗不会创建新标签；原版共用 Web URL 限制并完成编译检查。

### SEC-04：原版下载事件重入导致对象或遍历状态失效

- 严重性：**Medium**；置信度：**High**；状态：已修复。
- 位置：[main.cpp:747](main.cpp#L747)、[main.cpp:939](main.cpp#L939)、[main.cpp:1001](main.cpp#L1001)。
- 证据：暂停/取消操作可能触发同步状态回调；回调会移除运行时对象，而调用方继续使用原指针。退出时还会一边遍历运行时 map 一边取消，取消回调又删除该 map 中的元素。删除确认框也会处理下载消息，使此前指向记录 vector 的指针失效。
- 影响：出现悬空指针、遍历器失效、错误状态或应用崩溃。没有确认可控制的内存写入或代码执行能力。
- 修复：COM 操作在调用期间持有独立强引用；取消前复制 ID 列表并重新查找对象；对话框使用记录快照且操作前重校验；暂停依据引擎真实中断原因判断；清理前保留独立 ID。
- 验证：原生 COM 测试替身同步回调，覆盖暂停/继续、暂停后取消、多个下载同时取消，以及对象必须存活至取消返回的断言。

## 防御补强与低风险稳健性问题

### SEC-05：Electron IPC 未验证发送者，内部页面缺少 CSP

- 严重性：**Medium**（防御补强，利用需先获得发送 IPC 或内部页面脚本执行的能力）；置信度：**High**；状态：已修复。
- 位置：[elec/main.js:460](elec/main.js#L460)、[elec/src/security.js:7](elec/src/security.js#L7)、两个 `renderer/*.html` 第 5 行。
- 证据：原所有 `ipcMain` 处理器忽略发送来源，且同一个 preload 同时暴露标签与活动记录能力；内部 HTML 未设置 CSP。
- 修复：每个接口限定接收窗口、主框架和完整本地页面 URL；工具栏与活动中心拆分 preload；内部 UI 禁止导航、重定向、子框架、内联脚本和外部资源。
- 验证：另建带测试专用 IPC bridge 的窗口，即使加载同一 `activity.html` 地址，也无法读取、清空或删除记录；合法窗口仍可调用；内联脚本被 CSP 拦截；远程标签没有任何应用 bridge。
- 误报边界：原远程标签已禁用 Node、启用隔离和沙箱，不能仅凭没有 sender 校验就断言网页可直接读取历史或执行系统命令。

### SEC-06：文件操作缺少主进程确认及普通文件范围限制

- 严重性：**Low**（单独利用依赖可信 UI 脚本失陷或本地记录篡改）；置信度：**High**；状态：已修复。
- 位置：[elec/main.js:556](elec/main.js#L556)、[elec/src/security.js:26](elec/src/security.js#L26)、[main.cpp:717](main.cpp#L717)。
- 证据：Electron 删除确认仅在 renderer 中执行；存在性检查未区分文件、目录或链接；原版右键文件删除也未拒绝活动下载。
- 修复：主进程原生确认且默认取消；拒绝目录、链接、NUL、相对路径和 UNC/设备路径；拒绝活动下载文件删除；确认后重新检查记录；打开文件仅允许完成记录。
- 验证：单元测试检查路径和文件类型；Electron 集成测试拦截回收站调用，确认取消不删除、确认后只调用一次指定测试文件，目录在确认前即被拒绝。原生测试同样验证文件范围。
- 限制：普通文件检测与系统打开/回收站之间仍存在同用户进程换文件的时间窗口；未将该限制当作防御已控制当前用户账号的安全边界。

### SEC-07：结构错误但合法 JSON 的记录可使 Electron 启动失败

- 严重性：**Low**（本地资料损坏或同用户篡改场景）；置信度：**High**；状态：已修复。
- 位置：[elec/src/activity-store.js:18](elec/src/activity-store.js#L18)、[elec/src/activity-store.js:23](elec/src/activity-store.js#L23)。
- 证据：原逻辑仅检查外层数组，随后直接访问 `item.state`、日期和文件路径；数组中的 `null` 或错误类型会抛出异常。
- 修复：装载时检查记录结构、字段类型、日期、数值和状态；保留损坏原始文件的证据副本，只恢复有效记录。
- 验证：混合有效记录、null、错误路径类型和不允许的历史协议，确认不会崩溃、有效下载保留且证据副本存在。

## 验证结果与复现范围

- `node --test`：11/11 通过（活动记录、URL、IPC 信任条件、文件范围）。
- `node node_modules/electron/cli.js verification/security-smoke.cjs`：真实 Electron 进程集成检查通过。使用临时资料目录和本地 HTTP 页面，系统删除操作由替身拦截。
- `cmake --build --preset debug --parallel` 与 `ctest --test-dir build/debug -C Debug --output-on-failure`：构建成功，3/3 通过，包括真实 WebView2 已保存权限升级检查。
- Release 构建与 `ctest --test-dir build/release -C Release --output-on-failure` 同样成功，3/3 通过；原版 v1.3.1 NSIS 安装程序编译成功。
- Electron v1.3.1 安装版与便携版打包成功，ASAR 内包含独立活动中心 preload 与安全模块，不包含 verification/ 测试专用通信桥。
- 原生回归测试在 Debug/Release 均显式启用断言，避免 Release 的 `NDEBUG` 让断言检查失效。
- 按 package-lock 全部已锁定依赖版本查询 npm 官方 bulk advisory API，返回空公告集合；未将此结果等同于引擎或依赖绝无漏洞。
- 搜索并检查 DOM/脚本注入、原生桥、外部协议、Shell 调用、权限、文件路径和安装脚本。标题和 URL 通过文本节点展示，通知 XML 已转义；没有发现可确认的命令拼接注入。

## 剩余范围与使用影响

- 本次未做引擎模糊测试、Chromium 沙箱逃逸研究、第三方网站审计或完整安装/卸载和通知冷启动验收。
- 未签名程序仍会显示未知发布者提示。
- v1.3.1 默认拒绝网页敏感权限；仅通过网站请求打开的弹窗受 HTTP/HTTPS 限制；网络共享和链接路径的系统文件操作会被拒绝。
- WebView2 Evergreen 运行时仍需由系统保持更新。Electron 和构建依赖后续需要随上游安全版本维护。

## 参考资料

- [Electron 官方安全建议：IPC sender、隔离、导航与 CSP](https://www.electronjs.org/docs/latest/tutorial/security)
- [Electron webContents 导航和框架事件](https://www.electronjs.org/docs/latest/api/web-contents)
- [Microsoft WebView2 权限类型](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2)
- [Microsoft WebView2 下载状态及暂停/恢复接口](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2downloadoperation)
- [Microsoft WebView2 权限缓存与 SavesInProfile](https://github.com/MicrosoftEdge/WebView2Feedback/blob/main/specs/PermissionManagement.md)
- [Microsoft WebView2 按数据类型清理 Profile](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2profile2)
