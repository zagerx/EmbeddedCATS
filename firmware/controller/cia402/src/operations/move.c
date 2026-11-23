/*
 * move.c — PP 绝对位置运动操作
 *
 * 写四个参数 → 6040 bit4 上升沿（再落沿）触发 → 读 6041 等 bit10。
 *
 * 到位只信驱动器的状态字 bit10。写 6040 是收到应答才算成功的，应答之前驱动器
 * 已经处理过该控制字，所以之后读到的 bit10 属于本次运动，不会再拿到上一条的
 * 陈旧值。位置由 TPDO 反馈另行提供，不参与到位判定。
 *
 * 每笔事务分轮推进：工作线程可以在等待期间处理其它设备。
 *
 * Author: zager
 */

#include "../states/states.h"

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
		return SM_SUPER(&st_operation_active);
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
		return SM_SUPER(&st_operation_active);
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
		return SM_SUPER(&st_operation_active);
	}
}

/**
 * st_pp_write_target - 写 607A 目标位置
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_pp_write_target(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
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
		return SM_SUPER(&st_operation_active);
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
		dev_write(d, CIA402_OD_CONTROLWORD, d->cw.confirmed | CIA402_CW_NEW_SETPOINT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_trigger_low);

	default:
		return SM_SUPER(&st_operation_active);
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
			  d->cw.confirmed & (uint16_t)~CIA402_CW_NEW_SETPOINT, 2u,
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
		return SM_SUPER(&st_operation_active);
	}
}

/**
 * st_pp_read_status - 读 6041 等驱动器报到位
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 到位只信状态字 bit10：驱动器说到了就是到了，不再拿 6064 与目标比对。
 *
 * Return: bit10 = 1 迁回 st_idle 报成功；故障或超时报失败。
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
		if (cia402_parse(sw) == CIA402_FAULT) {
			dev_finish(d, CIA402_OUTCOME_FAILED, CIA402_FAIL_DRIVER);
			return SM_TRAN(&st_idle);
		}
		if ((sw & CIA402_SW_TARGET_REACHED) != 0u) {
			dev_finish(d, CIA402_OUTCOME_DONE, CIA402_FAIL_NONE);
			return SM_TRAN(&st_idle);
		}
		if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			dev_finish(d, CIA402_OUTCOME_FAILED, CIA402_FAIL_TIMEOUT);
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_pp_read_status);
	}

	default:
		return SM_SUPER(&st_operation_active);
	}
}

static void arm_arrive(struct cia402_device *d)
{
	d->operation.deadline = d->now_ms + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
}
