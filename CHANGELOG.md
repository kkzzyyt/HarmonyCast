# Changelog / 更新日志

All notable changes to the HarmonyCast project are documented in this file.
本项目的所有重要版本更新均记录在此文件中。

Format adheres to [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and follows [Semantic Versioning](https://semver.org/).
格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，并遵循 [语义化版本规范](https://semver.org/lang/zh-CN/)。

---

## [Unreleased]

---

## [2.4.1] - 2026-11-13

### Fixed / 修复
- [鸿蒙投屏] 兼容截图增加 `uitest screenCap` PNG 通道，在 `snapshot_display` 不可用或失败时自动尝试；按实际图片内容识别 JPEG/PNG，JPEG 插件缺失时可使用 PNG。
- [Windows] 兼容截图优先通过 HDC shell 的 Base64 文本传输，失败后尝试文件传输，记住可用通道，减少中文临时目录、文件锁和文件接收失败的影响。
- [恢复] 首张兼容截图使用启动超时预算，后续画面使用帧超时；每个会话使用独立临时文件，校验图片完整性、尺寸和文本输出上限，防止旧画面和异常数据误报成功。
- [验证] 增加 PNG、文本传输、Toybox、无 JPEG 插件、慢速首帧及损坏数据回归；实际故障 Windows 电脑和设备仍需复测。

---

## [2.4.0] - 2026-11-13

### Added / 新增
- [批量清理] 设备巡检新增应用数据清理任务，可勾选床旁、病房与 A33 门口控制三个指定包；复用逐台连接/断开逻辑，按 `bm clean -d -n` 执行，支持未安装跳过、逐包结果、停止及 CSV 导出。
- [Android] 接入 Android 8+ 的 ADB 设备发现、USB/网络连接、内嵌 H.264 投屏、触控与截图回退；与鸿蒙设备按平台隔离身份和保存记录。
- [工作台] Android 使用 Logcat、APK、应用/文件命令、WebView 调试与 ADB 批量巡检；Windows/macOS 标准包随包提供 ADB 和固定版本采屏服务。
- [验证] 增加模拟 ADB、真实视频解码、启动时序、生命周期和跨平台切换回归；当前尚无 Android 真机验收结果。

### Fixed / 修复
- [状态] 批量清理右侧表格随任务、IP 和包选择即时更新，实时显示连接、查询、逐包清理和断开状态，保留上次结果导出。
- [构建] 修正 Windows 重复资源目录、Qt5 视频帧注册与测试类型名称；macOS 仅部署所需 Qt 插件，并检查部署错误。
- [日志] 增量显示刷新由 50ms 放缓为 500ms，减少连续滚动和界面更新频率，采集、缓存与手动操作保持原有行为。
- [投屏] 原生采屏缺库或缺 API 时显示简短兼容提示，详细启动错误保留在诊断日志，收到首张兼容截图后明确提示回退成功。
- [兼容] 截图命令与文件传输检查 HDC 的失败文本，防止退出码为 0 的错误和旧图片误报成功；截图缩放参数不受支持时改用原始尺寸。
- [诊断] 区分截图命令、权限、传输和图片解码失败，明确的命令缺失或权限拒绝立即结束，其他失败保留重试和超时保护。

---

## [2.3.4] - 2026-11-12

### Changed / 变更
- [工作台] 默认仅投屏；顶部“投屏”“日志”可独立选择，双选默认左右分屏，在日志区域右上角切换左右/上下。打开日志区域自动开始 HiLog 采集，隐藏日志时停止采集并保留历史内容。

### Fixed / 修复
- [交互] 至少保留一个工作区；从仅日志重新勾选投屏立即恢复投屏，调整分屏方向时保留手动暂停采集的选择。

---

## [2.3.3] - 2026-11-12

### Changed / 变更
- [界面] 调整为中性雾灰与低饱和灰蓝，投屏空白画布及视频边缘跟随主题。
- [设备] 主窗口设备下拉固定高度，支持 IP、端口、名称及状态搜索；与设备中心共用设备目录和筛选规则，区分同 IP 不同端口，移除设备中心的 USB 无线端口及 HDC 重启按钮。
- [巡检] 移除独立设备 IP 列与 `/data` 使用率，型号读取 ICU/看门狗硬件型号，界面与 CSV 保持一致。
- [命令] 自定义设备命令默认添加 `hdc shell`，模板自动选择 Shell 或 HDC 主机命令模式。
- [日志] 分屏无线日志默认关闭，进入仅日志布局自动采集，三个开关增加间距并可换行。
- [更新] 默认使用中文 GitHub API 中转，失败后回退官方源；保留自定义更新地址。

### Fixed / 修复
- [兼容] 加固 Qt5 设备选择器模型与弹窗的销毁顺序，Windows 编译显式使用 UTF-8。
- [投屏] 软件模式同时禁用硬件视频解码；增加首帧与画面停滞超时、采屏启动检查、截图兼容回退及阶段错误日志，故障 Windows 设备仍需实机验证。
- [Windows] 增强 RemoteFX 远程会话检测及崩溃后软件模式恢复；HDC、日志、巡检统一进程环境，支持中文/空格路径，并补充日志进程启动与退出错误。
- [解码] 硬件解码或帧传输持续失败后安全切换 CPU，保留视频参数并等待关键帧，避免在解码回调中重建正在使用的上下文。

---

## [2.3.1] - 2026-11-11

### Changed / 变更
- [界面] 重新设计主窗口和巡检页，采用浅灰蓝背景、白色卡片、蓝色主操作及清晰的文字层级；完整显示侧栏品牌，减少重复边框并提升表格可读性。
- [滚动] 使用独立自绘滚动条，统一横纵滑块、16px 操作区域、悬停过渡、按下状态和轨道定位，保留键盘及辅助功能支持。
- [滚动] 表格改为逐像素滚动，支持触控板双向滚动及 Shift 横向滚动。

### Fixed / 修复
- [启动] 设备发现通知在释放设备列表锁后发出，修复回调重入读取设备列表时造成的界面死锁；窗口显示后再启动设备轮询和后台检查。
- [交互] 日志追加和重新渲染时保留手动阅读位置，拖动滑块时不强制追踪末尾，并修复日志视口的触控板字号缩放。
- [验证] 增加死锁隔离探测、慢 HDC 启动、滑块绘制与拖动、像素滚动及阅读位置回归；macOS 发布流程额外运行 Cocoa 原生窗口验证。

---

## [2.3.0] - 2026-11-11

### Added / 新增
- [巡检] 新增独立批量巡检区域，支持 IP TXT 导入、地址校验去重和自定义 HDC 端口，逐台连接、采集并断开当前设备。
- [巡检] 采集运行时长、IP/MAC、内存、存储及系统/CPU 信息；支持进度显示、中途停止和 CSV 表格导出，记录缺失字段及连接、采集、断开失败原因。
- [巡检] 增加模拟 HDC 的导入、解析、导出、逐台连接清理、超时、停止、窗口关闭和界面回归测试。
- [发布] macOS、Windows 和 Server 2012 自动构建均执行完整 CTest 回归，包含批量巡检与连接测试，通过后由版本标签触发安装包发布。

### Fixed / 修复
- [连接] 使用 HDC 详细设备列表，按状态列区分已连接、未授权和离线，避免设备名称干扰状态识别。
- [连接] 保留连接失败的原始 HDC 输出和无线调试授权指引，刷新设备列表后仍可查看失败原因；投屏区域按提示长度排版，避免错误详情被裁切。
- [连接] 保留自定义无线端口，优先选择已连接设备，防止离线轮询触发反复重连，并保持 USB / 无线断连防抖。

---

## [2.2.5] - 2026-11-07

### Highlights / 核心亮点
- **Hermetic Windows Runtime**: Bundled full Microsoft Visual C++ CRT (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) and UCRT dependencies directly into Windows Release packages, completely eliminating startup failures on clean Windows machines.
- **Windows 全环境零依赖加固**：通用 Windows 发行包全面内嵌 MSVC 核心运行时与 UCRT 基础库，彻底终结纯净 Windows 系统因缺少 VC 运行库导致的直接闪退与打不开问题。
- **Zero-Black-Screen Auto-Recovery**: Automatically detect Remote Desktop (RDP) sessions, previous crash states, or GPU D3D11/12 initialization failures, seamlessly falling back to safe software rasterization.
- **黑屏自愈与安全渲染**：自动检测远程桌面（RDP）、异常崩溃历史与 GPU 交换链状态，自适应回退至软件光栅化，杜绝窗口全黑。
- **Wi-Fi Multi-Device Anti-Flapping**: Implemented a 3-cycle debounce mechanism for network devices and decoupled streaming session lifetime from polling results, eliminating false disconnections in complex hospital Wi-Fi subnets.
- **无线多设备防抖抗抖动**：引入 3 周期丢帧防抖算法，并将底层视频流长连接与后台轮询彻底解耦，彻底解决局域网多设备并发下的偶发误断连与视频流强杀问题。
- **CI Runtime Verification Gate**: Integrated automated PE binary inspection (`check_windows_runtime.py`) into GitHub Actions CI pipeline, strictly blocking any build artifact with missing runtime libraries.
- **CI 运行时自动化守门人**：引入二进制依赖自动化校验门禁，若打包缺失任何关键 DLL 直接阻断发布。
- **NSIS Argument Forwarding**: Portable single-file executable now forwards all command-line arguments (such as `--software-rendering`) to `HarmonyCast.exe`.
- **单文件参数透传**：便携单文件 EXE 现已完整透传所有启动参数，支持直接附加 `--software-rendering` 安全模式运行。

### Fixed / 修复
- [Network] Prevented `MainWindow::onDeviceDisconnected` from prematurely terminating active `StreamSession` instances when TCP video connection is healthy.
- [网络] 修复后台目标扫描短暂漏查导致活跃投屏被意外强制终止的问题。
- [Stream] Improved capture server handshake retry logic with 24-attempt backoff window, preventing spurious fallback to slow snapshot mode.
- [推流] 增加采屏服务端启动阶段平滑重试窗口（24次约7.2秒），防止首次握手瞬间误降级为慢速快照流。
- [CI] Added `ilammy/msvc-dev-cmd@v1` and `deploy_windows_runtime.ps1` to standard Windows build job, ensuring `windeployqt` and deployment scripts find MSVC toolsets.
- [CI] 修复 Windows 构建流水线缺失 MSVC 编译环境变量的问题，确保 `windeployqt` 正确识别工具链并完整打包运行时。
- [Device] Enhanced ICU / MCU firmware information extraction across diverse sysfs naming conventions (`Version`, `HardwareVersion`, `StopDelay`).
- [设备] 优化 ICU/MCU 固件节点解析，自适应匹配大小写与驼峰命名，提升病房终端硬件版本识别准确率。

---

## [2.2.4] - 2026-10-29

### Highlights / 核心亮点
- **Stability First Re-architecture**: Eliminated idle disconnection timer and Wi-Fi sensitivity tripwires to ensure rock-solid continuous mirroring.
- **稳定性第一架构重构**：全面移除闲置自动断连与网络抖动假离线逻辑，保障长连接持续稳定投屏。
- **Modern Windows Compatibility**: Resolved black screen issues on Windows 10/11/12, purged stub DLL pollution, embedded native application manifest, and enforced IPv4 loopback connectivity.
- **Windows 全平台兼容优化**：彻底解决 Win10/11/12 启动黑屏与连接拒绝问题，剔除多余桩模块，注入原生应用清单，全面锁定 IPv4 回环连接。
- **macOS Crash Prevention**: Fixed `SIGSEGV at 0x8d` during asynchronous stream teardown by standardizing lifecycle cleanup and decoupling signal dispatch.
- **macOS 稳定性加固**：重构推流会话退出与信号派发时序，彻底解决异步析构触发的 `SIGSEGV 0x8d` 崩溃。

### Added / 新增
- [Core] Periodic keep-awake heartbeat (25-second interval) automatically wake devices and prevent Wi-Fi NIC sleep.
- [核心] 增加 25 秒保活唤醒心跳机制，定时向设备发送保活指令，防止手机休眠锁屏后无线网卡切入节电模式断流。
- [Windows] Official Windows application manifest (`HarmonyCast.manifest`) declaring compatibility for Windows 10, Windows 11, and future Windows 12, with PerMonitorV2 DPI awareness and long path support.
- [Windows] 引入标准 Windows 应用程序清单（`HarmonyCast.manifest`），显式声明兼容 Win10/11/12，支持 PerMonitorV2 高清缩放与长路径。
- [Decoder] Automatic 3-strike failover from hardware GPU decoding (D3D11VA/VideoToolbox) to multi-threaded CPU software decoding on frame transfer errors, preventing persistent black screens.
- [解码] 新增显存帧传输故障 3 次自动降级 CPU 软解机制，即使特定显卡驱动或虚拟机缺失硬件加速，也不会出现持续黑屏。
- [Snapshot] A/B alternating double-buffering for snapshot fallback mode with JPEG EOI (`0xFF, 0xD9`) validation, eliminating Windows Defender sharing locks (`ERROR_SHARING_VIOLATION`) and corrupt frames.
- [快照] Snapshot 兼容模式采用 A/B 临时文件双缓冲轮替与 JPEG 头尾标记校验，彻底杜绝防病毒软件独占文件锁冲突与残帧解析异常。

### Changed / 变更
- [Core] Default idle timeout set to `0` (never auto-disconnect); user preference persisted.
- [核心] 闲置超时断开默认值调整为 `0`（永不断开），保留设置持久化能力。
- [Device] Polling interval adjusted to 2500ms; DeviceManager requires 3 consecutive query failures before reporting devices offline, smoothing out transient Wi-Fi jitters.
- [设备] 设备轮询间隔调整为 2500ms，需连续 3 次确认失败才判定设备离线，平滑过滤网络抖动。
- [Network] Local socket binding and connection targets strictly forced to IPv4 `127.0.0.1`, bypassing RFC 6724 IPv6 (`::1`) loopback connection refusals on Windows 11/12.
- [网络] 本地套接字探测与长连接强制锁定为 IPv4 `127.0.0.1`，杜绝 Win11/12 因 IPv6 回环拒绝引发的连接失败。
- [HiLog] Background HiLog capture automatically disabled for wireless network sessions unless explicitly opened in log panel, prioritizing all bandwidth for video stream and touch events.
- [日志] 无线投屏期间默认阻断后台 HiLog 日志流，仅在用户展开日志面板时按需懒加载，确保带宽优先供给画面与触控。

### Fixed / 修复
- [Build] Isolated `d3d12_stub` compilation exclusively to `HARMONYCAST_SERVER2012_COMPAT=ON`. Standard Windows builds no longer bundle a fake `d3d12.dll` that hijacked system DirectX 12.
- [构建] 严格隔离 `d3d12_stub` 桩模块，仅在 Server 2012 兼容模式编译；常规 Windows 安装包全面剔除桩库，直通原生 DirectX 显卡驱动。
- [Process] Remote capture server startup command wrapped with `nohup ... >/dev/null 2>&1 &`, preventing `hdc shell` from hanging for 3000ms until timeout.
- [进程] 设备端服务端启动命令使用 `nohup ... >/dev/null 2>&1 &` 彻底分离，解决此前因管道未关闭导致 3 秒超时挂起的问题。
- [Windows] HDC process execution configured with `CREATE_NO_WINDOW (0x08000000)`, eliminating console window flickering.
- [Windows] HDC 命令行调用注入 `CREATE_NO_WINDOW` 标记，消除高频执行时的控制台黑框闪烁。
- [Windows] Resolved MSVC duplicate manifest collision (CVT1100 / LNK1123) by feeding the manifest through CMake target sources and guarding resource file embedding.
- [Windows] 修复 MSVC 链接器清单资源冲突报错（CVT1100 / LNK1123），改为由 CMake 原生传递 manifest 并规避资源文件重复编译。
- [macOS] Signal `streamFinished` connected via `Qt::QueuedConnection` and stream stopped prior to notification, preventing self-destruction crashes during active stack unwinding.
- [macOS] `streamFinished` 信号连接升级为 `Qt::QueuedConnection` 并重构停止时序，杜绝会话销毁时的空指针与段错误崩溃。

---

## [2.2.3] - 2026-10-29

### Highlights / 核心亮点
- Built-in cross-platform HDC execution toolchains for zero-dependency out-of-the-box operation.
- 内置全平台 HDC 运行工具链，无需用户安装 DevEco Studio 或配置环境变量，实现 0 依赖开箱即用。

### Added / 新增
- Bundled platform-native HDC binaries in `resources/hdc/` for macOS, Windows, and Windows Server 2012.
- 在 `resources/hdc/` 中内置适配 macOS、Windows 及 Windows Server 2012 的专用 HDC 二进制与依赖。

---

## [2.2.2] - 2026-10-28

### Highlights / 核心亮点
- Unified default port 8710 for wireless HDC connections and introduced Windows Server 2012 compatibility mode.
- 全局统一无线连接默认端口 8710，新增 Windows Server 2012 / 2012 R2 专用兼容构建。

### Added / 新增
- Dedicated `HARMONYCAST_SERVER2012_COMPAT` CMake option using Qt 5.15, MSVC v142, and downlevel UCRT runtimes.
- 新增 `HARMONYCAST_SERVER2012_COMPAT` 构建通道，采用 Qt 5.15、MSVC v142 与下层兼容运行库。
- Automated PE architecture and import table verification test suite (`check_server2012_runtime.py`).
- 新增 PE 二进制架构与导入表合规性自动化校验套件。

### Changed / 变更
- Automatic suffixing of port `:8710` for plain IP device targets.
- 纯 IP 设备目标自动规范化补齐 `:8710` 默认调试端口。

---

## [2.2.1] - 2026-10-28

### Highlights / 核心亮点
- Optimized wireless device diagnostics and network connection stability.
- 优化无线设备网络连接稳定性与连接状态检测诊断。

---

## [2.2.0] - 2026-10-27

### Highlights / 核心亮点
- Windows single-file portable executable and standalone macOS DMG distributions.
- 提供 Windows 单文件绿色便携版 EXE 与 macOS 独立分发 DMG 镜像。

### Added / 新增
- NSIS single-file portable packaging script extracting to temporary directories with automatic cleanup.
- 新增 NSIS 单文件便携版打包脚本，支持临时目录解压与退出自动清理。

---

## [2.0.0] - 2026-10-26

### Highlights / 核心亮点
- Full native rewrite from Python to C++20 and Qt 6.
- 从 Python 全面使用 C++20 + Qt6 进行原生重构。

### Added / 新增
- Native OpenHarmony video demuxing, decoding, and rendering pipeline.
- 原生 OpenHarmony 视频流解包、硬件解码与双缓冲呈现管线。
- Hardware accelerated decoding via macOS VideoToolbox and Windows D3D11VA.
- 支持 macOS VideoToolbox 与 Windows D3D11VA 硬件加速解码。
- Low-latency touch and keyboard binary injection (< 2ms response).
- 二进制协议触控与按键注入通道（端到端延迟压至 2ms 以内）。
- Multi-threaded HiLog parsing and streaming workbench with Chrome DevTools inspection.
- 多线程 HiLog 日志流解析器与 WebView Chrome DevTools 联动工作台。

---

## [1.5.0] - 2026-10-24

### Highlights / 核心亮点
- Enhanced log viewing ergonomics with font scaling controls and mouse wheel zooming.
- 增强日志浏览体验，支持快捷字号缩放与鼠标滚轮无级缩放。

---

## [1.4.1] - 2026-10-23

### Highlights / 核心亮点
- Initial public open-source release of HarmonyCast.
- HarmonyCast 首个公开发行版本。
