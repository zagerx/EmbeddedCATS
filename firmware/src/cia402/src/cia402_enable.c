/*
 * cia402_enable.c — 使能序列：把"写什么 + 等哪个状态"写成一张表
 *
 *   6060 = 1（PP，先定模式；写完不确认）
 *   6040 = 06 → 等 0x21（READY）
 *   6040 = 07 → 等 0x23（SWITCHED_ON）
 *   6040 = 0F → 等 0x27（OP_ENABLED）
 *
 * 每条命令走四个相位（CIA402_PHASE_*），一条命令一次只看一个相位：
 *   写帧 → 等写结果 → 发读 → 等读结果并判状态字
 * 状态字还没到位就再读一次（驱动器可能慢一拍）；读到 FAULT 立刻判失败。
 *
 * 本文件只有"一条命令怎么走完"和"下一条是谁"：
 *   SDO 怎么发怎么收        → cia402_sdo.c
 *   什么时候开始、谁在调用  → cia402.c
 */

#include "cia402_internal.h"

#include <stddef.h>

#define MODE_PP 1u /* 6060 = 1 = 协议位置模式（手册 5.1.3） */

/*
 * 一条使能命令：写哪个对象、写什么、写几个字节、写完要确认到哪个状态。
 * expect == CIA402_STATE_UNKNOWN 表示"写完就算过，不用读状态字确认"。
 */
struct enable_cmd {
	enum cia402_step step;    /* 对外进度标签 */
	uint16_t index;           /* 写哪个对象 */
	uint32_t value;           /* 写什么 */
	uint8_t size;             /* 字节数 */
	enum cia402_state expect; /* 写完后要读到这个状态才算这一步成功 */
};

static const struct enable_cmd ENABLE_SEQ[] = {
	{CIA402_STEP_SET_MODE, CIA402_IDX_MODE, MODE_PP, 1, CIA402_STATE_UNKNOWN},
	{CIA402_STEP_SHUTDOWN, CIA402_IDX_CONTROLWORD, CIA402_CW_SHUTDOWN, 2, CIA402_READY},
	{CIA402_STEP_SWITCH_ON, CIA402_IDX_CONTROLWORD, CIA402_CW_SWITCH_ON, 2, CIA402_SWITCHED_ON},
	{CIA402_STEP_ENABLE, CIA402_IDX_CONTROLWORD, CIA402_CW_ENABLE_OP, 2, CIA402_OP_ENABLED},
};

#define ENABLE_SEQ_COUNT (sizeof(ENABLE_SEQ) / sizeof(ENABLE_SEQ[0]))

static void fail(struct cia402_node *n)
{
	n->step = CIA402_STEP_FAILED;
	n->activity = CIA402_ACT_IDLE;
	n->result = CIA402_RESULT_FAILED;
}

/* 写一条命令：控制字走统一入口（要记住当前值），其余直接写 */
static int write_cmd(struct cia402_node *n, const struct enable_cmd *cmd)
{
	if (cmd->index == CIA402_IDX_CONTROLWORD) {
		return cia402_write_controlword(n, (uint16_t)cmd->value);
	}
	return cia402_sdo_write(n, cmd->index, 0, cmd->value, cmd->size);
}

/* 这条命令过完了：轮到下一条；已是最后一条就把结论置 ENABLED */
static void advance(struct cia402_node *n)
{
	if (n->en.seq + 1U >= ENABLE_SEQ_COUNT) {
		n->step = CIA402_STEP_ENABLED; /* 序列走完；游标停在最后一条，不再用 */
		n->activity = CIA402_ACT_IDLE;
		n->result = CIA402_RESULT_DONE;
		return;
	}
	n->en.seq++;
	n->en.phase = CIA402_PHASE_WRITE;
	n->step = ENABLE_SEQ[n->en.seq].step;
}

void cia402_enable_start(struct cia402_node *n)
{
	n->en.seq = 0;
	n->en.phase = CIA402_PHASE_WRITE;
	n->step = ENABLE_SEQ[0].step;
	n->activity = CIA402_ACT_ENABLING;
	n->result = CIA402_RESULT_NONE;
	n->controlword = CIA402_CW_DISABLE; /* 使能前驱动器处于无输出状态 */
}

void cia402_enable_step(struct cia402_node *n)
{
	const struct enable_cmd *cmd = &ENABLE_SEQ[n->en.seq];

	switch (n->en.phase) {
	case CIA402_PHASE_WRITE: /* 发写 */
		if (write_cmd(n, cmd) != 0) {
			fail(n);
			return;
		}
		n->en.phase = CIA402_PHASE_WAIT_WRITE;
		return;

	case CIA402_PHASE_WAIT_WRITE: { /* 等写结果 */
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return; /* 还在途 */
		}
		if (r < 0) {
			fail(n);
			return;
		}
		if (cmd->expect == CIA402_STATE_UNKNOWN) {
			advance(n); /* 这一步不需要确认 */
			return;
		}
		n->en.phase = CIA402_PHASE_READ;
		return;
	}

	case CIA402_PHASE_READ: /* 发读状态字 */
		if (cia402_sdo_read(n, CIA402_IDX_STATUSWORD, 0) != 0) {
			fail(n);
			return;
		}
		n->en.phase = CIA402_PHASE_WAIT_READ;
		return;

	default: { /* CIA402_PHASE_WAIT_READ：等读结果并判状态 */
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return;
		}
		if (r < 0) {
			fail(n);
			return;
		}
		cia402_take_statusword(n);
		if (n->state == CIA402_FAULT) {
			fail(n); /* 驱动器自己进故障，不再往下走 */
			return;
		}
		if (n->state != cmd->expect) {
			n->en.phase = CIA402_PHASE_READ; /* 还没到，再读一次 */
			return;
		}
		advance(n);
		return;
	}
	}
}
