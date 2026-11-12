/*
 * state_halt.c — 减速停：6040 bit8 置位，等停稳后清位
 *
 * 清 bit8 是为了保持使能（有保持力矩），停稳后可以立刻接下一句运动。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_halt_write - 置 6040 bit8（Halt）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_halt_write(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, d->sdo.cw_ok | CIA402_CW_HALT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		d->operation.deadline = d->now_ms + CIA402_DEVICE_HALT_TIMEOUT_MS;
		return SM_TRAN(&st_halt_read);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_halt_read - 等停稳（bit10）
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 停稳迁 st_halt_clear；故障或超时判失败。
 */
sm_ret_t st_halt_read(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		dev_cache_statusword(d, (uint16_t)r->value);
		if ((r->value & CIA402_SW_TARGET_REACHED) != 0u) {
			return SM_TRAN(&st_halt_clear);
		}
		if (cia402_parse((uint16_t)r->value) == CIA402_FAULT ||
		    (int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_DRIVER);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_halt_read);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_halt_clear - 清 6040 bit8，保持使能
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_halt_clear(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, d->sdo.cw_ok & (uint16_t)~CIA402_CW_HALT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
		} else {
			dev_finish(d, CIA402_OP_DONE, CIA402_OP_FAIL_NONE);
		}
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&st_running);
	}
}
