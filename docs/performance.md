# 性能优化与验证

本轮优化保留默认原画、60FPS 请求参数、现有 HDC 控制命令、原生采集协议和截图兼容模式。没有替换设备端二进制，也没有把低分辨率设为默认。

## 已实施

- 视频拆包、解码、颜色转换、缩放和旋转在后台执行。只有一批待处理网络数据和一帧待显示图像；后台仍顺序解码 H.264，展示时合并过期帧。
- 解码输出为共享的自有 QImage，复用未被显示端引用的缓冲。旧帧可安全保留，截图仍使用原始图像。
- TCP 拆包使用读偏移，流式处理期间保留缓冲容量，避免逐包移动和大帧反复扩缩容；停止时释放缓冲。
- 日志增量刷新合并为 50ms，搜索防抖 180ms；隐藏或暂停时继续缓存，返回面板或恢复时补齐显示。
- HiLog 缓存最多 10000 条，普通总文本预算约 32MiB；WebView 日志最多 5000 条、约 16MiB。预算按字符串内容估算，不是进程 RSS 上限；单条超大日志保留完整内容。HiLog 不再重复保存一份 raw 文本与会话副本。
- WebView 可见文档限制 10000 个文本块，请求记录保留最近 1000 条。响应体 RAM 缓存预算 16MiB，单个大于 2MiB 的响应使用临时文件缓存；不截断正在查看的响应。清空、淘汰请求或退出会清理文件。
- 网络响应按请求 ID 定位行，过滤和计数批量更新。迟到响应不会重新创建已清空的请求，缓存保留 Base64 标志。
- HDC 普通命令最多 4 个工作线程，输入命令使用一个线程保持顺序。关闭窗口后取消尚未执行的输入回调；采集启动与清理串行执行，避免快速重启相互干扰。
- HDC 终端只渲染最近 10000 行，完整输出写入自动清理的临时文件，复制和导出包含全部输出。停止日志进程时不在界面线程同步等待退出。
- 分辨率和流模式未变化时，不重复更新状态标签样式。隐藏投屏区域时释放显示缓存并停止为其预先缩放图像。

## 回归检查

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
QT_SCALE_FACTOR=2 ./build/test_harmonycast_performance
```

Release 下核心测试仍启用断言。性能回归覆盖拆分协议头、连续帧和源坐标、帧生命周期、后台处理、本地 TCP 大帧传输、原尺寸截图、Retina 旋转、H.264 解码与软解回退、暂停恢复、12000 条控制台日志、请求淘汰与迟到响应、完整大响应的临时文件缓存、终端完整历史，以及模拟 HDC 命令顺序和快速启停。测试不需要连接设备。

本机也使用 LLVM 22 的 AddressSanitizer/UndefinedBehaviorSanitizer 运行同一套回归。复现命令：

```sh
cmake -S . -B build/asan-llvm -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build/asan-llvm --target test_harmonycast_core test_harmonycast_performance -j 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/asan-llvm --output-on-failure
```

这些检查覆盖被执行路径的越界、释放后访问及未定义行为，不代表进行了全量泄漏扫描或真机长期稳定性测试。

## 本地基准

```sh
./build/benchmark_harmonycast
# macOS 同时查看进程峰值内存：
/usr/bin/time -l ./build/benchmark_harmonycast
```

基准使用 1080×1920 RGBA 合成图像，每帧按 64KiB 分块输入，显示区域为 540×960。预热 5 帧后处理 120 帧，输出 JSON。这是接收、拆包与显示的局部基准，不含真实设备采集、HDC 带宽、端到端延迟或长时间运行结果。

2026-10-28 本机 Release 构建，与修改前 HEAD 使用同一基准交替执行三次，最终对比中位数：

| 指标 | 修改前 | 修改后 |
| --- | ---: | ---: |
| 每帧处理耗时 | 1.640 ms | 1.291 ms |
| 峰值 RSS | 81,461,248 B | 73,252,864 B |

原始数据保存在本地 `artifacts/performance-comparison.json`。这些数值不代表所有设备与平台的提升比例。旧 `scripts/benchmark_capture.py` 依赖历史 Python 后端，不用于验证当前 C++ 实现。

## 验证边界

没有在线设备时，只能验证协议、解码、界面和模拟控制行为；USB/网络投屏、具体设备的硬件编码、控制响应及兼容截图仍需真机回归。GPU 直接显示、设备端分辨率调整和持久化输入协议涉及平台兼容性，本轮保持原有路径。
