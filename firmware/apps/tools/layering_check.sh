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

# 只保代码、扔掉纯注释行：注释里解释"本层不知道 402"不算依赖
code_only() { grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true; }

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
# 我们的移植层（co_driver_cats）只搬字节：不许出现 402 的对象号与词汇
section "R2  canopen_node/ 不认识 402"
out=$(grep -rnE '0x60(40|41|3[Ff])|0x606[014cC]|0x607[Aa]|ds402|cia402' \
	"$SRC/canopen_node" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# ---------------------------------------------------------------- R3
# 只有 master/、drive/backends 与移植层可以 include CANopenNode 的头
section "R3  只有 master/、drive/backends、canopen_node 碰 CANopenNode 头"
inc_list=$(grep -rlE '#include *[<"](CANopen\.h|301/)' "$SRC" $INC 2>/dev/null || true)
viol=""
while IFS= read -r f; do
	[ -z "$f" ] && continue
	case "$f" in
	# 第三方源码本身不参与分层检查
	controller/third_party/*) continue ;;
	controller/canopen_node/*) continue ;;
	controller/master/*) continue ;;
	controller/drive/backends/* | controller/drive/src/backends/*) continue ;;
	# 后端的对外接口头：只有它需要暴露 CO_SDOclient_t
	controller/drive/include/drive/od_sdo.h) continue ;;
	controller/config/canopen_node/*) continue ;;
	esac
	viol+="$f"$'\n'
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

# ---------------------------------------------------------------- R9
# canopen 是协议栈：不知道有几个从站，也不认识主站的站号表
section "R9  canopen_node/ 不认识主站"
out=$(grep -rnE '\bmaster_[a-z_]*\(|node_count|MASTER_HB_TIMEOUT|<master/' \
	"$SRC/canopen_node" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# ---------------------------------------------------------------- R10
# master 只建"谁收发给谁"：不认识 402、轴、业务状态
section "R10  master/ 不认识 402 与业务"
out=$(grep -rnE '0x60(40|41|60|61|7A|64|6C|3F)|ds402|cia402|\bcats_|\bstm_|\baxis_|<drive/|<statemachine/' \
	"$SRC/master" $INC 2>/dev/null | code_only)
if [ -z "$out" ]; then ok; else bad "$out"; fi

# ---------------------------------------------------------------- 汇总
if [ "$FAILS" -eq 0 ]; then
	printf '\n分层检查：通过\n'
	exit 0
fi
printf '\n分层检查：%d 处违规\n' "$FAILS"
exit 1
