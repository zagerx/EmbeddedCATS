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

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
			return SM_TRAN(&st_idle);
		}
		d->wait_deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
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

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		dev_cache_statusword(d, (uint16_t)r->value);
		if (cia402_parse((uint16_t)r->value) == CIA402_SWITCH_ON_DISABLED) {
			dev_finish(d, 0, 0u);
			return SM_TRAN(&st_idle);
		}
		if ((int32_t)(d->now_ms - d->wait_deadline) >= 0) {
			dev_finish(d, -1, 0u);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_disable_read);
	}

	default:
		return SM_SUPER(&st_running);
	}
}
