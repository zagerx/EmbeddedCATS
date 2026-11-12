/*
 * state_disable.c — 断使能：写 6040 = 00 并等 Switch on disabled
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_disable_write - 写断使能控制字
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_disable_write(struct sm_hsm *const me, const struct sm_event *const e)
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
			return SM_TRAN(&st_idle);
		}
		d->operation.deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		return SM_TRAN(&st_disable_read);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_disable_read - 等 Switch on disabled（0x0040）
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 到位迁回 st_idle；超时判失败。
 */
sm_ret_t st_disable_read(struct sm_hsm *const me, const struct sm_event *const e)
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
		if (cia402_parse((uint16_t)r->value) == CIA402_SWITCH_ON_DISABLED) {
			dev_finish(d, CIA402_OP_DONE, CIA402_OP_FAIL_NONE);
			return SM_TRAN(&st_idle);
		}
		if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_TIMEOUT);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_disable_read);
	}

	default:
		return SM_SUPER(&st_running);
	}
}
