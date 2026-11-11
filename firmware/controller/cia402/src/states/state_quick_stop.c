/*
 * state_quick_stop.c — 快速停：6040 = 02，等驱动器自己停下，再断使能
 *
 * 这条链即使中途失败也要走完：写失败仍去读状态，读失败仍去断使能。
 * 安全动作不允许因为一笔事务没成而停在"还在使能"的状态。
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
		d->wait_deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
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
		if ((int32_t)(d->now_ms - d->wait_deadline) >= 0) {
			return SM_TRAN(&st_qstop_disable);
		}
		return SM_TRAN(&st_qstop_read);
	}

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
			dev_finish(d, -1, DEV_DONE_EVT(e)->abort_code);
		} else {
			dev_finish(d, 0, 0u);
		}
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&st_running);
	}
}
