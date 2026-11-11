/*
 * state_pp.c — PP 绝对位置运动
 *
 * 写四个参数 → 6040 bit4 上升沿（再落沿）触发 → 读 6041 等到位。
 *
 * 到位判定要求"先见到 bit10 = 0，再见到 bit10 = 1"：否则刚触发那一拍
 * 读到的可能是上一条运动的旧值。若 bit10 一开始就是 1，则读 6064 与目标
 * 比对，分辨"目标等于当前位置"和"旧值未清"。
 *
 * 每笔事务之间只隔一拍：服务线程可以在等待期间轮转其它设备。
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
		dev_write(d, CIA402_OD_PROFILE_VELOCITY, (uint32_t)d->vel, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
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
		dev_write(d, CIA402_OD_PROFILE_ACCEL, (uint32_t)d->acc, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
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
		dev_write(d, CIA402_OD_PROFILE_DECEL, (uint32_t)d->dec, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
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
		d->pp_tgt = d->tgt;
		dev_write(d, CIA402_OD_TARGET_POSITION, (uint32_t)d->tgt, 4u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
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
		dev_write(d, CIA402_OD_CONTROLWORD, d->controlword | CIA402_CW_NEW_SETPOINT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
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
			  d->controlword & (uint16_t)~CIA402_CW_NEW_SETPOINT, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
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

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);
		uint16_t sw;

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		sw = (uint16_t)r->value;
		dev_cache_statusword(d, sw);
		if (cia402_parse(sw) == CIA402_FAULT) {
			dev_finish(d, -1, 0u);
			return SM_TRAN(&st_idle);
		}
		if ((sw & CIA402_SW_TARGET_REACHED) == 0u) {
			d->pp_seen_low = 1u; /* 目标确实还没到 */
		} else if (d->pp_seen_low != 0u) {
			dev_finish(d, 0, 0u);
			return SM_TRAN(&st_idle);
		} else {
			/* bit10 一开始就是 1：读 6064 分辨"目标等于当前位置"和旧值 */
			return SM_TRAN(&st_pp_read_position);
		}
		if ((int32_t)(d->now_ms - d->pp_deadline) >= 0) {
			dev_finish(d, -1, 0u);
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

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		pthread_mutex_lock(&d->lock);
		d->position = (int32_t)r->value;
		pthread_mutex_unlock(&d->lock);
		if ((int32_t)r->value == d->pp_tgt) {
			dev_finish(d, 0, 0u);
			return SM_TRAN(&st_idle);
		}
		if ((int32_t)(d->now_ms - d->pp_deadline) >= 0) {
			dev_finish(d, -1, 0u);
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
	d->pp_seen_low = 0u;
	d->pp_deadline = d->now_ms + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
}
