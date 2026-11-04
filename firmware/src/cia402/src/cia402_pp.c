/*
 * cia402_pp.c — 一条 PP（Profile Position）绝对位置运动
 *
 * 协议时序（DS 402、手册 5.1.3 / 5.3）：
 *
 *   6081 速度 → 6083 加速度 → 6084 减速度 → 607A 目标位置
 *   → 6040 bit4 上升沿（新目标）→ bit4 落沿
 *   → 等 6041 bit10（target reached）到位
 *
 * 注意两个位不是一回事：
 *   bit12 = set-point acknowledge（新目标已被接收的握手）
 *   bit10 = target reached（真的走到了）  ← 到位判据用这个
 *
 * 插补在驱动器内部做，主站只在"下发"和"等结论"两点出现，没有周期发帧。
 *
 * 已知待真机验证：驱动器若在写新 607A 时不把 bit10 清 0（极短距离运动），
 * 这里用 seen_low 防旧值误判，仍可能判不出。届时改用 6064（实际位置）比对。
 */

#include "cia402_internal.h"

#define PP_PARAM_COUNT 4u                      /* 6081 / 6083 / 6084 / 607A */
#define PP_STEP_TRIGGER (PP_PARAM_COUNT)       /* 置 bit4 */
#define PP_STEP_RELEASE (PP_STEP_TRIGGER + 1u) /* 清 bit4 */
#define PP_STEP_WAIT (PP_STEP_RELEASE + 1u)    /* 等到位 */

#define PP_ARRIVE_TIMEOUT_MS 5000u

/* 第 i 条参数写什么：返回字节数，参数填进 index/value */
static uint8_t param_at(const struct cia402_node *n, uint8_t i, uint16_t *index, uint32_t *value)
{
	switch (i) {
	case 0:
		*index = CIA402_IDX_PROFILE_VELOCITY;
		*value = (uint32_t)n->pp.vel;
		return 4;
	case 1:
		*index = CIA402_IDX_PROFILE_ACCELERATION;
		*value = (uint32_t)n->pp.acc;
		return 4;
	case 2:
		*index = CIA402_IDX_PROFILE_DECELERATION;
		*value = (uint32_t)n->pp.dec;
		return 4;
	default:
		*index = CIA402_IDX_TARGET_POSITION;
		*value = (uint32_t)n->pp.target;
		return 4;
	}
}

int cia402_pp_begin(struct cia402_node *n, int32_t target, int32_t vel, int32_t acc, int32_t dec)
{
	n->result = CIA402_RESULT_NONE; /* 清掉上一个过程的结论，免得被当成本次的 */
	n->pp.target = target;
	n->pp.vel = vel;
	n->pp.acc = acc;
	n->pp.dec = dec;
	n->pp.step = 0;
	n->pp.seen_low = 0;
	n->pp.since_ms = n->last_ms;
	n->pp.phase = CIA402_PP_WRITE;
	return 0;
}

/* 本条写完（或写完那一步）：轮到下一条 */
static void next_step(struct cia402_node *n)
{
	n->pp.step++;
	n->pp.phase = CIA402_PP_WRITE;
}

static void pp_fail(struct cia402_node *n)
{
	cia402_finish(n, CIA402_RESULT_FAILED);
}

void cia402_pp_step(struct cia402_node *n)
{
	switch (n->pp.phase) {
	case CIA402_PP_WRITE: {
		uint16_t index = 0;
		uint32_t value = 0;
		uint8_t size;
		int r;

		if (n->pp.step < PP_PARAM_COUNT) {
			size = param_at(n, n->pp.step, &index, &value);
			r = cia402_sdo_write(n, index, 0, value, size);
		} else if (n->pp.step == PP_STEP_TRIGGER) {
			r = cia402_write_controlword(
				n, (uint16_t)(n->controlword | CIA402_CW_NEW_SETPOINT));
		} else if (n->pp.step == PP_STEP_RELEASE) {
			r = cia402_write_controlword(
				n, (uint16_t)(n->controlword & (uint16_t)~CIA402_CW_NEW_SETPOINT));
		} else {
			n->pp.seen_low = 0;
			n->pp.phase = CIA402_PP_WAIT_ARRIVE;
			return; /* 下一拍开始读状态字 */
		}
		if (r != 0) {
			pp_fail(n);
			return;
		}
		n->pp.phase = CIA402_PP_WAIT_WRITE;
		return;
	}

	case CIA402_PP_WAIT_WRITE: {
		int r = cia402_sdo_result(n);

		if (r == 0) {
			return; /* 还在途 */
		}
		if (r < 0) {
			pp_fail(n);
			return;
		}
		next_step(n);
		return;
	}

	case CIA402_PP_WAIT_ARRIVE: { /* 读状态字，等 bit10 */
		int r;

		if ((uint32_t)(n->last_ms - n->pp.since_ms) > PP_ARRIVE_TIMEOUT_MS) {
			pp_fail(n); /* 一直等不到到位 ⇒ 判失败 */
			return;
		}
		if (n->sdo == CIA402_SDO_IDLE) { /* 该发一次读 */
			if (cia402_sdo_read(n, CIA402_IDX_STATUSWORD, 0) != 0) {
				pp_fail(n);
			}
			return;
		}
		r = cia402_sdo_result(n);
		if (r == 0) {
			return;
		}
		if (r < 0) {
			pp_fail(n);
			return;
		}
		cia402_take_statusword(n);
		if (n->state == CIA402_FAULT) {
			pp_fail(n);
			return;
		}
		if ((n->statusword & CIA402_SW_TARGET_REACHED) == 0U) {
			n->pp.seen_low = 1; /* 目标确实还没到；下一次读到 1 才算到位 */
			return;
		}
		if (n->pp.seen_low == 0U) {
			/* bit10 一开始就是 1：可能目标就等于当前位置（距离 0），
			 * 也可能只是上一次运动的旧值——读 6064 分辨。 */
			n->pp.phase = CIA402_PP_CHECK_POS;
			return;
		}
		cia402_finish(n, CIA402_RESULT_DONE);
		return;
	}

	case CIA402_PP_CHECK_POS: /* 读实际位置，看是不是已经在目标上 */
		if (n->sdo == CIA402_SDO_IDLE) {
			if (cia402_sdo_read(n, CIA402_IDX_POSITION_ACTUAL, 0) != 0) {
				pp_fail(n);
			}
			return;
		}
		n->pp.phase = CIA402_PP_WAIT_POS;
		return;

	default: { /* CIA402_PP_WAIT_POS */
		int r = cia402_sdo_result(n);
		int32_t pos;

		if (r == 0) {
			return;
		}
		if (r < 0) {
			pp_fail(n);
			return;
		}
		pos = (int32_t)((uint32_t)n->data[0] | (uint32_t)n->data[1] << 8 |
				(uint32_t)n->data[2] << 16 | (uint32_t)n->data[3] << 24);
		if (pos == n->pp.target) {
			cia402_finish(n, CIA402_RESULT_DONE);
			return;
		}
		n->pp.phase = CIA402_PP_WAIT_ARRIVE; /* 不在目标上：继续等真正的到位 */
		return;
	}
	}
}
