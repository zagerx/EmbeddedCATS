/*
 * cia402_stop.c — 两种"停"
 *
 *   Halt：6040 bit8 = 1，按 6084 减速，**保持使能 = 保持力矩**，停稳后清 bit8。
 *         出口是"还在可操作态"，可以立刻接下一句运动（应用层的 STOPPING）。
 *
 *   急停：6040 = 0x02（quick stop）→ 等驱动器减速到 Switch on disabled
 *         → 6040 = 0x00（断使能）。出口是"没有力矩"，只能重新使能才动
 *         （应用层的 ESTOP）。
 *
 * "停稳"的判据用 6041 bit10（target reached）：Halt 生效后驱动器把当前位置
 * 当成目标，走到即置位。真机上要确认这一点（刹车/负载下可能一直不置位）。
 */

#include "cia402_internal.h"

#define HALT_TIMEOUT_MS 3000u
#define QUICK_STOP_TIMEOUT_MS 5000u

/* ---- Halt ---- */

int cia402_halt_begin(struct cia402_node *n)
{
	n->result = CIA402_RESULT_NONE;
	n->halt.phase = CIA402_HALT_SET;
	n->halt.since_ms = n->last_ms;
	return 0;
}

static void halt_done(struct cia402_node *n, enum cia402_result r)
{
	n->activity = CIA402_ACT_IDLE;
	n->result = r;
}

void cia402_halt_step(struct cia402_node *n)
{
	switch (n->halt.phase) {
	case CIA402_HALT_SET: /* 置 bit8 */
		if (cia402_write_controlword(n, (uint16_t)(n->controlword | CIA402_CW_HALT)) != 0) {
			halt_done(n, CIA402_RESULT_FAILED);
			return;
		}
		n->halt.phase = CIA402_HALT_WAIT_SET;
		return;

	case CIA402_HALT_WAIT_SET: { /* 等置位写完 */
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return;
		}
		if (r < 0) {
			halt_done(n, CIA402_RESULT_FAILED);
			return;
		}
		n->halt.phase = CIA402_HALT_WAIT_STOP;
		return;
	}

	case CIA402_HALT_WAIT_STOP: { /* 读状态字，等 bit10 */
		int r;

		if ((uint32_t)(n->last_ms - n->halt.since_ms) > HALT_TIMEOUT_MS) {
			halt_done(n, CIA402_RESULT_FAILED);
			return;
		}
		if (n->sdo == CIA402_SDO_IDLE) {
			if (cia402_sdo_read(n, CIA402_IDX_STATUSWORD, 0) != 0) {
				halt_done(n, CIA402_RESULT_FAILED);
			}
			return;
		}
		r = cia402_sdo_result(n);
		if (r == 0) {
			return;
		}
		if (r < 0) {
			halt_done(n, CIA402_RESULT_FAILED);
			return;
		}
		cia402_take_statusword(n);
		if (n->state == CIA402_FAULT) {
			halt_done(n, CIA402_RESULT_FAILED);
			return;
		}
		if ((n->statusword & CIA402_SW_TARGET_REACHED) != 0U) {
			n->halt.phase = CIA402_HALT_CLEAR;
		}
		return;
	}

	case CIA402_HALT_CLEAR: /* 清 bit8，回到可操作态 */
		if (cia402_write_controlword(
			    n, (uint16_t)(n->controlword & (uint16_t)~CIA402_CW_HALT)) != 0) {
			halt_done(n, CIA402_RESULT_FAILED);
			return;
		}
		n->halt.phase = CIA402_HALT_WAIT_CLEAR;
		return;

	default: { /* CIA402_HALT_WAIT_CLEAR */
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return;
		}
		halt_done(n, (r < 0) ? CIA402_RESULT_FAILED : CIA402_RESULT_DONE);
		return;
	}
	}
}

/* ---- 急停：quick stop → 断使能 ---- */

int cia402_quick_stop_begin(struct cia402_node *n)
{
	n->result = CIA402_RESULT_NONE;
	n->estop.phase = CIA402_ESTOP_QUICK;
	n->estop.since_ms = n->last_ms;
	return 0;
}

void cia402_quick_stop_step(struct cia402_node *n)
{
	switch (n->estop.phase) {
	case CIA402_ESTOP_QUICK: /* 6040 = 0x02 */
		if (cia402_write_controlword(n, CIA402_CW_QUICK_STOP) != 0) {
			n->result = CIA402_RESULT_FAILED;
			n->estop.phase = CIA402_ESTOP_DISABLE;
			return;
		}
		n->estop.phase = CIA402_ESTOP_WAIT_QUICK;
		return;

	case CIA402_ESTOP_WAIT_QUICK: {
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return;
		}
		/* 写不进去也照样往下走：断使能是更安全的那一半 */
		n->result = (r < 0) ? CIA402_RESULT_FAILED : CIA402_RESULT_NONE;
		n->estop.phase = CIA402_ESTOP_DECEL;
		return;
	}

	case CIA402_ESTOP_DECEL: {
		int r;

		if ((uint32_t)(n->last_ms - n->estop.since_ms) > QUICK_STOP_TIMEOUT_MS) {
			n->result = CIA402_RESULT_FAILED; /* 减速等不到，但还是要断使能 */
			n->estop.phase = CIA402_ESTOP_DISABLE;
			return;
		}
		if (n->sdo == CIA402_SDO_IDLE) {
			if (cia402_sdo_read(n, CIA402_IDX_STATUSWORD, 0) != 0) {
				n->result = CIA402_RESULT_FAILED;
				n->estop.phase = CIA402_ESTOP_DISABLE;
			}
			return;
		}
		r = cia402_sdo_result(n);
		if (r == 0) {
			return;
		}
		if (r < 0) {
			n->result = CIA402_RESULT_FAILED;
			n->estop.phase = CIA402_ESTOP_DISABLE;
			return;
		}
		cia402_take_statusword(n);
		if (n->state == CIA402_SWITCH_ON_DISABLED || n->state == CIA402_NOT_READY) {
			n->estop.phase = CIA402_ESTOP_DISABLE; /* 已无输出力矩 */
		}
		return;
	}

	case CIA402_ESTOP_DISABLE: /* 6040 = 0x00，断使能 */
		if (cia402_write_controlword(n, CIA402_CW_DISABLE) != 0) {
			n->result = CIA402_RESULT_FAILED;
			n->estop.phase = CIA402_ESTOP_WAIT_DISABLE;
			return;
		}
		n->estop.phase = CIA402_ESTOP_WAIT_DISABLE;
		return;

	default: { /* CIA402_ESTOP_WAIT_DISABLE：写完就结束，不再自动恢复 */
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return;
		}
		if (r < 0) {
			n->result = CIA402_RESULT_FAILED;
		}
		n->activity = CIA402_ACT_IDLE;
		n->step = CIA402_STEP_IDLE; /* 已无输出：再动必须先重新使能 */
		if (n->result == CIA402_RESULT_NONE) {
			n->result = CIA402_RESULT_DONE;
		}
		return;
	}
	}
}
