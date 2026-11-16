# 批量安装与卸载备用脚本

脚本独立于图形界面，可在 macOS 自带 Bash 3.2 或 Linux Bash 中运行，不依赖 Python 或 GNU `timeout`。先关闭 HarmonyCast 当前投屏/批量任务，避免同一设备被两个任务同时操作。

准备 UTF-8 的 `devices.txt`，每行填一个设备 IP，默认端口为 8710。支持 `IP:端口`、`#` 注释、空行、空格/逗号/分号分隔，重复目标自动去重。Windows CRLF 和 UTF-8 BOM 可以直接读取；UTF-16 文件请先另存为 UTF-8。

```text
192.168.1.10
192.168.1.11:8710
```

安装你提供的安装包，可以重复 `--file` 一次选择多个 HAP/APP，按参数顺序逐个安装：

```bash
bash scripts/batch_apps.sh install --ips devices.txt --file "/Users/yourname/Downloads/床旁 应用.hap"
```

脚本先把每个安装包复制到本次临时目录，命名为 `package_001.hap` 这样的纯 ASCII 短文件名，再从该目录执行 `hdc -t IP:端口 install package_001.hap`。这样 HDC 的安装参数不再包含原始中文、空格或长路径，可减少二次解析路径造成的“找不到地址/文件”问题。每个文件只准备一次，后续所有设备复用同一份内容，终端仍显示你选择的原始路径。

安装不自动卸载，也不附加 `-r`。全部文件复制完成后才连接第一台设备；任何文件复制失败会在连接前结束，并显示原始文件路径。已准备的副本在正常结束、超时后结束或取消时自动删除，原始安装包不会修改。若设备已安装的版本、签名或权限不允许安装，会显示原始文件路径和失败原因并继续下一项。

卸载默认处理床旁、病房、A33 门口控制三个指定包：

```bash
bash scripts/batch_apps.sh uninstall --ips devices.txt --yes
```

只卸载床旁应用：

```bash
bash scripts/batch_apps.sh uninstall --ips devices.txt --package bedside --yes
```

`--package` 可使用 `bedside`、`room`、`door`、`all`，或这三个完整包名，可重复选择。其他包名会在执行前拒绝。卸载使用 `hdc -t IP:端口 uninstall 包名`，不保留应用数据。先通过 `bm dump -a` 精确检查包是否已安装，未安装则跳过。没有 `--yes` 时必须在终端输入 `yes` 确认，非交互运行会退出。

指定 HDC 和命令超时：

```bash
bash scripts/batch_apps.sh install --ips devices.txt \
  --file "/path/to/app.hap" --hdc "/path/to/hdc" --timeout 180
```

也可以设置 `HDC=/path/to/hdc`。未指定时优先使用仓库随包的 `resources/hdc/hdc`，其次从 PATH 查找。连接和查询均有独立超时；安装/卸载默认每项 120 秒，`--timeout` 可设为 1–3600 秒。

脚本会先校验全部 IP、HDC 和文件，随后逐台连接、执行、断开，再进入下一台。失败会继续后续项，但最终退出码为 1；全部成功或仅有未安装跳过时退出码为 0。输入错误或取消确认退出码为 2；Ctrl+C/终止信号退出码为 130，会终止当前本地 HDC 命令，并尽力只断开当前目标。已完成的安装或卸载不会撤销，超时/中止时设备端操作结果需要复核。

可以把终端输出保存下来，便于检查：

```bash
bash scripts/batch_apps.sh uninstall --ips devices.txt --yes > batch-result.log 2>&1
```
