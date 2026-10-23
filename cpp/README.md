# HarmonyCast C++ Native (跨平台高性能原生客户端)

HarmonyCast 纯 C++20 + Qt6 + FFmpeg 硬件加速原生客户端，具备全链路硬件零拷贝、超低延迟触控以及微秒级日志解析性能。

---

## 架构优势

1. **显存级零拷贝渲染**：
   - macOS: 集成 **VideoToolbox** 硬件解码，直通 `CVPixelBuffer` 显存句柄渲染，**CPU 占用 < 3%**。
   - Windows: 集成 **Direct3D 11 / DXVA2** 硬件解码，原生显卡加速。
2. **极速二进制触控通道**：
   - 通过原生 `ControlChannel` 注入触控与按键包，**端到端触控延迟压至 2ms 以内**。
3. **极小安装包与秒开体验**：
   - 二进制体积仅约 **400KB**，macOS Standalone 打包约 **50MB**（含全套 Qt 与 FFmpeg 依赖）。
   - 冷启动耗时 **< 150ms**。

---

## 本地编译与构建指南

### 1. macOS 编译

**前置依赖**：
```bash
brew install qt ffmpeg cmake
```

**编译命令**：
```bash
cd cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**运行测试**：
```bash
./build/test_harmonycast_core
```

**打包为独立 DMG 镜像**：
```bash
macdeployqt build/HarmonyCast.app -dmg
# 生成产物：build/HarmonyCast.dmg
```

---

### 2. Windows 编译 (MSVC x64)

**前置依赖**：
- Visual Studio 2022 (带 C++ 桌面开发套件)
- CMake 3.22+
- Qt 6.6+ (MSVC 2019/2022 64-bit)
- FFmpeg (通过 vcpkg 安装: `vcpkg install ffmpeg:x64-windows`)

**编译命令**：
```cmd
cd cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**打包 Windows 独立发行包**：
```cmd
windeployqt build\Release\HarmonyCast.exe --compiler-runtime
# 将 build\Release 目录压缩为 HarmonyCast-Windows-x64.zip 即为绿色便携版
```
