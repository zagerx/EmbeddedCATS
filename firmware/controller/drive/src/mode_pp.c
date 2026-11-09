/*
 * mode_pp.c — PP（Profile Position）绝对位置运动
 *
 *   6081 速度 → 6083 加速 → 6084 减速 → 607A 目标 → 6040 bit4 脉冲
 *   → 之后由 drive_service() 的轮询读状态字，等 bit10（target reached）
 *
 * 两个位不是一回事：
 *   bit12 = set-point acknowledge（驱动器收到新目标的握手）
 *   bit10 = target reached（真的走到了）← 到位判据用这个
 *
 * 插补在驱动器内部做，主站只在"下发"和"等结论"两点出现。
 */

#include "drive_internal.h"

#include <ds402/od.h>

static int pp_begin(struct drive *d, int32_t target, int32_t vel, int32_t acc, int32_t dec,
		    uint32_t now_ms)
{
	if (drive_od_write(d, DS402_OD_PROFILE_VELOCITY, 0u, (uint32_t)vel, 4u) != 0) {
		return -1;
	}
	if (drive_od_write(d, DS402_OD_PROFILE_ACCEL, 0u, (uint32_t)acc, 4u) != 0) {
		return -1;
	}
	if (drive_od_write(d, DS402_OD_PROFILE_DECEL, 0u, (uint32_t)dec, 4u) != 0) {
		return -1;
	}
	if (drive_od_write(d, DS402_OD_TARGET_POSITION, 0u, (uint32_t)target, 4u) != 0) {
		return -1;
	}

	/* bit4 上升沿触发新目标，紧接着落沿（DS 402 要求边沿而非电平） */
	if (drive_write_controlword(d, (uint16_t)(d->controlword | DS402_CW_NEW_SETPOINT)) != 0) {
		return -1;
	}
	if (drive_write_controlword(
		    d, (uint16_t)(d->controlword & (uint16_t)~DS402_CW_NEW_SETPOINT)) != 0) {
		return -1;
	}

	d->pp_goal = target;
	d->pp_seen_low = 0u;
	d->pp_since_ms = now_ms;
	d->pp_wait = 1u;
	return 0;
}

static int pp_poll(struct drive *d, uint32_t now_ms)
{
	uint16_t sw = 0u;
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (d->pp_wait == 0u) {
		return 0;
	}
	if ((uint32_t)(now_ms - d->pp_since_ms) > DRIVE_PP_ARRIVE_TIMEOUT_MS) {
		d->pp_wait = 0u;
		return -1;
	}
	if (d->od->read(d->od->ctx, DS402_OD_STATUSWORD, 0u, &v, &len, DRIVE_MON_TIMEOUT_MS) != 0) {
		return 0; /* 这一轮读不到：下一拍再读 */
	}

	sw = (uint16_t)(v & 0xFFFFu);
	d->statusword = sw;
	d->fb_valid |= DRIVE_FB_STATUSWORD;

	if (ds402_status_parse(sw) == DS402_FAULT) {
		d->pp_wait = 0u;
		return -1;
	}
	if ((sw & DS402_SW_TARGET_REACHED) == 0u) {
		d->pp_seen_low = 1u; /* 目标确实还没到：下一次读到 1 才算到位 */
		return 0;
	}
	if (d->pp_seen_low == 0u) {
		/* bit10 一开始就是 1：可能目标等于当前位置（距离 0），也可能是旧值。
		 * 读 6064 分辨。 */
		if (d->od->read(d->od->ctx, DS402_OD_POSITION_ACTUAL, 0u, &v, &len,
				DRIVE_MON_TIMEOUT_MS) != 0) {
			return 0;
		}
		d->position = (int32_t)v;
		d->fb_valid |= DRIVE_FB_POSITION;
		if (d->position != d->pp_goal) {
			return 0; /* 不在目标上：继续等真正的到位 */
		}
	}

	d->pp_wait = 0u;
	return 1;
}

static int pp_busy(const struct drive *d)
{
	return d != NULL && d->pp_wait != 0u;
}

static const struct mode_ops PP_OPS = {
	.id = DS402_MODE_PP,
	.begin = pp_begin,
	.poll = pp_poll,
	.busy = pp_busy,
};

const struct mode_ops *mode_pp_ops(void)
{
	return &PP_OPS;
}
