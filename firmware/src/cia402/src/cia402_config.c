/*
 * cia402_config.c — 参数配置：把"写什么"和"读回该是什么"写成一张表
 *
 *   6060 = 1（PP） → 读 6061 确认还是 1
 *
 * 为什么与使能分开：配置改的是对象字典参数，判据是"读回值对不对"；
 * 使能推的是驱动器状态机，判据是"状态字到位没到"。混在一张表里就会出现
 * "写完不管"这种特殊值。本表写完必校验，校验不过直接判失败。
 *
 * 相位与使能共用 CIA402_PHASE_*，一条一次只看一个相位：
 *   写帧 → 等写结果 → 发读 → 等读结果并比对
 * 与使能不同：值不对就失败，不重读（状态字可能慢一拍，参数值不会）。
 *
 * 只在驱动器未使能时执行：CiA 402 规定 6060 在带电机输出时写入会被拒绝
 * （本流程排在 NMT 启动之前，即 pre-operational 阶段）。
 *
 * 本文件只有"一条参数怎么走完"和"下一条是谁"：
 *   SDO 怎么发怎么收        → cia402_sdo.c
 *   什么时候开始、谁在调用  → cia402.c
 */

#include "cia402_internal.h"

#include <stddef.h>

#define MODE_PP 1u /* 6060 = 1 = 协议位置模式（手册 5.1.3） */

/*
 * 一条参数：写哪个对象、写什么，以及写完之后读哪个对象、哪个位段来核对。
 * check_index == 0 表示"这条不校验"（驱动器不支持读回时用）。
 */
struct config_item {
	uint16_t index;       /* 写哪个对象 */
	uint8_t sub;          /* 子索引 */
	uint8_t size;         /* 写几个字节 */
	uint32_t value;       /* 写什么 */
	uint16_t check_index; /* 读哪个对象来核对；0 = 不核对 */
	uint8_t check_sub;    /* 核对的子索引 */
	uint32_t check_mask;  /* (读回 & check_mask) 必须等于 check_value */
	uint32_t check_value;
};

static const struct config_item CONFIG_SEQ[] = {
	/* 模式：设成 PP，并确认驱动器真的接受了（手册初始值是 8 = CSP） */
	{CIA402_IDX_MODE, 0, 1, MODE_PP, CIA402_IDX_MODE_ACTUAL, 0, 0xFFu, MODE_PP},
};

#define CONFIG_SEQ_COUNT (sizeof(CONFIG_SEQ) / sizeof(CONFIG_SEQ[0]))

static void fail(struct cia402_node *n)
{
	n->cfg_state = CIA402_CFG_FAILED;
	n->activity = CIA402_ACT_IDLE;
	n->result = CIA402_RESULT_FAILED;
}

/* 把最近一次 SDO 读到的原始数据（小端）拼成整数 */
static uint32_t read_value(const struct cia402_node *n)
{
	uint32_t v = 0;
	uint8_t i;

	for (i = 0; i < n->data_len && i < 4U; i++) {
		v |= (uint32_t)n->data[i] << (8u * i);
	}
	return v;
}

/* 这条参数过完了：轮到下一条；已是最后一条就是配置完成 */
static void advance(struct cia402_node *n)
{
	if (n->cfg.seq + 1U >= CONFIG_SEQ_COUNT) {
		n->cfg_state = CIA402_CFG_CONFIGURED; /* 游标停在最后一条，不再用 */
		n->activity = CIA402_ACT_IDLE;
		n->result = CIA402_RESULT_DONE;
		return;
	}
	n->cfg.seq++;
	n->cfg.phase = CIA402_PHASE_WRITE;
}

void cia402_config_start(struct cia402_node *n)
{
	n->cfg.seq = 0;
	n->cfg.phase = CIA402_PHASE_WRITE;
	n->cfg_state = CIA402_CFG_PENDING;
	n->activity = CIA402_ACT_CONFIGURING;
	n->result = CIA402_RESULT_NONE;
}

void cia402_config_step(struct cia402_node *n)
{
	const struct config_item *item = &CONFIG_SEQ[n->cfg.seq];

	switch (n->cfg.phase) {
	case CIA402_PHASE_WRITE: /* 发写 */
		if (cia402_sdo_write(n, item->index, item->sub, item->value, item->size) != 0) {
			fail(n);
			return;
		}
		n->cfg.phase = CIA402_PHASE_WAIT_WRITE;
		return;

	case CIA402_PHASE_WAIT_WRITE: { /* 等写结果 */
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return; /* 还在途 */
		}
		if (r < 0) {
			fail(n); /* abort：驱动器不接受这个参数 */
			return;
		}
		if (item->check_index == 0U) {
			advance(n); /* 这条不校验 */
			return;
		}
		n->cfg.phase = CIA402_PHASE_READ;
		return;
	}

	case CIA402_PHASE_READ: /* 发读，核对写进去的值 */
		if (cia402_sdo_read(n, item->check_index, item->check_sub) != 0) {
			fail(n);
			return;
		}
		n->cfg.phase = CIA402_PHASE_WAIT_READ;
		return;

	default: { /* CIA402_PHASE_WAIT_READ：等读结果并比对 */
		int r = cia402_sdo_result(n);
		uint32_t actual;

		if (r == 0) {
			return;
		}
		if (r < 0) {
			fail(n);
			return;
		}
		actual = read_value(n) & item->check_mask;
		if (actual != item->check_value) {
			fail(n); /* 参数没生效：后面照常发指令也不会按预期动 */
			return;
		}
		advance(n);
		return;
	}
	}
}
