/*
 * state_pp.c — PP 绝对位置运动
 *
 * 写四个参数 → 6040 bit4 上升沿（再落沿）触发 → 读 6041 等到位。
 *
 * 到位判定要求"先见到 bit10 = 0，再见到 bit10 = 1"：否则刚触发那一拍
 * 读到的可能是上一条运动的旧值。若 bit10 一开始就是 1，则读 6064 与目标
 * 比对，分辨"目标等于当前位置"和"旧值未清"。
 *
 * 每笔事务分轮推进：工作线程可以在等待期间处理其它设备。
 *
 * Author: zager
 */

#include "states.h"

static void arm_arrive(struct cia402_device *d);

/**
 * st_pp_write_velocity - 写 6081 轮廓速度
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_pp_write_velocity(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_PROFILE_VELOCITY, (uint32_t)d->operation.pp.velocity, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_write_accel);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_write_accel - 写 6083 轮廓加速度
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_pp_write_accel(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_PROFILE_ACCEL, (uint32_t)d->operation.pp.accel, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_write_decel);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_write_decel - 写 6084 轮廓减速度
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_pp_write_decel(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_PROFILE_DECEL, (uint32_t)d->operation.pp.decel, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_write_target);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_write_target - 写 607A 目标位置
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 目标值同时存进 pp_tgt：之后判定到位用它，不受调用方改 tgt 影响。
 */
sm_ret_t st_pp_write_target(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		d->operation.pp.issued = d->operation.pp.target;
		dev_write(d, CIA402_OD_TARGET_POSITION, (uint32_t)d->operation.pp.target, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_trigger_high);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_trigger_high - 6040 bit4 上升沿触发新目标
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_pp_trigger_high(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, d->sdo.cw_ok | CIA402_CW_NEW_SETPOINT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_trigger_low);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_trigger_low - 6040 bit4 落沿（DS 402 要求边沿而非电平）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_pp_trigger_low(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD,
			  d->sdo.cw_ok & (uint16_t)~CIA402_CW_NEW_SETPOINT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		arm_arrive(d);
		return SM_TRAN(&st_pp_read_status);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_read_status - 读到没到位
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 到位迁回 st_idle 报成功；故障或超时报失败。
 */
sm_ret_t st_pp_read_status(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);
		uint16_t sw;

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		sw = (uint16_t)r->value;
		dev_cache_statusword(d, sw);
		if (cia402_parse(sw) == CIA402_FAULT) {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_DRIVER);
			return SM_TRAN(&st_idle);
		}
		if ((sw & CIA402_SW_TARGET_REACHED) == 0u) {
			d->operation.pp.seen_low = 1u; /* 目标确实还没到 */
		} else if (d->operation.pp.seen_low != 0u) {
			dev_finish(d, CIA402_OP_DONE, CIA402_OP_FAIL_NONE);
			return SM_TRAN(&st_idle);
		} else {
			/* bit10 一开始就是 1：读 6064 分辨"目标等于当前位置"和旧值 */
			return SM_TRAN(&st_pp_read_position);
		}
		if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_TIMEOUT);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_read_status);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_pp_read_position - 读 6064 与目标比对
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 位置等于目标即到位；否则回 st_pp_read_status 继续等。
 */
sm_ret_t st_pp_read_position(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, CIA402_OD_POSITION_ACTUAL, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		d->feedback.position = (int32_t)r->value;
		if ((int32_t)r->value == d->operation.pp.issued) {
			dev_finish(d, CIA402_OP_DONE, CIA402_OP_FAIL_NONE);
			return SM_TRAN(&st_idle);
		}
		if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_TIMEOUT);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_read_status);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

static void arm_arrive(struct cia402_device *d)
{
	d->operation.pp.seen_low = 0u;
	d->operation.deadline = d->now_ms + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
}
