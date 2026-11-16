#!/bin/bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/hc-batch-script-test.XXXXXX")
temporary=$(cd "$temporary" && pwd -P)
cleanup() {
    result=$?
    if [ "$result" -ne 0 ]; then
        for report in "$temporary"/*.log "$temporary/commands"; do [ ! -f "$report" ] || cat "$report" >&2; done
    fi
    rm -rf "$temporary"
}
trap cleanup EXIT
export HC_BATCH_SCRIPT_LOG="$temporary/commands"
export HC_BATCH_SCRIPT_READY="$temporary/ready"
: > "$HC_BATCH_SCRIPT_LOG"
cat > "$temporary/fake hdc" <<'EOF'
#!/bin/bash
printf '<%s>' "$@" >> "$HC_BATCH_SCRIPT_LOG"
printf '\n' >> "$HC_BATCH_SCRIPT_LOG"
if [ "${1:-}" = tconn ]; then
    printf 'Connect OK\n'
elif [ "${3:-}" = shell ]; then
    printf 'com.yarward.etnbedsideoh\ncom.yarward.etnroomoh.extra\ncom.yarward.etndoorcontrol_a33\n'
elif [ "${3:-}" = install ]; then
    if [ "${HC_BATCH_SCRIPT_SLOW:-0}" = 1 ]; then
        : > "$HC_BATCH_SCRIPT_READY"
        exec sleep 20
    elif [ "$2" = 192.0.2.22:8710 ]; then printf 'error: signature mismatch\n'
    elif [ "${HC_BATCH_SCRIPT_NO_ACK:-0}" = 1 ]; then printf '[Info]App install path:%s msg:request sent\n' "$4"
    else printf '[Info]App install path:%s msg:install bundle successfully.\n' "$4"; fi
elif [ "${3:-}" = uninstall ]; then
    if [ "$2" = 192.0.2.22:8710 ] && [ "$4" = com.yarward.etnbedsideoh ]; then printf '[Fail]uninstall failed\n'
    else printf 'uninstall bundle successfully.\n'; fi
else printf 'unexpected command\n' >&2; exit 8
fi
EOF
chmod +x "$temporary/fake hdc"
printf 'test HAP\n' > "$temporary/床旁 error 安装包.hap"
printf '# target list\r\n192.0.2.21\r\n192.0.2.21:8710\n192.0.2.22\n' > "$temporary/devices.txt"
script="$root/scripts/batch_apps.sh"
common=(--ips "$temporary/devices.txt" --hdc "$temporary/fake hdc")

if bash "$script" install "${common[@]}" --file "$temporary/床旁 error 安装包.hap" > "$temporary/install.log" 2>&1; then
    echo 'Expected one failed device' >&2; exit 1
fi
grep -Fq '<-t><192.0.2.21:8710><install><'"$temporary/床旁 error 安装包.hap"'>' "$HC_BATCH_SCRIPT_LOG"
[ "$(grep -Fc '<tconn><192.0.2.21:8710>' "$HC_BATCH_SCRIPT_LOG")" -eq 2 ]
grep -Fq '已安装' "$temporary/install.log"
grep -Fq '<tconn><192.0.2.22:8710><-remove>' "$HC_BATCH_SCRIPT_LOG"
echo 'PASS: deduplication, Unicode/spaces, exact install arguments, path error text, failure continuation and disconnect'

: > "$HC_BATCH_SCRIPT_LOG"
if bash "$script" uninstall "${common[@]}" --yes > "$temporary/uninstall.log" 2>&1; then
    echo 'Expected one failed uninstall' >&2; exit 1
fi
grep -Fq '<-t><192.0.2.21:8710><uninstall><com.yarward.etnbedsideoh>' "$HC_BATCH_SCRIPT_LOG"
! grep -Fq '<uninstall><com.yarward.etnroomoh>' "$HC_BATCH_SCRIPT_LOG"
grep -Fq '<-t><192.0.2.22:8710><uninstall><com.yarward.etndoorcontrol_a33>' "$HC_BATCH_SCRIPT_LOG"
echo 'PASS: only allowed packages, exact installed matching, uninstall failure continuation'

for invalid in '999.1.2.3' '192.0.2.01' '192.0.2.2:0' '192.0.2.2;$(touch /tmp/should-not-run)'; do
    : > "$HC_BATCH_SCRIPT_LOG"
    printf '192.0.2.21\n%s\n' "$invalid" > "$temporary/invalid.txt"
    if bash "$script" uninstall --ips "$temporary/invalid.txt" --hdc "$temporary/fake hdc" --yes > /dev/null 2>&1; then exit 1; fi
    [ ! -s "$HC_BATCH_SCRIPT_LOG" ]
done
: > "$HC_BATCH_SCRIPT_LOG"
if bash "$script" uninstall "${common[@]}" --package com.other.app --yes > /dev/null 2>&1; then exit 1; fi
if bash "$script" uninstall "${common[@]}" < /dev/null > /dev/null 2>&1; then exit 1; fi
if bash "$script" install "${common[@]}" --file "$temporary/missing.hap" > /dev/null 2>&1; then exit 1; fi
[ ! -s "$HC_BATCH_SCRIPT_LOG" ]
echo 'PASS: invalid IP/package/file and missing confirmation perform no device commands'

printf '192.0.2.21\n' > "$temporary/single.txt"
: > "$HC_BATCH_SCRIPT_LOG"
printf 'test HAP\n' > "$temporary/install bundle successfully.hap"
if HC_BATCH_SCRIPT_NO_ACK=1 bash "$script" install --ips "$temporary/single.txt" --hdc "$temporary/fake hdc" --file "$temporary/install bundle successfully.hap" > /dev/null 2>&1; then exit 1; fi
echo 'PASS: no false success from a filename or missing positive receipt'

: > "$HC_BATCH_SCRIPT_LOG"
if HC_BATCH_SCRIPT_SLOW=1 bash "$script" install --ips "$temporary/single.txt" --hdc "$temporary/fake hdc" --file "$temporary/床旁 error 安装包.hap" --timeout 1 > "$temporary/timeout.log" 2>&1; then exit 1; fi
grep -Fq '命令超时' "$temporary/timeout.log"
grep -Fq '<tconn><192.0.2.21:8710><-remove>' "$HC_BATCH_SCRIPT_LOG"
echo 'PASS: bounded timeout and disconnection'

: > "$HC_BATCH_SCRIPT_LOG"
rm -f "$HC_BATCH_SCRIPT_READY"
HC_BATCH_SCRIPT_SLOW=1 bash "$script" install "${common[@]}" --file "$temporary/床旁 error 安装包.hap" > "$temporary/cancel.log" 2>&1 &
batch_pid=$!
for attempt in {1..100}; do [ ! -f "$HC_BATCH_SCRIPT_READY" ] || break; sleep 0.05; done
[ -f "$HC_BATCH_SCRIPT_READY" ]
kill -TERM "$batch_pid"
if wait "$batch_pid"; then echo 'Expected cancelled exit' >&2; exit 1; else [ "$?" -eq 130 ]; fi
grep -Fq '<tconn><192.0.2.21:8710><-remove>' "$HC_BATCH_SCRIPT_LOG"
! grep -Fq '192.0.2.22' "$HC_BATCH_SCRIPT_LOG"
echo 'PASS: termination disconnects the current device and never starts the next'
echo 'All batch shell tests passed'
