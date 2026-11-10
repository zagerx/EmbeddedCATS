#!/usr/bin/env bash
# check_c_layout.sh — 只读检查 C 源文件是否符合本项目排版规范
#
# 用法: ./check_c_layout.sh <file.c> [file.c ...]
# 退出码: 0 全部通过, 1 有违规或文件不存在, 2 用法错
#
# 检查项:
#   1. 禁止 /* ---- xxx ---- */ 之类装饰性分段标记
#   2. 注释里禁止圆圈序号
#   3. 文件头必须含 Author 行
#   4. 文件头注释块不超过 20 行
#   5. static 函数实现不得出现在对外函数定义之前

rc=0

usage() {
	echo "用法: $0 <file.c> [file.c ...]" >&2
	exit 2
}

report() {
	printf '%s:%s: %s\n' "$1" "$2" "$3"
	rc=1
}

# 打印所有匹配行，参数: 文件 正则 原因
scan() {
	local f="$1" re="$2" why="$3" ln
	while IFS=: read -r ln _; do
		[ -n "$ln" ] || continue
		report "$f" "$ln" "$why"
	done < <(grep -n -E "$re" "$f" 2>/dev/null || true)
}

check_file() {
	local f="$1" hdr_end lines first_pub last_priv

	if [ ! -f "$f" ]; then
		printf '%s: 文件不存在\n' "$f" >&2
		rc=1
		return
	fi

	# 1. 装饰性分段标记
	scan "$f" '/\*[[:space:]]*[-=]{2,}.*[-=]{2,}[[:space:]]*\*/' \
		"禁止分段标记注释 /* ---- xxx ---- */"

	# 2. 圆圈序号
	scan "$f" '[①②③④⑤⑥⑦⑧⑨⑩]' "注释里禁止圆圈序号，改用名词短语"

	# 3. 文件头必须有 Author
	if ! head -n 30 "$f" | grep -q -E '^[[:space:]]*\*[[:space:]]*Author:'; then
		report "$f" 1 "文件头缺少 Author 行"
	fi

	# 4. 文件头 <= 20 行（第一个 */ 的行号）
	hdr_end=$(awk 'NR > 1 && /^[[:space:]]*\*\//{print NR; exit}' "$f")
	if [ -z "$hdr_end" ]; then
		report "$f" 1 "找不到文件头注释块"
	elif [ "$hdr_end" -gt 20 ]; then
		report "$f" "$hdr_end" "文件头注释块 $hdr_end 行，超过 20 行上限"
	fi

	# 5. static 实现不得排在对外函数之前（实现应全部沉底）
	# 只有往下第一个标点是 { 才算实现；多行原型后面跟的是 ;
	first_priv=$(awk '
		/^static[[:space:]]/ {
			cand = NR
			if ($0 ~ /\{/) { print cand; exit }
			if ($0 ~ /;/) { cand = 0 }
			next
		}
		cand {
			if ($0 ~ /\{/) { print cand; exit }
			if ($0 ~ /;/) { cand = 0 }
		}
	' "$f")
	last_pub=$(grep -n -E '^[a-zA-Z_][a-zA-Z0-9_[:space:]\*]*\(' "$f" |
		grep -v -E ';[[:space:]]*$' |
		grep -v -E '^[0-9]+:(static|typedef)[[:space:]]' |
		tail -n 1 | cut -d: -f1)

	if [ -n "$first_priv" ] && [ -n "$last_pub" ] && [ "$first_priv" -lt "$last_pub" ]; then
		report "$f" "$first_priv" "static 函数实现出现在对外函数之前，应全部沉到文件底部"
	fi
}

[ $# -ge 1 ] || usage

for f in "$@"; do
	check_file "$f"
done

if [ "$rc" -eq 0 ]; then
	echo "OK: $# 个文件符合排版规范"
fi

exit "$rc"
