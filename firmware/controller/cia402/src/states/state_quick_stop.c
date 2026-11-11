/*
 * state_quick_stop.c — 快速停：6040 = 02，等驱动器自己停下，再断使能
 *
 * 这条链即使中途失败也要走完：写失败仍去读状态，读失败仍去断使能。
 * 安全动作不允许因为一笔事务没成而停在「还在使能」的状态。
 *
 * 因此三个状态都自己处理 SIG_DEV_CANCEL（单笔超时/被抢断），而不是让
 * st_running 的统一退出把整条链一起收掉；只有最后一步失败才报失败。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_qstop_write - 写快速停控制字
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 无论写成功与否都迁 st_qstop_read。
 */
sm_ret_t st_qstop_write(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		/* 写失败也继续读：断使能链不因一笔事务失败而中断 */
		d->job.deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
		return SM_TRAN(&st_qstop_read);

	case SIG_DEV_CANCEL:
		/* 单笔事务超时或被抢断：同样往下走，不允许停在「还在使能」 */
		d->job.deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
		return SM_TRAN(&st_qstop_read);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_qstop_read - 等驱动器停下（Switch on disabled 或 Not ready）
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 停下或超时都迁 st_qstop_disable，继续走断使能。
 */
sm_ret_t st_qstop_read(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);

		if (r->status == 0) {
			enum cia402_pds_state pds = cia402_parse((uint16_t)r->value);

			dev_cache_statusword(d, (uint16_t)r->value);
			if (pds == CIA402_SWITCH_ON_DISABLED || pds == CIA402_NOT_READY) {
				return SM_TRAN(&st_qstop_disable);
			}
		}
		/* 读失败也判期限：停下就不再重读，直接断使能 */
		if ((int32_t)(d->now_ms - d->job.deadline) >= 0) {
			return SM_TRAN(&st_qstop_disable);
		}
		return SM_TRAN(&st_qstop_read);
	}

	case SIG_DEV_CANCEL:
		/* 超时或被抢断：不等了，直接断使能 */
		return SM_TRAN(&st_qstop_disable);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_qstop_disable - 断使能并收尾
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_qstop_disable(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
		} else {
			dev_finish(d, CIA402_RESULT_DONE, CIA402_FAIL_NONE);
		}
		return SM_TRAN(&st_idle);

	case SIG_DEV_CANCEL:
		/* 最后一环也失败：这里是唯一该报失败的地方 */
		dev_fail(d, &d->xfer.result);
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&st_running);
	}
}
