# Changelog / 更新日志

All notable changes to the HarmonyCast project are documented in this file.
本项目的所有重要版本更新均记录在此文件中。

Format adheres to [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and follows [Semantic Versioning](https://semver.org/).
格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，并遵循 [语义化版本规范](https://semver.org/lang/zh-CN/)。

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
