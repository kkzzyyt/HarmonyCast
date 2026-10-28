# HarmonyCast C++ Native (跨平台高性能原生客户端)

HarmonyCast 使用 C++20、Qt6 和 FFmpeg，支持硬件解码、软件解码与截图兼容回退。

## 性能与内存管理

- 视频拆包、解码、颜色转换和显示尺寸适配在后台线程执行；界面线程使用共享图像绘制。
- 接收侧限制单次交给工作线程的数据量，待显示队列仅保留最新一帧。H.264 压缩帧仍按顺序解码，避免破坏帧间依赖。
- 图像缓冲按所有权复用，截图保留完整源图；旋转、缩放及触控坐标有回归测试覆盖。
- 日志每 50ms 合并刷新，搜索防抖 180ms。隐藏面板继续接收，重新显示时更新内容；暂停仅冻结显示。
- HiLog 和 WebView 使用滚动缓存，去除重复原始文本；大响应体使用临时文件缓存，查看与复制保持完整内容。
- HDC 输入命令按顺序执行并复用后台线程。终端界面限制为最近 10000 行，完整输出保存在临时文件中供复制、导出，清空或退出时释放。

硬件解码后目前仍需要 CPU 颜色转换，不宣称全链路 GPU 零拷贝。实际 CPU、内存、帧率和延迟取决于设备、分辨率、连接方式及日志负载。

详细验证与限制见 [性能验证说明](docs/performance.md)。

---

## 本地编译与构建指南

### 1. macOS 编译

**前置依赖**：
```bash
brew install qt ffmpeg cmake
```

**编译命令**：
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**运行测试**：
```bash
ctest --test-dir build --output-on-failure
```

**打包为独立 DMG 镜像**：
```bash
macdeployqt build/HarmonyCast.app -dmg
# 生成产物：build/HarmonyCast.dmg
```

---

### 2. Windows 编译 (MSVC x64)

以下是 Qt6 普通版构建。Windows Server 2012/2012 R2 使用独立的 [Qt5 兼容构建](docs/windows-server2012.md)，开启 `HARMONYCAST_SERVER2012_COMPAT` 并部署专用 FFmpeg 和 v142 / UCRT 运行库。

**前置依赖**：
- Visual Studio 2022 (带 C++ 桌面开发套件)
- CMake 3.22+
- Qt 6.6+ (MSVC 2019/2022 64-bit)
- FFmpeg (通过 vcpkg 安装: `vcpkg install ffmpeg:x64-windows`)

**编译命令**：
```cmd
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**打包 Windows 独立发行包**：
```cmd
windeployqt build\Release\HarmonyCast.exe --compiler-runtime
# 将 build\Release 目录压缩为 HarmonyCast-Windows-x64.zip 即为绿色便携版
```
