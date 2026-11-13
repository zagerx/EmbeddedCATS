#!/usr/bin/env bash
# 分层边界检查；规则见 docs/01_设计文档/分层与线程模型.md。
# 可在任意目录执行；0 = 通过，1 = 存在违规。

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

SRC=controller
INC='--include=*.c --include=*.h'
FAILS=0

section() { printf '\n=== %s ===\n' "$1"; }
ok()      { printf '  ok\n'; }
bad()     { printf '%s\n' "$1"; FAILS=$((FAILS + 1)); }
code_only() { grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true; }

section "R1  cia402 PDS 无传输依赖"
PDS_FILES=(
	"$SRC/cia402/src/pds.c"
	"$SRC/cia402/include/cia402/types.h"
	"$SRC/cia402/include/cia402/od.h"
	"$SRC/cia402/include/cia402/pds.h"
)
out=$(grep -nE 'canopen|ds301|0x600|0x580|<transport/|pthread|clock_' "${PDS_FILES[@]}" 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R2  宿主不接触 CANopen 驱动与协议类型"
out=$(grep -rnE 'linux/can|sys/socket|CANopen\.h|CO_epoll|CO_driver|controller_socketcan' \
	apps $INC --include=*.cpp 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R3  CANopenNode / CANopenLinux 头文件使用范围"
inc_list=$(grep -rlE '#include *[<"](CANopen\.h|301/|CO_epoll_interface\.h|CO_driver_target\.h)' \
	"$SRC" $INC 2>/dev/null || true)
viol=""
while IFS= read -r f; do
	[ -z "$f" ] && continue
	case "$f" in
	controller/third_party/*) continue ;;
	controller/canopen/master/*) continue ;;
	controller/canopen/transport/*) continue ;;
	controller/config/canopen_node/*) continue ;;
	esac
	viol+="$f"$'\n'
done <<<"$inc_list"
if [ -z "$viol" ]; then ok; else bad "$viol"; fi

section "R4  命令与业务状态机无阻塞调用"
out=$(grep -rnE 'cia402_device_service|sdo_client_write|sdo_client_read|nanosleep|usleep|pthread_cond_wait|epoll_wait' \
	"$SRC/control_api.c" "$SRC/states" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R5  SocketCAN 只由上游 CANopenLinux 承接"
out=$(grep -rlE 'linux/can.h|sys/socket.h' "$SRC" $INC --exclude-dir=third_party 2>/dev/null || true)
for dir in "$SRC/bus" "$SRC/canopen/driver"; do
	if [ -d "$dir" ]; then bad "$dir: 已被替代的本机总线目录不应存在"; fi
done
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R6  整机状态机不访问协议、传输与主站层"
out=$(grep -rnE '<canopen/|<cia402/|<transport/|<master/|\bmaster_[a-z_]*\(|\bcia402_[a-z_]*\(|0x60(40|41|60|61|7A|64|6C|3F)' \
	"$SRC/states" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R7  HSM 不认识业务类型"
out=$(grep -rnE '\bcontroller_|struct controller|<axis/|<cia402/|<transport/|<states/' \
	"$SRC/hsm" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R8  线程创建归宿主"
out=$(grep -rnE 'pthread_create|std::thread' "$SRC" $INC --include=*.cpp --exclude-dir=third_party 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R9  第三方源码保持原样"
out=$(git -C "$SRC/third_party/CANopenLinux" diff --quiet -- . ':!CANopenNode' 2>&1 &&
	git -C "$SRC/third_party/CANopenLinux/CANopenNode" diff --quiet 2>&1)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R10  主站不认识 402 与业务"
out=$(grep -rnE '0x60(40|41|60|61|7A|64|6C|3F)|ds402|cia402|\bcontroller_|\baxis_|<cia402/|<transport/|<states/' \
	"$SRC/canopen/master" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

section "R11  CiA402 不依赖线程、epoll 与协议栈"
out=$(grep -rnE 'pthread_|epoll|CANopen\.h|301/' "$SRC/cia402" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

if [ "$FAILS" -eq 0 ]; then
	printf '\n分层检查：通过\n'
	exit 0
fi
printf '\n分层检查：%d 处违规\n' "$FAILS"
exit 1
