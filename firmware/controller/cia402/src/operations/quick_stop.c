/*
 * quick_stop.c — 快速停操作：6040 = 02，等驱动器自己停下，再断使能
 *
 * 这条链即使中途失败也要走完：写失败仍去读状态，读失败仍去断使能。
 * 三个状态各自处理 CIA402_SIG_CANCEL，只有最后一步失败才报失败。
 *
 * Author: zager
 */

#include "../states/states.h"

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

	case CIA402_SIG_SDO_DONE:
		/* 写失败也继续读：断使能链不因一笔事务失败而中断 */
		d->operation.deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
		return SM_TRAN(&st_qstop_read);

	case CIA402_SIG_CANCEL:
		/* 事务失败或被抢断：同样往下走 */
		d->operation.deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
		return SM_TRAN(&st_qstop_read);

	default:
		return SM_SUPER(&st_operation_active);
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

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status == 0) {
			enum cia402_pds_state pds = cia402_parse((uint16_t)r->value);

			if (pds == CIA402_SWITCH_ON_DISABLED || pds == CIA402_NOT_READY) {
				return SM_TRAN(&st_qstop_disable);
			}
		}
		/* 读失败也判期限：停下就不再重读，直接断使能 */
		if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			return SM_TRAN(&st_qstop_disable);
		}
		return SM_TRAN(&st_qstop_read);
	}

	case CIA402_SIG_CANCEL:
		/* 超时或被抢断：不等了，直接断使能 */
		return SM_TRAN(&st_qstop_disable);

	default:
		return SM_SUPER(&st_operation_active);
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

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
		} else {
			dev_finish(d, CIA402_OUTCOME_DONE, CIA402_FAIL_NONE);
		}
		return SM_TRAN(&st_idle);

	case CIA402_SIG_CANCEL:
		/* 最后一环也失败：这里是唯一该报失败的地方 */
		dev_fail(d, DEV_CANCEL_RESULT(e));
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&st_operation_active);
	}
}
