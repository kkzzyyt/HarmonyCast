#!/bin/bash
# macOS Bash 3.2 compatible. Runs only explicitly selected IPs and application tasks.
set -u

usage() {
    cat <<'EOF'
批量安装 / 卸载鸿蒙应用（每台设备执行后断开）

  bash scripts/batch_apps.sh install --ips devices.txt --file /path/app.hap [--file /path/room.hap]
  bash scripts/batch_apps.sh uninstall --ips devices.txt [--package bedside|room|door|all] --yes

选项：
  --hdc PATH       HDC 程序路径，也可通过 HDC 环境变量指定
  --timeout SEC    每个安装 / 卸载命令的超时秒数，默认 120
  --package NAME   仅卸载模式；默认 all，也接受下列完整包名，可重复指定
  --yes            确认卸载；未指定时只接受终端手动确认
  --help           显示帮助

卸载包名限定为：
  com.yarward.etnbedsideoh
  com.yarward.etnroomoh
  com.yarward.etndoorcontrol_a33

IP 文件使用 UTF-8，每行一个 IPv4 或 IPv4:端口；默认端口 8710。
支持 # 注释、空行、空格/逗号/分号分隔及 CRLF。先验证全部输入，再连接设备。
安装包先复制为临时目录内的 ASCII 短文件名，再执行 hdc install 相对文件名。
不自动卸载、不附加 -r。失败会继续后续项，最终非零退出。
EOF
}

fail() { printf '错误：%s\n' "$*" >&2; exit 2; }
need_value() { [ "$#" -ge 2 ] && [ -n "$2" ] || fail "$1 缺少参数"; }

mode=${1:-}
case "$mode" in
    --help|-h|'') usage; exit 0 ;;
    install|uninstall) shift ;;
    *) usage >&2; fail "任务只能是 install 或 uninstall" ;;
esac
ips_file= hdc=${HDC:-} timeout_seconds=120 confirmed=0
files=() packages=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --ips) need_value "$@"; ips_file=$2; shift 2 ;;
        --file) need_value "$@"; files+=("$2"); shift 2 ;;
        --hdc) need_value "$@"; hdc=$2; shift 2 ;;
        --timeout) need_value "$@"; timeout_seconds=$2; shift 2 ;;
        --package) need_value "$@"; packages+=("$2"); shift 2 ;;
        --yes) confirmed=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) fail "未知选项：$1" ;;
    esac
done
case "$timeout_seconds" in ''|*[!0-9]*) fail "超时必须是 1 到 3600 的整数" ;; esac
[ "${#timeout_seconds}" -le 4 ] && [ "$timeout_seconds" -ge 1 ] && [ "$timeout_seconds" -le 3600 ] || fail "超时必须是 1 到 3600 的整数"
[ -n "$ips_file" ] && [ -f "$ips_file" ] && [ -r "$ips_file" ] || fail "请通过 --ips 指定可读取的 IP 列表"
[ "$(wc -c < "$ips_file" | tr -d ' ')" -le 1048576 ] || fail "IP 文件不能超过 1 MiB"

script_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P) || exit 2
if [ -z "$hdc" ]; then
    if [ -x "$script_root/resources/hdc/hdc" ]; then hdc=$script_root/resources/hdc/hdc
    else hdc=$(command -v hdc) || fail "未找到 hdc，请使用 --hdc 指定"; fi
fi
case "$hdc" in */*) ;; *) hdc=$(command -v "$hdc") || fail "找不到 HDC：$hdc" ;; esac
[ -x "$hdc" ] && [ ! -d "$hdc" ] || fail "HDC 不可执行：$hdc"
hdc_directory=$(cd "$(dirname "$hdc")" && pwd -P) || fail "HDC 目录无法读取"
hdc=$hdc_directory/$(basename "$hdc")
export PATH="$hdc_directory:$PATH"
export DYLD_LIBRARY_PATH="$hdc_directory:${DYLD_LIBRARY_PATH:-}"

selected_packages=()
if [ "$mode" = uninstall ]; then
    [ "${#files[@]}" -eq 0 ] || fail "卸载任务不接受 --file"
    if [ "${#packages[@]}" -eq 0 ]; then packages=(all); fi
    for package in "${packages[@]}"; do
        case "$package" in
            all) names=(com.yarward.etnbedsideoh com.yarward.etnroomoh com.yarward.etndoorcontrol_a33) ;;
            bedside|com.yarward.etnbedsideoh) names=(com.yarward.etnbedsideoh) ;;
            room|com.yarward.etnroomoh) names=(com.yarward.etnroomoh) ;;
            door|com.yarward.etndoorcontrol_a33) names=(com.yarward.etndoorcontrol_a33) ;;
            *) fail "不允许卸载此包：$package" ;;
        esac
        for name in "${names[@]}"; do
            duplicate=0
            if [ "${#selected_packages[@]}" -gt 0 ]; then
                for previous in "${selected_packages[@]}"; do [ "$previous" != "$name" ] || duplicate=1; done
            fi
            [ "$duplicate" -eq 1 ] || selected_packages+=("$name")
        done
    done
else
    [ "${#packages[@]}" -eq 0 ] || fail "安装任务不接受 --package"
    [ "${#files[@]}" -gt 0 ] && [ "${#files[@]}" -le 32 ] || fail "请选择 1 到 32 个安装包（--file）"
fi

selected_files=()
if [ "$mode" = install ]; then
    for file in "${files[@]}"; do
        [ -f "$file" ] && [ -r "$file" ] && [ -s "$file" ] || fail "安装包不存在、为空或无法读取：$file"
        suffix=$(printf '%s' "${file##*.}" | tr '[:upper:]' '[:lower:]')
        case "$suffix" in hap|app) ;; *) fail "安装包必须为 .hap 或 .app：$file" ;; esac
        directory=$(cd "$(dirname "$file")" && pwd -P) || fail "安装包目录无法读取：$file"
        absolute=$directory/$(basename "$file")
        duplicate=0
        if [ "${#selected_files[@]}" -gt 0 ]; then
            for previous in "${selected_files[@]}"; do [ "$previous" != "$absolute" ] || duplicate=1; done
        fi
        [ "$duplicate" -eq 1 ] || selected_files+=("$absolute")
    done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/harmonycast-batch.XXXXXX") || fail "无法创建临时目录"
work=$(cd "$work" && pwd -P) || fail "无法读取临时目录"
current_target= active_pid= timer_pid= command_output= command_status=0
command_diagnostic=
stop_child() {
    if [ -n "$active_pid" ]; then
        kill -TERM "$active_pid" 2>/dev/null || true
        kill -KILL "$active_pid" 2>/dev/null || true
        wait "$active_pid" 2>/dev/null || true
        active_pid=
    fi
    if [ -n "$timer_pid" ]; then
        kill -TERM "$timer_pid" 2>/dev/null || true
        wait "$timer_pid" 2>/dev/null || true
        timer_pid=
    fi
}

run_hdc_at() {
    local directory=$1 limit=$2 child sleeper watcher status diagnostic
    shift 2
    : > "$work/output"
    rm -f "$work/timed-out"
    if [ -n "$directory" ]; then
        (cd "$directory" && exec "$hdc" "$@") > "$work/output" 2>&1 &
    else
        "$hdc" "$@" > "$work/output" 2>&1 &
    fi
    active_pid=$!; child=$active_pid
    (
        sleeper=
        trap '[ -z "$sleeper" ] || kill "$sleeper" 2>/dev/null; exit 0' TERM INT
        sleep "$limit" & sleeper=$!
        wait "$sleeper" || exit 0
        sleeper=
        if kill -0 "$child" 2>/dev/null; then
            : > "$work/timed-out"
            kill -TERM "$child" 2>/dev/null || true
            sleep 1 & sleeper=$!
            wait "$sleeper" || exit 0
            sleeper=
            kill -KILL "$child" 2>/dev/null || true
        fi
    ) &
    timer_pid=$!; watcher=$timer_pid
    wait "$child"; status=$?
    active_pid=
    kill -TERM "$watcher" 2>/dev/null || true
    wait "$watcher" 2>/dev/null || true
    timer_pid=
    command_output=$(head -c 2097152 "$work/output")
    command_status=$status
    if [ -f "$work/timed-out" ]; then command_status=124; command_output="命令超时（${limit} 秒），设备端结果未确认"; fi
    if [ "$(wc -c < "$work/output" | tr -d ' ')" -gt 2097152 ]; then command_status=125; command_output="命令输出超过 2 MiB"; fi
    [ "$command_status" -eq 0 ] || return 1
    # HDC echoes the local path, which may legitimately contain "error"/"failed".
    diagnostic=$(printf '%s\n' "$command_output" | sed -E 's/^(\[Info\])?App (install|uninstall) path:.*msg:/msg:/')
    command_diagnostic=$diagnostic
    ! printf '%s\n' "$diagnostic" | grep -Eiq '\[Fail\]|\[E[[:xdigit:]]+\]|(^|[[:space:]])(error|failed|failure)(:|[[:space:]])|permission denied|access denied|not permitted|not found|inaccessible|unauthori[sz]ed'
}
run_hdc() { run_hdc_at "" "$@"; }

disconnect_target() {
    [ -n "$current_target" ] || return 0
    if ! run_hdc 5 tconn "$current_target" -remove; then
        printf '  断开失败：%s\n' "$command_output" >&2
        return 1
    fi
    current_target=
}
on_signal() {
    trap '' INT TERM
    printf '\n已停止：当前设备端结果可能未确认，正在断开 %s\n' "${current_target:-无}" >&2
    stop_child
    disconnect_target || true
    exit 130
}
on_exit() { stop_child; rm -rf "$work"; }
trap on_signal INT TERM
trap on_exit EXIT

# awk validates every token before any HDC process is started; never eval input.
if ! awk '
    NR == 1 { sub(/^\357\273\277/, "") }
    { sub(/#.*/, ""); gsub(/\r/, ""); gsub(/[,;]/, " ")
      for (i=1; i<=NF; ++i) {
        item=$i; valid=1; count=split(item, endpoint, ":")
        if (count>2 || count<1) valid=0
        n=split(endpoint[1], parts, ".")
        if (n!=4) valid=0
        for (j=1; j<=n; ++j) if (parts[j]!~/^[0-9]+$/ || length(parts[j])>3 || parts[j]+0>255 || (length(parts[j])>1 && substr(parts[j],1,1)=="0")) valid=0
        port=count==1 ? "8710" : endpoint[2]
        if (port!~/^[0-9]+$/ || length(port)>5 || port+0<1 || port+0>65535) valid=0
        if (!valid) { printf "IP 列表第 %d 行无效：%s\n", NR, item > "/dev/stderr"; bad=1; continue }
        target=endpoint[1] ":" (port+0)
        if (!seen[target]++) { ++total; print target }
      }
    }
    END { if (!total || total>10000) { print "有效目标必须为 1 到 10000 台" > "/dev/stderr"; bad=1 }; exit bad ? 1 : 0 }
' "$ips_file" > "$work/targets"; then fail "请先修正整个 IP 列表，尚未执行任何设备命令"; fi

total=$(wc -l < "$work/targets" | tr -d ' ')
printf '任务：%s，设备：%s 台，HDC：%s\n' "$mode" "$total" "$hdc"
cat "$work/targets"
if [ "$mode" = uninstall ]; then
    printf '将卸载以下应用（应用数据也会删除）：\n'
    printf '  %s\n' "${selected_packages[@]}"
    if [ "$confirmed" -ne 1 ]; then
        [ -t 0 ] || fail "非交互执行卸载请显式添加 --yes"
        printf '确认卸载？输入 yes 继续：'
        IFS= read -r answer
        [ "$answer" = yes ] || { printf '已取消，未连接设备\n'; exit 2; }
    fi
else
    printf '将按顺序安装以下文件：\n'
    printf '  %s\n' "${selected_files[@]}"
fi

# Stage every file before any connection. HDC receives only a short ASCII
# relative filename, so its internal argument parsing never sees the user path.
staged_files=()
if [ "$mode" = install ]; then
    mkdir "$work/packages" || fail "无法创建安装包准备目录"
    for ((file_index=0; file_index<${#selected_files[@]}; ++file_index)); do
        file=${selected_files[$file_index]}
        suffix=$(printf '%s' "${file##*.}" | tr '[:upper:]' '[:lower:]')
        short_name=$(printf 'package_%03d.%s' "$((file_index+1))" "$suffix")
        printf '准备安装包：%s\n' "$file"
        cp "$file" "$work/packages/$short_name" > "$work/copy-output" 2>&1 &
        active_pid=$!
        wait "$active_pid"; copy_status=$?
        active_pid=
        if [ "$copy_status" -ne 0 ] || [ ! -r "$work/packages/$short_name" ] || [ ! -s "$work/packages/$short_name" ]; then
            fail "准备安装包失败：$file；$(head -c 1024 "$work/copy-output")"
        fi
        staged_files+=("$short_name")
    done
fi

completed=0 succeeded=0 failed=0 skipped=0
while IFS= read -r target; do
    current_target=$target
    printf '\n[%s/%s] 连接 %s\n' "$((completed+1))" "$total" "$target"
    if ! run_hdc 10 tconn "$target"; then
        printf '  连接失败：%s\n' "$command_output" >&2; failed=$((failed+1))
    elif [ "$mode" = install ]; then
        for ((file_index=0; file_index<${#selected_files[@]}; ++file_index)); do
            file=${selected_files[$file_index]}
            short_name=${staged_files[$file_index]}
            printf '  安装 %s\n' "$file"
            if [ ! -f "$work/packages/$short_name" ] || [ ! -r "$work/packages/$short_name" ] || [ ! -s "$work/packages/$short_name" ]; then
                printf '  安装失败：准备后的安装包已不可用，原文件：%s\n' "$file" >&2; failed=$((failed+1)); continue
            fi
            if run_hdc_at "$work/packages" "$timeout_seconds" -t "$target" install "$short_name" &&
                printf '%s\n' "$command_diagnostic" | grep -Eiq '^[[:space:]]*(msg:)?[[:space:]]*install[[:space:]]+bundle[[:space:]]+successfully\.?[[:space:]]*$'; then
                printf '  已安装\n'; succeeded=$((succeeded+1))
            else printf '  安装失败或未收到成功确认，原文件：%s\n  %s\n' "$file" "$command_output" >&2; failed=$((failed+1)); fi
        done
    else
        if ! run_hdc 15 -t "$target" shell bm dump -a; then
            printf '  查询应用失败：%s\n' "$command_output" >&2; failed=$((failed+1))
        else
            installed=$command_output
            for package in "${selected_packages[@]}"; do
                # Tokenize the package listing so prefix/suffix matches never count.
                if ! printf '%s\n' "$installed" | tr -cs 'A-Za-z0-9_.' '\n' | grep -Fxq "$package"; then
                    printf '  %s 未安装，跳过\n' "$package"; skipped=$((skipped+1)); continue
                fi
                printf '  卸载 %s\n' "$package"
                if run_hdc "$timeout_seconds" -t "$target" uninstall "$package" &&
                    printf '%s\n' "$command_diagnostic" | grep -Eiq '^[[:space:]]*(msg:)?[[:space:]]*uninstall[[:space:]]+bundle[[:space:]]+successfully\.?[[:space:]]*$'; then
                    printf '  已卸载\n'; succeeded=$((succeeded+1))
                else printf '  卸载失败或未收到成功确认：%s\n' "$command_output" >&2; failed=$((failed+1)); fi
            done
        fi
    fi
    disconnect_target || failed=$((failed+1))
    current_target=
    completed=$((completed+1))
done < "$work/targets"
printf '\n处理设备 %s/%s 台；应用成功 %s，跳过 %s，失败 %s\n' "$completed" "$total" "$succeeded" "$skipped" "$failed"
[ "$failed" -eq 0 ]
