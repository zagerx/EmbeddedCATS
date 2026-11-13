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

# 依赖检查忽略纯注释行
code_only() { grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true; }

# R1：PDS 纯知识不依赖传输、线程或平台时钟
section "R1  cia402 PDS 无传输依赖"
PDS_FILES=(
	"$SRC/cia402/src/pds.c"
	"$SRC/cia402/include/cia402/types.h"
	"$SRC/cia402/include/cia402/od.h"
	"$SRC/cia402/include/cia402/pds.h"
)
out=$(grep -nE 'canopen|ds301|0x600|0x580|<transport/|pthread|clock_' "${PDS_FILES[@]}" 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R2：CO_driver 移植层不认识 402
section "R2  canopen/driver/ 不认识 402"
out=$(grep -rnE '0x60(40|41|3[Ff])|0x606[014cC]|0x607[Aa]|ds402|cia402' \
	"$SRC/canopen/driver" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R3：限制 CANopenNode 头文件的使用范围
section "R3  CANopenNode 头文件使用范围"
inc_list=$(grep -rlE '#include *[<"](CANopen\.h|301/)' "$SRC" $INC 2>/dev/null || true)
viol=""
while IFS= read -r f; do
	[ -z "$f" ] && continue
	case "$f" in
	controller/third_party/*) continue ;;
	controller/canopen/driver/*) continue ;;
	controller/canopen/master/*) continue ;;
	controller/canopen/transport/*) continue ;;
	controller/config/canopen_node/*) continue ;;
	esac
	viol+="$f"$'\n'
done <<<"$inc_list"
if [ -z "$viol" ]; then ok; else bad "$viol"; fi

# R4：控制入口与业务状态机不调用阻塞服务
section "R4  控制线程无阻塞调用"
out=$(grep -rnE 'cia402_device_service|cia402_device_pp_move|sdo_client_write|sdo_client_read|nanosleep|usleep|pthread_cond_wait' \
	"$SRC/control_api.c" "$SRC/states" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R5：SocketCAN 只由 CO_driver 承接，不再保留通用 bus 层
section "R5  SocketCAN 仅由 CO_driver 承接"
out=$(grep -rlE 'linux/can.h|sys/socket.h' "$SRC" $INC --exclude-dir=third_party | \
	grep -v '^controller/canopen/driver/CO_driver.c$' || true)
if [ -d "$SRC/bus" ]; then
	bad "$SRC/bus: 通用 bus 目录不应再存在"
fi
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R6：业务状态机不访问协议、传输与主站层
section "R6  states/ 不认识协议与主站"
out=$(grep -rnE '<canopen/|<cia402/|<transport/|<master/|\bmaster_[a-z_]*\(|\bcia402_[a-z_]*\(|0x60(40|41|60|61|7A|64|6C|3F)' \
	"$SRC/states" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R7：HSM 不认识业务类型
section "R7  hsm/ 不认识业务"
out=$(grep -rnE '\bcontroller_|struct controller|<axis/|<cia402/|<transport/|<states/' \
	"$SRC/hsm" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R8：线程创建归宿主
section "R8  controller/ 不创建线程"
out=$(grep -rnE 'pthread_create|std::thread' "$SRC" $INC --include=*.cpp 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R9：移植层不认识主站节点表
section "R9  canopen/driver/ 不认识主站"
out=$(grep -rnE '\bmaster_[a-z_]*\(|node_count|MASTER_HB_TIMEOUT|<master/' \
	"$SRC/canopen/driver" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# R10：主站不认识 402 与业务
section "R10  canopen/master/ 不认识 402 与业务"
out=$(grep -rnE '0x60(40|41|60|61|7A|64|6C|3F)|ds402|cia402|\bcontroller_|\baxis_|<cia402/|<transport/|<states/' \
	"$SRC/canopen/master" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

if [ "$FAILS" -eq 0 ]; then
	printf '\n分层检查：通过\n'
	exit 0
fi
printf '\n分层检查：%d 处违规\n' "$FAILS"
exit 1
