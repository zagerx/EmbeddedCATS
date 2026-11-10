/*
 * device_pp.c — 执行可取消的 CiA 402 PP 绝对位置运动
 *
 * 写入运动参数后以 6040 bit4 上升沿触发，并等待 6041 bit10 到位。
 * 本流程由服务线程同步执行。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <cia402/od.h>
#include <cia402/pds.h>

/**
 * cia402_device_pp_move - 执行一次 PP 绝对位置运动
 * @d: 设备对象
 * @target: 目标位置
 * @vel: 轮廓速度
 * @acc: 轮廓加速度
 * @dec: 轮廓减速度
 *
 * Return: 0 到位；-1 对象访问失败、驱动器故障、取消或超时。
 */
int cia402_device_pp_move(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			  int32_t dec)
{
	const uint32_t deadline = cia402_device_now_ms(d) + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
	uint8_t seen_low = 0u;
	uint16_t sw = 0u;

	if (cia402_device_od_write(d, CIA402_OD_PROFILE_VELOCITY, 0u, (uint32_t)vel, 4u) != 0) {
		return -1;
	}
	if (cia402_device_od_write(d, CIA402_OD_PROFILE_ACCEL, 0u, (uint32_t)acc, 4u) != 0) {
		return -1;
	}
	if (cia402_device_od_write(d, CIA402_OD_PROFILE_DECEL, 0u, (uint32_t)dec, 4u) != 0) {
		return -1;
	}
	if (cia402_device_od_write(d, CIA402_OD_TARGET_POSITION, 0u, (uint32_t)target, 4u) != 0) {
		return -1;
	}

	/* bit4 上升沿触发新目标，紧接着落沿（DS 402 要求边沿而非电平） */
	if (cia402_device_write_controlword(
		    d, (uint16_t)(d->controlword | CIA402_CW_NEW_SETPOINT)) != 0) {
		return -1;
	}
	if (cia402_device_write_controlword(
		    d, (uint16_t)(d->controlword & (uint16_t)~CIA402_CW_NEW_SETPOINT)) != 0) {
		return -1;
	}

	for (;;) {
		uint32_t v = 0u;
		uint8_t len = 0u;

		if (cia402_device_read_statusword(d, &sw) != 0) {
			return -1;
		}
		if (cia402_parse(sw) == CIA402_FAULT) {
			return -1;
		}
		if (cia402_device_cancelled(d) != 0) {
			return -1;
		}
		if ((sw & CIA402_SW_TARGET_REACHED) == 0u) {
			seen_low = 1u; /* 目标确实还没到：下一次读到 1 才算到位 */
		} else if (seen_low != 0u) {
			return 0;
		} else {
			/*
			 * bit10 一开始就是 1：可能目标等于当前位置（距离 0），
			 * 也可能是上一条运动的旧值。读 6064 分辨。
			 */
			if (cia402_device_od_read(d, CIA402_OD_POSITION_ACTUAL, 0u, &v, &len) !=
			    0) {
				return -1;
			}
			pthread_mutex_lock(&d->lock);
			d->position = (int32_t)v;
			pthread_mutex_unlock(&d->lock);
			if ((int32_t)v == target) {
				return 0;
			}
		}
		if ((int32_t)(cia402_device_now_ms(d) - deadline) >= 0) {
			return -1;
		}
	}
}
