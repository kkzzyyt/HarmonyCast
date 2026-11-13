# Android 8+ 设备支持

支持 Android 8.0 / API 26 及以上设备，使用 ADB 管理连接，使用固定版本的 scrcpy-server 采集 H.264 视频和接收触控。画面显示、硬件/软件解码、旋转、全屏和日志界面与鸿蒙设备共用。

## 连接设备

### USB

1. 在设备中开启开发者选项和 USB 调试，通过数据线连接电脑。
2. 在设备上确认调试授权，回到 HarmonyCast 按 F5 刷新。
3. 在顶部设备列表选择带有 Android 标识的设备，点击「开始投屏」。未授权或离线设备会保留对应状态。

Windows 还需要设备厂商提供的 USB 调试驱动。标准 Windows 10+ 和 macOS 安装包随包提供 ADB，无需安装 Android Studio。

### 局域网 / 网口

在「设备中心」选择 Android，输入设备 IP 和调试端口，默认端口为 5555。端口可以自定义；保存记录会区分 Android 和鸿蒙，旧版本记录默认仍属于鸿蒙。

Android 8 普通设备通常需要先通过 USB 开启网络调试：选中 USB 设备，在「常用命令」中使用「ADB 命令」模式执行 `tcpip 5555`，然后在设备中心连接 IP 和端口。已开放网络 ADB 的工控终端可直接连接。

此版本提供传统 ADB TCP 连接。Android 11+ 的配对码接入界面尚未提供。

## 功能对应

| 功能 | Android 行为 |
| --- | --- |
| 投屏 | 内嵌 H.264 视频、画质档位、旋转、全屏；采屏失败时尝试低帧率 PNG 截图回退 |
| 输入 | 点击、拖动、返回、Home、电源键；视频控制通道不可用时使用 ADB 输入命令 |
| 截图 | 优先保存当前画面，无画面时直接读取设备 PNG 截图 |
| 设备日志 | Logcat，支持现有级别、关键词、暂停显示和导出；定期解析 PID 与包名关系 |
| 设备信息 | 型号、Android/API 版本、CPU、内存、存储、网络、包清单和业务应用版本 |
| 应用与文件 | APK 安装/卸载、应用启动/停止、推送/拉取文件；清除应用数据前明确提示并确认 |
| WebView | 发现已开启调试的 WebView，转发设备 socket 后使用现有 CDP 控制台与网络查看功能 |
| 批量巡检 | 选择 Android 后导入 IP 列表，默认 5555；逐台连接、采集、断开并导出 CSV |

Android 的应用数据清除会删除本地数据与登录信息，区别于鸿蒙的缓存清理。鸿蒙专用 ICU 看门狗操作在 Android 设备上不可用。应用未开启 WebView 调试时无法发现对应页面。

## 随包工具与主机兼容

- scrcpy-server 固定为 **5.0.1**，运行前验证 SHA-256；视频协议与该版本配套，不自动替换为其他版本。
- ADB 使用 Google Platform Tools **37.0.1**，macOS 包含 Intel / Apple Silicon 通用程序，Windows 标准包包含官方独立 ADB 进程及其相邻 DLL。
- Server 2012 兼容版不携带当前 ADB；需要在系统中安装适用于该主机的 ADB，通过 PATH 或 `HARMONYCAST_ADB` 指定。Android 在 Server 2012 上尚未进行运行验收。
- 工具查找顺序：`HARMONYCAST_ADB` → 当前平台随包工具 → PATH → Android SDK 常见目录。
- 来源、许可证和原始文件校验值见 `resources/adb/README.md`、各平台 `NOTICE.txt`、`resources/adb/checksums.json` 和 `resources/servers/android/README.md`。

连接失败时先检查设备授权、USB 驱动、ADB 端口和网络连通性。投屏状态会区分启动、兼容回退成功、权限失败和截图超时；详细服务端输出保留在诊断日志。

## 验证范围

自动化测试使用独立的模拟 ADB 进程、本地 TCP 视频/控制通道和真实 H.264/PNG 数据，覆盖设备身份与状态、协议分片、旋转会话、输入编码、启动连接重试、截图回退、超时、停止/销毁、日志、设备切换和 Android 巡检。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p test_windows_runtime.py
```

当前开发环境没有 Android 真机。USB 驱动、具体设备的编码器、系统触控权限、实体屏幕旋转、长时间运行和 Windows 主机运行情况仍需真机验收，自动化通过不代表这些项目已验收。

上游资料：[scrcpy](https://github.com/Genymobile/scrcpy)、[5.0.1 协议说明](https://github.com/Genymobile/scrcpy/blob/v5.0.1/doc/develop.md)、[Android ADB 文档](https://developer.android.com/tools/adb)。
