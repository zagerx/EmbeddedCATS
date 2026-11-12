/*
 * state_config.c — 写 6060 并回读 6061 确认模式
 *
 * 模式设不进去就等于后面所有 PP 都跑错，所以这一步必须回读比对；
 * 回读值不等于请求值即判失败，不做重试（重试由上层决定）。
 *
 * Author: zager
 */

#include "states.h"

/* 写 6060 与读 6061 之间没有额外等待 */

/**
 * st_config_write_mode - 写目标模式
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 事务完成后迁 st_config_read_mode。
 */
sm_ret_t st_config_write_mode(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_MODE, CIA402_MODE_PP, 1u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_config_read_mode);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_config_read_mode - 回读模式并比对
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 匹配迁回 st_idle 并报成功，否则报失败。
 */
sm_ret_t st_config_read_mode(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, CIA402_OD_MODE_ACTUAL, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status != 0) {
			dev_fail(d, r);
		} else if ((r->value & 0xFFu) == CIA402_MODE_PP) {
			dev_finish(d, CIA402_OP_DONE, CIA402_OP_FAIL_NONE);
		} else {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_CHECK); /* 模式没设进去 */
		}
		return SM_TRAN(&st_idle);
	}

	default:
		return SM_SUPER(&st_running);
	}
}
