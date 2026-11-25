/*
 * disable.c — 断使能操作：写 6040 = 00 并等 Switch on disabled
 *
 * 两步顺序推进：写控制字，再反复读 6041 直到进入禁止启动状态。
 *
 * Author: zager
 */

#include "../states/states.h"

enum disable_stage {
	DISABLE_WRITE = 0,
	DISABLE_READ,
};

static sm_state_t disable_step(struct cia402_device *d, const struct sdo_result *result);

/**
 * st_disable - 断使能：写 00 并等状态字
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_disable(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		d->operation.stage = DISABLE_WRITE;
		next = disable_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = disable_step(d, DEV_SDO_RESULT(e));
		break;

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * disable_step - 判上一步结果并推进
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入流程
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t disable_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		if (d->operation.stage == DISABLE_WRITE) {
			d->operation.stage = DISABLE_READ;
			d->operation.deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		} else if (cia402_parse((uint16_t)result->value) == CIA402_SWITCH_ON_DISABLED) {
			dev_done(d);
			return &st_idle;
		} else if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			dev_finish(d, CIA402_FAIL_TIMEOUT);
			return &st_idle;
		}
	}

	if (d->operation.stage == DISABLE_WRITE) {
		dev_start_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE, 2u,
				CIA402_DEVICE_SDO_TIMEOUT_MS);
	} else {
		dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
	return NULL;
}
