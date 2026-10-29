# Windows Server 2012 / 2012 R2 兼容构建

普通 Windows 发行包使用 Qt6，要求 Windows 10/11。Server 2012/2012 R2 属于 Windows 8/8.1 内核系列；普通包可能在进入 `main()` 前因 QtGui 依赖 `d3d12.dll` 而退出，不能通过设置软件渲染环境变量修复。

项目新增独立的 `HARMONYCAST_SERVER2012_COMPAT` 构建选项，目标为带桌面环境的 Server 2012/2012 R2 x64。该构建使用 Qt 5.15.2、MSVC v142、Windows SDK 10.0.19041.0 的下层兼容运行库，以及自行编译的 FFmpeg 6.1.3。视频默认使用 CPU 解码，窗口使用 QWidget 光栅绘制，保留投屏、截图、触控、日志和 WebView 调试功能。

这是兼容构建目标，尚需在目标服务器验证启动、HDC 连接及实际投屏；GitHub 的 Windows 2022 runner 不能代替 Server 2012 真机验证。客户端已内置兼容 Windows Server 2012 / 2012 R2（子系统版本 6.0，仅依赖 UCRT）的 HDC 命令行及配套动态库（`resources/hdc/hdc.exe` 与 `resources/hdc/libusb_shared.dll`），开箱即用，无需预装 DevEco Studio 或配置环境变量。

## 自动构建

`.github/workflows/build_native.yml` 的 `build-windows-server2012` 作业会独立生成：

- `HarmonyCast-<版本>-Windows-Server2012-x64.exe`：单文件便携包。
- `HarmonyCast-<版本>-Setup-Server2012-x64.exe`：安装包。
- `HarmonyCast-<版本>-Windows-Server2012-x64.zip`：完整解压版。

兼容版按包名选择同一版本线的更新资源；远端没有兼容包时提供发行页面，不自动选择普通 Windows 或 macOS 包。这些文件需要运行新增工作流后才会产生，既有发行包不受本地源码修改影响。

## 手动构建

在 Windows 构建机安装 VS 2022 的 **MSVC v142 (14.29)** 工具集、Windows SDK **10.0.19041.0**、CMake、Qt **5.15.2 MSVC 2019 x64**（包含 Svg、WebSockets）及 MSYS2（make、nasm、diffutils）。使用 v142 的 x64 开发者环境，让 MSYS2 继承其环境变量。

在 MSYS2 中构建 FFmpeg（脚本将 `_WIN32_WINNT` 设为 `0x0602`，禁用硬件解码和外部依赖，仅启用 H.264 解码及颜色转换）：

```bash
git clone --depth 1 --branch n6.1.3 https://github.com/FFmpeg/FFmpeg.git ffmpeg-server2012-src
bash scripts/windows/build_server2012_ffmpeg.sh \
  "$PWD/ffmpeg-server2012-src" "$PWD/.deps/ffmpeg-server2012" "$PWD/ffmpeg-server2012-build"
```

回到继承同一开发者环境的 PowerShell，设置实际 Qt 路径后执行：

```powershell
$env:QT_ROOT_DIR = 'C:\Qt\5.15.2\msvc2019_64'
$env:FFMPEG_DIR = "$PWD\.deps\ffmpeg-server2012"
cmake -B build-server2012 -A x64 -T v142 `
  -DCMAKE_SYSTEM_VERSION=10.0.19041.0 `
  -DHARMONYCAST_SERVER2012_COMPAT=ON `
  -DCMAKE_PREFIX_PATH="$env:QT_ROOT_DIR" -DFFMPEG_DIR="$env:FFMPEG_DIR"
cmake --build build-server2012 --config Release
& "$env:QT_ROOT_DIR\bin\windeployqt.exe" build-server2012\Release\HarmonyCast.exe --release --no-translations --no-compiler-runtime
Copy-Item "$env:FFMPEG_DIR\bin\*.dll" build-server2012\Release\
./scripts/windows/deploy_server2012_runtime.ps1 -Destination build-server2012/Release
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$env:QT_ROOT_DIR\plugins"
ctest --test-dir build-server2012 -C Release --output-on-failure
python -m pip install -r scripts/windows/requirements-build.txt
python -m unittest discover -s tests -p test_windows_runtime.py
python scripts/windows/check_server2012_runtime.py build-server2012/Release
```

必须完整部署 Qt 插件、FFmpeg、v142 CRT 和 UCRT，不能仅替换普通包中的 `HarmonyCast.exe`。运行库脚本固定选择 14.29 CRT，避免构建机的新运行库被误带入旧系统包。

打包检查递归检查 EXE/DLL 的架构、子系统版本、Qt6 / D3D12 依赖及若干已知 Windows 10 导入函数，并检查必要运行库是否齐全。这能阻止已知启动问题进入发行包，但不是完整的 Windows API 兼容性认证。
