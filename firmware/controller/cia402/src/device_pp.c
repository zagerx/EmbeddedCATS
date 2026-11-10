/*
 * device_pp.c — CiA 402 PP 绝对位置运动：一段启动 + 每拍推进一步
 *
 * 启动写参数并以 6040 bit4 上升沿触发目标，然后立刻返回；到位与否由
 * cia402_device_pp_step() 每拍读一次 6041 bit10 判定。
 *
 * 之所以不等在启动里：服务线程是逐轴轮转的，一个轴阻塞到位会挡住另一个轴。
 * 拆开后两轴在同一拍里各推进一次，才真正同时运动。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <cia402/od.h>
#include <cia402/pds.h>

/**
 * cia402_device_pp_begin - 下发 PP 参数并触发新目标
 * @d: 设备对象
 * @target: 目标位置
 * @vel: 轮廓速度
 * @acc: 轮廓加速度
 * @dec: 轮廓减速度
 * @now_ms: 当前时刻，用于算到位超时
 *
 * 只让驱动器开始走，不等到位。
 *
 * Return: 0 已触发；-1 对象访问失败。
 */
int cia402_device_pp_begin(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			   int32_t dec, uint32_t now_ms)
{
	d->pp_run = 0u; /* 触发失败时不留半截进度 */

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

	/* 目标记在服务线程私有字段里：调用方等待期间改 tgt 不影响本次判定 */
	d->pp_tgt = target;
	d->pp_seen_low = 0u;
	d->pp_deadline = now_ms + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
	d->pp_run = 1u;
	return 0;
}

/**
 * cia402_device_pp_step - 推进一步到位判定
 * @d: 设备对象
 * @now_ms: 当前时刻
 *
 * 每拍只读一次状态字，不持有任何等待。
 *
 * Return: 1 还在走；0 到位；-1 驱动器故障、被取消、访问失败或超时。
 */
int cia402_device_pp_step(struct cia402_device *d, uint32_t now_ms)
{
	uint16_t sw = 0u;

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
		d->pp_seen_low = 1u; /* 目标确实还没到：下一次读到 1 才算到位 */
	} else if (d->pp_seen_low != 0u) {
		return 0;
	} else {
		/*
		 * bit10 一开始就是 1：可能目标等于当前位置（距离 0），
		 * 也可能是上一条运动的旧值。读 6064 分辨。
		 */
		uint32_t v = 0u;
		uint8_t len = 0u;

		if (cia402_device_od_read(d, CIA402_OD_POSITION_ACTUAL, 0u, &v, &len) != 0) {
			return -1;
		}
		pthread_mutex_lock(&d->lock);
		d->position = (int32_t)v;
		pthread_mutex_unlock(&d->lock);
		if ((int32_t)v == d->pp_tgt) {
			return 0;
		}
	}
	if ((int32_t)(now_ms - d->pp_deadline) >= 0) {
		return -1;
	}
	return 1;
}
