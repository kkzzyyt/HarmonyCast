# 鸿蒙投屏工作台 · HarmonyCast 2.0

<p align="center">
  <img src="resources/app-icon.svg" width="96" height="96" alt="HarmonyCast Logo"/>
</p>

<p align="center">
  <b>面向 OpenHarmony / 鸿蒙设备的原生高性能桌面投屏、调试与系统维护工作台</b><br>
  完全基于 <b>C++20 + Qt6</b> 原生引擎重构，提供极致流畅的硬件加速投屏、实时日志流、Chrome DevTools 联动及硬件维护能力。
</p>

<p align="center">
  <a href="https://github.com/kkzzyyt/HarmonyCast/releases/latest"><img src="https://img.shields.io/github/v/release/kkzzyyt/HarmonyCast?color=5ce0c1&label=Release" alt="Latest Release"></a>
  <img src="https://img.shields.io/badge/Language-C%2B%2B20-00599C?logo=cplusplus" alt="C++20">
  <img src="https://img.shields.io/badge/Framework-Qt%206-41CD52?logo=qt" alt="Qt6">
  <img src="https://img.shields.io/badge/Platform-macOS%20%7C%20Windows-lightgrey" alt="Platform">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--3.0--or--later-blue.svg" alt="License"></a>
</p>

---

## ✨ 核心特性

### 1. ⚡ 原生 C++20 极致性能架构
- **原生实现**：采用 C++20 + Qt6；视频处理在后台执行，界面使用共享图像和有界刷新队列。
- **硬件加速解码**：支持 macOS VideoToolbox 与 Windows D3D11VA，并保留软件解码回退；实际帧率与延迟取决于设备和传输方式。
- **设备控制**：保留 HDC 触摸与按键控制，复用后台线程并按顺序调度输入。

### 2. 📱 自由分屏投屏工作台 (Workbench)
- **多形态布局**：一键无缝切换 **左右分屏**、**上下分屏**、**仅投屏**、**仅日志** 四种工作流。
- **流式触控栏**：支持动态调整画质（1.5M / 4M / 8M 码率）、0°/90°/180°/270° 画面旋转、高清截图、返回键、Home 键及 F11 全屏演示。
- **触控轨迹反馈**：内置手机轮廓与优雅微发光触控点/拖拽轨迹反馈。

### 3. 📑 实时 HiLog 日志流中心
- **分层工具栏**：采用清晰的两行功能分区排版，检索输入框自动弹性拉伸，彻底杜绝拥挤挤压。
- **多维度即时筛选**：按包名/前缀（默认 `com.yarward.*`，支持一键切换全部应用）、日志级别（DEBUG/INFO/WARN/ERROR/FATAL）与关键字全文正则即时过滤。
- **便捷字号缩放**：
  - 工具栏直观的 `[ − ] [ 11 pt ] [ + ] [ 默认 ]` 快速加减与复位按钮。
  - 全局快捷键：`⌘ +`（放大）、`⌘ −`（缩小）、`⌘ 0`（恢复 11 pt）。
  - 支持 **`⌘ / Ctrl + 鼠标滚轮`** 在日志视窗内无级平滑缩放。
- **流式导出**：支持实时日志暂停、自动滚动切换与纯文本一键导出。

### 4. 🌐 WebView 控制台与 Chrome DevTools 联动
- **双标签视图**：在工作台无缝查看设备端应用内嵌 WebView 的控制台日志（Console）与接口网络抓包（Network）。
- **交互式执行**：内置 `JS >` 控制台，支持向当前设备 WebView 直接注入并执行 JavaScript 指令。
- **网络抓包分析**：记录 URL、请求方法、状态码、响应耗时，右键支持**一键复制为 cURL 命令**。
- **一键唤起 Chrome**：点击「Chrome」按钮直接打开 Google Chrome 的 `chrome://inspect/#devices`，调试地址自动复制到剪贴板。

### 5. 🔍 全景设备信息看板 (Device Info)
- **智能未连接指引**：设备未就绪时自动呈现清晰的 USB 有线与网络调试配置向导。
- **硬件与系统卡片**：品牌厂商、设备型号、系统版本（API 级别）、Linux 内核、CPU ABI、UDID、存储与内存容量全方位展示。
- **Yarward 专属应用集成**：自动识别核心业务应用，支持一键拉起前台、强制停止、持久化日志打包拉取。
- **ICU 硬件模块与看门狗调试**：针对医疗与工控终端，支持一键暂停看门狗（调试防复位）、恢复看门狗及手动喂狗（FeedWatchdog）。
- **网络与接口清单**：枚举所有物理/虚拟网卡 MAC 地址与当前 IP。
- **全部应用包清单**：表格化列出已安装全部应用，支持即时过滤搜索与包名复制。

### 6. 💻 HDC 常用命令中心 (HDC Presets)
- **快捷控制卡片**：集成桌面、返回、电源/息屏、挂载系统分区读写（`target mount`）、安全重启、HAP 应用安装/卸载与文件收发。
- **模板与自动参数**：预置 10+ 常用命令模板；自动补全当前设备 `-t <serial>` 参数。
- **交互式终端输出**：命令实时异步执行，支持中断、清空控制台与终端日志导出。

---

## 📥 预编译版本下载

前往 [GitHub Releases](https://github.com/kkzzyyt/HarmonyCast/releases/latest) 下载对应平台的安装包：

| 平台 | 安装包格式 | 架构要求 | 说明 |
| :--- | :--- | :--- | :--- |
| **macOS** | `.dmg` | Apple Silicon (M1/M2/M3/M4) | 开箱即用，双击拖拽至 Applications 目录即可 |
| **Windows** | `.zip` | Windows 10/11 x64 | 解压后直接双击运行 `HarmonyCast.exe` |

Windows Server 2012/2012 R2 提供独立的 [兼容构建方案](docs/windows-server2012.md)（Qt5、软件解码，待服务器验证）。新增工作流生成的包名包含 `Windows-Server2012-x64`，请完整解压使用；普通 Windows 包仍要求 Windows 10/11。

> [!TIP]
> **macOS 首次打开提示“无法打开”或“不受信任的开发者”？**
> 本项目使用开源 Ad-hoc 本地签名。如遇拦截，请在系统「设置」→「隐私与安全性」中点击 **仍要打开**，或在终端执行：
> ```bash
> xattr -cr /Applications/HarmonyCast.app
> ```

---

## 🛠 源码构建指南

### 1. 系统要求
- **macOS** 14.0+ / Apple Clang 15+ 或 **Windows** 10/11 x64 / MSVC 2019+
- **CMake** 3.22+
- **Qt 6**（组件：`Core`, `Gui`, `Widgets`, `Network`, `Svg`, `SvgWidgets`, `WebSockets`）
- **FFmpeg 4.4+ / 6.0+**（包含 `libavcodec`, `libavutil`, `libswscale`）
- **HDC 命令行工具**（包含在 OpenHarmony SDK 或 DevEco Studio 中）

### 2. macOS 快速构建

```bash
# 1. 安装构建依赖
brew install cmake qt ffmpeg

# 2. 克隆仓库
git clone https://github.com/kkzzyyt/HarmonyCast.git
cd HarmonyCast

# 3. 配置与编译
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/opt/homebrew/opt/qt;/opt/homebrew/opt/ffmpeg"
cmake --build build -j$(sysctl -n hw.ncpu)

# 4. 运行验证测试套件
ctest --test-dir build --output-on-failure

# 5. 打包独立 DMG 应用
macdeployqt build/HarmonyCast.app -dmg
```

### 3. Windows 快速构建

```powershell
# 1. 使用 vcpkg 安装 FFmpeg
vcpkg install ffmpeg:x64-windows

# 2. 配置与构建 (已安装 Qt6 与 MSVC)
cmake -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release

# 3. 部署 Qt 运行依赖
windeployqt build/Release/HarmonyCast.exe --compiler-runtime
```

---

性能验证、缓存策略和本地基准命令见 [性能说明](docs/performance.md)。

后续功能、阶段路线与验收标准见 [发展规划](docs/development-roadmap.md)，全部文档入口见 [项目文档](docs/README.md)。

## ⌨️ 常用快捷键

| 快捷键 (macOS / Windows) | 功能说明 |
| :--- | :--- |
| **`F5`** | 刷新在线设备列表 |
| **`F11`** | 进入 / 退出全屏投屏演示模式 |
| **`Alt + ←`** | 模拟设备返回键 (Back) |
| **`Alt + Home`** | 模拟设备主页键 (Home) |
| **`⌘ +` / `Ctrl +`** | 放大日志字号 (+1 pt) |
| **`⌘ −` / `Ctrl −`** | 缩小日志字号 (-1 pt) |
| **`⌘ 0` / `Ctrl 0`** | 恢复默认日志字号 (11 pt) |
| **`⌘ + 滚轮`** | 鼠标滚轮实时平滑缩放日志字号 |
| **`Esc`** | 退出全屏模式 |

---

## 📜 许可证

- HarmonyCast 原生重构代码基于 **GPL-3.0-or-later** 许可证分发，详见 [LICENSE](LICENSE)。
- 上游流协议与参考实现致敬 [OpenHarmony_Scrcpy](https://gitcode.com/OpenHarmony_Tools/OpenHarmony_Scrcpy) (Apache-2.0)。
- 详细第三方开源组件声明参见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
