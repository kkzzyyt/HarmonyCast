# 鸿蒙投屏 · HarmonyCast

[English](README.en.md) · [贡献指南](CONTRIBUTING.md) · [第三方组件](THIRD_PARTY_NOTICES.md)

面向 **macOS 14+ / Apple Silicon** 的 OpenHarmony 桌面调试工具：在一个窗口中完成投屏、触控、HiLog 筛选、设备信息查看和 HDC 调试。

本项目基于现有 HarmonyCast 1.4.1 工作区整理首次公开源码。视频协议、解码器和部分设备服务来自 [OpenHarmony_Scrcpy](https://gitcode.com/OpenHarmony_Tools/OpenHarmony_Scrcpy)，保留原作者署名与 Apache-2.0 许可证。HarmonyCast 的 Qt 界面、日志工作流及 `native/` 下的 RISC-V 采屏服务在本仓库维护。

## 功能

| 模块 | 能力 |
| --- | --- |
| 投屏 | OHScrcpy H.264/H.265 视频、RISC-V 原生连续采屏、兼容截图模式 |
| 操作 | 点击、滑动、返回/主页/音量按键、旋转、分辨率预设、全屏、截图 |
| HiLog | 独立日志页、按包名或命名空间筛选、多进程跟踪、级别配色、搜索和纯文本导出 |
| 设备 | USB/网络连接、多设备选择、系统/网络/应用信息、应用启动和停止 |
| 排障 | HDC 命令面板、持久化应用日志导出、故障报告导出 |

包含面向特定开发板的 ICU/看门狗扩展；是否可用取决于设备驱动和调试权限。日志页沿用原工作区的 `com.yarward.*` 默认筛选，可在包名框中选择「全部应用」或输入自己的包名/命名空间。

## 从源码运行

需要 Python 3.12+、macOS 14+ 和 HDC。通过官方 HarmonyOS/OpenHarmony SDK 或 DevEco Studio 获取 HDC，开启设备的 USB 调试并授权电脑。

```bash
git clone https://github.com/kkzzyyt/HarmonyCast.git
cd HarmonyCast
python3.12 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python -m harmonycast
```

应用会查找常见 DevEco Studio 安装位置和 PATH，也可以在「设置」中指定 HDC。预编译 HDC/libusb 不提交到此仓库，具体说明见 [resources/hdc/README.md](resources/hdc/README.md)。

1. 在顶部刷新并选择设备。
2. 选择合适的投屏方式，点击「开始投屏」。
3. 切换到「日志」页自动采集；离开日志页会释放采集进程，投屏继续运行。
4. F11 或 Control+Command+F 切换全屏，Esc 返回。

应用可能沿用本机已有设置，包括自动开始投屏。日志自动保存在 `~/Library/Application Support/HarmonyCast/logs/`，采用大小轮转。导出的是当前缓存中符合筛选条件的内容。

## 设备兼容性

- **RISC-V 开发设备**：使用 `native/` 采屏程序，通过公开 OpenHarmony C API 和设备编码器传输 H.264；不具备编码能力时可回退 RGBA。需要对应 API、系统库及采屏权限。
- **ARM 开发设备**：复用上游服务。内含 RK3568 ARM32 与上游 HarmonyOS 5.0.1/6.0/6.1 ARM64 服务，必须与设备系统接口及权限匹配。
- **兼容截图模式**：依赖设备的 `snapshot_display`，刷新率较低。
- **商用设备**：能否采屏和控制由系统的实际调试权限决定，不保证普通商用固件兼容。

上游 ARM 服务的「部署」操作会安装到设备系统目录。RISC-V 连续采屏使用 `/data/local/tmp/` 的临时服务。请在自己的开发设备上按实际系统选择模式。

此首次公开版本未在本次发布流程中进行真机兼容性或性能复测，不将旧工作区的测试截图与测速数据作为公开验证结果。

## 自动测试

```bash
.venv/bin/python -m pip install -r requirements-dev.txt
QT_QPA_PLATFORM=offscreen .venv/bin/python -m pytest -q --cov --cov-report=term-missing
```

测试包含 HDC 参数与错误处理、日志筛选/导出、流协议边界、真实 H.264 编解码与本地 TCP 集成、无界面 Qt 用户流程。自动测试不需要连接设备。覆盖率配置统计核心模块，单独排除了界面和上游代码；界面仍有功能测试。

`scripts/smoke_device.py`、`smoke_logs.py` 和 `benchmark_capture.py` 是手动真机脚本，需要自行连接开发设备，会生成本地日志/截图，不在 CI 中运行。这些输出已加入 `.gitignore`。

## 本地打包

```bash
.venv/bin/python -m pip install -r requirements-dev.txt
./scripts/build_macos.sh
```

生成 `dist/HarmonyCast.app`、macOS ZIP 和 SHA-256。此脚本面向 Apple Silicon，产物使用本机临时签名，未做 Developer ID 签名与公证。首次公开发布提供源码；不上传原工作区的 app、设备日志和截图。

RISC-V 服务源码位于 `native/`，重新编译：

```bash
HARMONYCAST_SDK=/path/to/sdk/openharmony/native \
  ./scripts/build_riscv_capture.sh /path/to/device-link-libraries
```

链接库包括 `libc.so`、`libnative_avscreen_capture.so`、`libnative_media_core.so`、`libnative_media_venc.so`、`libnative_media_codecbase.so`、`libnative_buffer.so` 和 `libnative_display_manager.so`，需要与目标设备匹配。设备系统库不随仓库分发。

## 贡献与许可

欢迎通过 [Issues](https://github.com/kkzzyyt/HarmonyCast/issues) 报告可重现问题，或提交文档、测试和兼容性改进。参见 [CONTRIBUTING.md](CONTRIBUTING.md) 与 [SECURITY.md](SECURITY.md)。

HarmonyCast 自有代码采用 **GPL-3.0-or-later**，见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。上游组件保留各自许可证；参见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 `licenses/`。
