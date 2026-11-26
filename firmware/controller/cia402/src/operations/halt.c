/*
 * halt.c — 减速停操作：6040 bit8 置位，等停稳后清位
 *
 * 清 bit8 是为了保持使能（有保持力矩），停稳后可以立刻接下一句运动。
 *
 * 三步顺序推进：置 halt → 读 6041 等 bit10（或期限到）→ 清 halt。
 *
 * Author: zager
 */

#include "../states/states.h"

enum halt_stage {
	HALT_WRITE = 0,
	HALT_READ,
	HALT_CLEAR,
};

static sm_state_t halt_step(struct cia402_device *d, const struct sdo_result *result);

/**
 * st_halt - 减速停并保持使能
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_halt(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		d->operation.stage = HALT_WRITE;
		next = halt_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = halt_step(d, DEV_SDO_RESULT(e));
		break;

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * halt_step - 判上一步结果并推进一步
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入流程
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t halt_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		switch (d->operation.stage) {
		case HALT_WRITE:
			d->operation.stage = HALT_READ;
			d->operation.deadline = d->now_ms + CIA402_DEVICE_HALT_TIMEOUT_MS;
			break;
		case HALT_READ:
			if ((result->value & CIA402_SW_TARGET_REACHED) != 0u) {
				d->operation.stage = HALT_CLEAR;
				break;
			}
			if (cia402_parse((uint16_t)result->value) == CIA402_FAULT ||
			    (int32_t)(d->now_ms - d->operation.deadline) >= 0) {
				dev_finish(d, CIA402_FAIL_DRIVER);
				return &st_idle;
			}
			break; /* 还没停稳：再读一次 */
		default:       /* HALT_CLEAR */
			dev_done(d);
			return &st_idle;
		}
	}

	switch (d->operation.stage) {
	case HALT_WRITE:
		return dev_start_write(d, CIA402_OD_CONTROLWORD, d->cw.confirmed | CIA402_CW_HALT,
				       2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
	case HALT_READ:
		return dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
	default: /* HALT_CLEAR */
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       d->cw.confirmed & (uint16_t)~CIA402_CW_HALT, 2u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
}
