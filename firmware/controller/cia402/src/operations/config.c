/*
 * config.c — PP 配置操作：写 6060 并回读 6061 确认模式
 *
 * 模式设不进去就等于后面所有 PP 都跑错，所以这一步必须回读比对；
 * 回读值不等于请求值即判失败，重试由上层决定。
 *
 * 两步顺序推进：步骤号在 operation.stage，读到值才决定成功还是失败。
 *
 * Author: zager
 */

#include "../states/states.h"

enum config_stage {
	CONFIG_WRITE_MODE = 0,
	CONFIG_READ_MODE,
};

static sm_state_t config_step(struct cia402_device *d, const struct sdo_result *result);

/**
 * st_config - 写模式并回读确认
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_config(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 只记步骤：本事件里发起的事务失败无法转移（内核丢弃 ENTRY 返回值） */
		d->operation.stage = CONFIG_WRITE_MODE;
		return SM_HANDLED();

	case CIA402_SIG_TICK:
		/* poll 只在通道空闲时送 TICK，正是可以发起事务的时刻 */
		next = config_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = config_step(d, DEV_SDO_RESULT(e));
		break;

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * config_step - 判上一步结果并推进
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入流程
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t config_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		if (d->operation.stage == CONFIG_READ_MODE) {
			/* 回读值必须等于请求值，否则模式没设进去 */
			if ((result->value & 0xFFu) == CIA402_MODE_PP) {
				dev_done(d);
			} else {
				dev_finish(d, CIA402_FAIL_CHECK);
			}
			return &st_idle;
		}
		d->operation.stage = CONFIG_READ_MODE; /* 写成功：去回读 */
	}

	if (d->operation.stage == CONFIG_WRITE_MODE) {
		return dev_start_write(d, CIA402_OD_MODE, CIA402_MODE_PP, 1u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
	return dev_start_read(d, CIA402_OD_MODE_ACTUAL, CIA402_DEVICE_SDO_TIMEOUT_MS);
}
