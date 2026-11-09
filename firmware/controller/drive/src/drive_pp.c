/*
 * PP 绝对位置运动：写运动参数和目标，以 6040 bit4 上升沿触发。
 * 等待 6041 bit10 到位；同步执行并可被取消。
 */

#include "drive_internal.h"

#include <ds402/od.h>
#include <ds402/pds.h>

int drive_pp_move(struct drive *d, int32_t target, int32_t vel, int32_t acc, int32_t dec)
{
	const uint32_t deadline = drive_now_ms(d) + DRIVE_PP_ARRIVE_TIMEOUT_MS;
	uint8_t seen_low = 0u;
	uint16_t sw = 0u;

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

	for (;;) {
		uint32_t v = 0u;
		uint8_t len = 0u;

		if (drive_read_statusword(d, &sw) != 0) {
			return -1;
		}
		if (ds402_parse(sw) == DS402_FAULT) {
			return -1;
		}
		if (drive_cancelled(d) != 0) {
			return -1;
		}
		if ((sw & DS402_SW_TARGET_REACHED) == 0u) {
			seen_low = 1u; /* 目标确实还没到：下一次读到 1 才算到位 */
		} else if (seen_low != 0u) {
			return 0;
		} else {
			/*
			 * bit10 一开始就是 1：可能目标等于当前位置（距离 0），
			 * 也可能是上一条运动的旧值。读 6064 分辨。
			 */
			if (drive_od_read(d, DS402_OD_POSITION_ACTUAL, 0u, &v, &len) != 0) {
				return -1;
			}
			pthread_mutex_lock(&d->lock);
			d->position = (int32_t)v;
			d->fb_valid |= DRIVE_FB_POSITION;
			pthread_mutex_unlock(&d->lock);
			if ((int32_t)v == target) {
				return 0;
			}
		}
		if ((int32_t)(drive_now_ms(d) - deadline) >= 0) {
			return -1;
		}
	}
}
