/*
 * ds402_selftest — PDS 状态解析与序列表的自测（纯逻辑，不碰 CAN）
 *
 * 两个判据：
 *   1. 状态字 → 状态：8 种典型状态字都要解对；
 *   2. 起点快进：0x40/0x21/0x23/0x27 分别从第 0/1/2/3 步开始。
 */

#include <ds402/pds.h>

#include <stdio.h>

static int s_fail;

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
			s_fail++;                                                                  \
		}                                                                                  \
	} while (0)

static void test_parse(void)
{
	printf("parse：状态字 → 状态\n");
	CHECK(ds402_parse(0x0000u) == DS402_NOT_READY);
	CHECK(ds402_parse(0x0040u) == DS402_SWITCH_ON_DISABLED);
	CHECK(ds402_parse(0x0021u) == DS402_READY);
	CHECK(ds402_parse(0x0023u) == DS402_SWITCHED_ON);
	CHECK(ds402_parse(0x0027u) == DS402_OP_ENABLED);
	CHECK(ds402_parse(0x0008u) == DS402_FAULT); /* 故障 */
	CHECK(ds402_parse(0x000Fu) == DS402_FAULT); /* 故障反应中 */
	CHECK(ds402_parse(0x1234u) == DS402_STATE_UNKNOWN);
	/* 高位置位不影响判定（只看 bit0..3 / bit5 / bit6） */
	CHECK(ds402_parse(0x0627u) == DS402_OP_ENABLED);
}

static void test_skip(void)
{
	uint8_t n = 0u;
	const struct ds402_seq_step *seq = ds402_seq_enable(&n);

	printf("skip：起点快进\n");
	CHECK(n == 3u);
	CHECK(ds402_seq_skip(0x0040u, seq, n) == 0u); /* 禁启动：从 06 开始 */
	CHECK(ds402_seq_skip(0x0021u, seq, n) == 1u); /* Ready：跳过 06 */
	CHECK(ds402_seq_skip(0x0023u, seq, n) == 2u); /* Switched on：直接 0F */
	CHECK(ds402_seq_skip(0x0027u, seq, n) == 3u); /* 已使能：不用发帧 */

	seq = ds402_seq_disable(&n);
	CHECK(n == 1u);
	CHECK(ds402_seq_skip(0x0027u, seq, n) == 0u); /* 使能中 → 要写 00 */
	CHECK(ds402_seq_skip(0x0040u, seq, n) == 1u); /* 已禁启动 → 无需动作 */
}

int main(void)
{
	test_parse();
	test_skip();

	if (s_fail == 0) {
		printf("ds402_selftest: 全部通过\n");
		return 0;
	}
	printf("ds402_selftest: %d 处失败\n", s_fail);
	return 1;
}
