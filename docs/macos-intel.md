# macOS Intel 安装包

Intel Mac 使用 `HarmonyCast-版本-macOS-x86_64.dmg`，M 系列使用 `macOS-arm64.dmg`。2.7.0 起两种包均面向 macOS 15 及以上版本，更新检查根据应用的编译架构选择对应包。

Intel 包在 GitHub Actions 的 `macos-15-intel` 构建机上原生编译和运行测试。ARM64 包使用 `macos-15`，两者均固定 Qt 6.6.3、FFmpeg 6.1.3，FFmpeg 源码提交为 `01d3f764f79b2f571174a4cec855e651258f20ec`。媒体库仅开启应用使用的 H.264 解码、解析、缩放和 VideoToolbox，不依赖电脑上另外安装的 Homebrew 媒体库。

随包 macOS HDC、libusb 和 ADB 均包含 Intel 与 ARM64 切片。Intel HDC/libusb 来自经过 SHA-256 校验的官方 OpenHarmony 5.0.0.71 SDK，原有 ARM64 切片保留；来源与派生二进制校验码在 `resources/hdc/checksums.json`。

打包使用 `scripts/build_dmg.sh`。本地可通过 `HARMONYCAST_QT_PREFIX`、`HARMONYCAST_FFMPEG_PREFIX` 指定对应架构的 Qt 和 FFmpeg 安装目录，通过 `HARMONYCAST_MAC_ARCH` 明确选择 `arm64` 或 `x86_64`；不能把本机 ARM64 库用于 Intel 构建。默认从 Homebrew 查找依赖时，产物最低系统要求以实际依赖的 Mach-O 元数据为准。

打包前会运行 `scripts/check_macos_bundle.py`，检查每个 Mach-O 的架构、最低系统要求及依赖加载链，并拒绝包外库、缺失依赖和外逃符号链接。主程序、Qt 平台与图像/TLS 插件、HDC、libusb、ADB 和许可证必须齐全。随后写入最低系统版本、进行 ad-hoc 签名、生成并校验 DMG。

云端回归包含实际 x86_64/ARM64 运行、设备工具启动、解码、巡检及更新包选择。它不等同于所有 Intel Mac 型号或实际设备连接的验收；更早 macOS 版本不在本次构建的支持范围内。
