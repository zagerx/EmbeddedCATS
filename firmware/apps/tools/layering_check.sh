#!/usr/bin/env bash
# layering_check.sh — 分层边界检查（本地与 CI 都能跑）
#
# 用法：apps/tools/layering_check.sh（在 firmware/ 下执行，或用绝对路径）
# 退出码：0 = 无违规；1 = 有违规
#
# 规则来源：docs/01_设计文档/分层与线程模型.md
# 注意：脚本自身在 firmware/apps/tools/，源码根在它上两级的 controller/
#   分层的价值靠"能一条命令检查"来保证；只写在文档里的约定一定会漂。

set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

SRC=controller
INC='--include=*.c --include=*.h'

# 迁移期债务白名单：阶段 5 删除 src/cia402/ 时，同步删掉这一行
DEBT_ALLOW='^controller/cia402/'

FAILS=0
DEBT_LINES=""

section() { printf '\n=== %s ===\n' "$1"; }
ok()      { printf '  ok\n'; }
bad()     { printf '%s\n' "$1"; FAILS=$((FAILS + 1)); }

# ---------------------------------------------------------------- R1
# ds402 是纯标准知识：不许出现传输相关的东西
section "R1  ds402/ 不依赖任何传输"
if [ ! -d "$SRC/ds402" ]; then
	ok
	printf '  （目录尚未建立，跳过）\n'
else
	out=$(grep -rnE 'canopen|ds301|0x600|0x580' "$SRC/ds402" $INC 2>/dev/null || true)
	if [ -z "$out" ]; then ok; else bad "$out"; fi
fi

# ---------------------------------------------------------------- R2
# canopen 是 DS 301：不许出现 402 的对象号与词汇
section "R2  canopen/ 不认识 402"
out=$(grep -rnE '0x60(40|41|3[Ff])|0x606[014cC]|0x607[Aa]|ds402|cia402' \
	"$SRC/canopen" $INC 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# ---------------------------------------------------------------- R3
# 只有 drive/backends 与 control 可以 include <canopen/...>
section "R3  只有 drive/backends 与 control 碰 <canopen/...>"
inc_list=$(grep -rl '<canopen/' "$SRC" $INC 2>/dev/null || true)
viol=""
while IFS= read -r f; do
	[ -z "$f" ] && continue
	case "$f" in
	controller/canopen/*) continue ;;
	controller/drive/backends/* | controller/drive/src/backends/*) continue ;;
	controller/include/* | controller/*.c) continue ;;
	esac
	if [[ "$f" =~ $DEBT_ALLOW ]]; then
		DEBT_LINES+="  $f"$'\n'
	else
		viol+="$f"$'\n'
	fi
done <<<"$inc_list"
if [ -z "$viol" ]; then ok; else bad "$viol"; fi

# ---------------------------------------------------------------- R4
# 控制线程里不许出现会等待的调用
section "R4  控制线程无阻塞调用"
CTRL="$SRC/ctrl_thread.c"
if [ ! -f "$CTRL" ]; then
	ok
	printf '  （%s 尚未拆分，跳过）\n' "$CTRL"
else
	out=$(grep -nE 'drive_run_to|drive_service|od_sdo_write|od_sdo_read|nanosleep|usleep|pthread_cond_wait' \
		"$CTRL" 2>/dev/null || true)
	if [ -z "$out" ]; then ok; else bad "$out"; fi
fi

# ---------------------------------------------------------------- R5
# bus 只管一帧：不许出现协议词
section "R5  bus/ 不认识协议"
if [ ! -d "$SRC/bus" ]; then
	ok
	printf '  （目录尚未建立，跳过）\n'
else
	out=$(grep -rniE 'nmt|sdo|pdo|emcy|lss|0x600|0x580' "$SRC/bus" $INC 2>/dev/null || true)
	if [ -z "$out" ]; then ok; else bad "$out"; fi
fi

# ---------------------------------------------------------------- R6
# app 是业务层：不许出现协议层词汇（402 对象号、canopen）
section "R6  statemachine/ 不认识协议"
if [ ! -d "$SRC/statemachine" ]; then
	ok
	printf '  （目录尚未建立，跳过）\n'
else
	# 只看真实依赖与对象号；注释里提到 "canopen" 不算违规
	out=$(grep -rnE '<canopen/|<ds402/|\bds402_[a-z_]*\(|0x60(40|41|60|61|7A|64|6C|3F)' \
		"$SRC/statemachine" $INC 2>/dev/null || true)
	if [ -z "$out" ]; then ok; else bad "$out"; fi
fi

# ---------------------------------------------------------------- R7
# hsm 是通用内核：不许认识业务（cats_/stm_）或任何上层类型
section "R7  hsm/ 不认识业务"
if [ ! -d "$SRC/hsm" ]; then
	ok
	printf '  （目录尚未建立，跳过）\n'
else
	out=$(grep -rnE 'cats_|\bstm_[a-z_]|struct stm|<axis/|<drive/|<statemachine/' "$SRC/hsm" $INC 2>/dev/null || true)
	if [ -z "$out" ]; then ok; else bad "$out"; fi
fi

# ---------------------------------------------------------------- R8
# controller 只提供"推进一步"的接口，线程创建归宿主（apps/）
section "R8  controller/ 不创建线程"
out=$(grep -rnE 'pthread_create|std::thread' "$SRC" $INC --include=*.cpp 2>/dev/null || true)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# ---------------------------------------------------------------- 汇总
if [ -n "$DEBT_LINES" ]; then
	printf '\n=== [debt] 迁移期白名单（不算失败）===\n%s' "$DEBT_LINES"
fi

if [ "$FAILS" -eq 0 ]; then
	printf '\n分层检查：通过\n'
	exit 0
fi
printf '\n分层检查：%d 处违规\n' "$FAILS"
exit 1
