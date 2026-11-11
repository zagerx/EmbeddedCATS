/*
 * state_enable.c — 使能流程：故障复位 → Shutdown → Switch on → Enable operation
 *
 * 每一步都是「写一条控制字，然后反复读 6041 等它到位」。读到意外状态就按
 * PDS 现状重新选路（例如中途报故障就转复位链），只有超时才判失败。
 *
 * 状态字是**从站**状态的投影：本层不维护自己的迁移表，只按 6041 决定下一步。
 *
 * Author: zager
 */

#include "states.h"

/* 读到一个 PDS 状态后该去哪；OP_ENABLED / 未知状态当场收尾 */
static sm_state_t route_from(struct cia402_device *d, enum cia402_pds_state pds);
/* 等一个期望状态；NULL 表示还没到，调用方重入本状态再读一次 */
static sm_state_t wait_step(struct cia402_device *d, uint16_t sw, enum cia402_pds_state want,
			    sm_state_t next);
/* 记录本步等状态字的期限 */
static void arm_wait(struct cia402_device *d, uint32_t timeout_ms);

/**
 * st_enable_read_pds - 读当前 PDS 状态并选路
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 按 6041 迁到对应步骤；已使能则回 st_idle 报成功。
 */
sm_ret_t st_enable_read_pds(struct sm_hsm *const me, const struct sm_event *const e)
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
		return SM_TRAN(route_from(d, cia402_parse((uint16_t)r->value)));
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_reset_low - 故障复位：先写 00 拉低
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_reset_low(struct sm_hsm *const me, const struct sm_event *const e)
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
		return SM_TRAN(&st_enable_reset_high);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_reset_high - 故障复位：写 bit7 上升沿
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_reset_high(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_FAULT_RESET, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
			return SM_TRAN(&st_idle);
		}
		arm_wait(d, CIA402_DEVICE_STEP_TIMEOUT_MS * 2u); /* 复位退出 FAULT 稍慢 */
		return SM_TRAN(&st_enable_read_reset);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_read_reset - 等驱动器退出 FAULT
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 已退出则按新状态选路；仍在 FAULT 且超时则报失败。
 */
sm_ret_t st_enable_read_reset(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);
		enum cia402_pds_state pds;

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		pds = cia402_parse((uint16_t)r->value);
		dev_cache_statusword(d, (uint16_t)r->value);
		if (pds != CIA402_FAULT) {
			return SM_TRAN(route_from(d, pds));
		}
		if ((int32_t)(d->now_ms - d->job.deadline) >= 0) {
			dev_finish(d, CIA402_RESULT_FAILED, CIA402_FAIL_DRIVER); /* 一直是 FAULT：复位无效 */
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_enable_read_reset);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_write_shutdown - 写 Shutdown（6040 = 06）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_write_shutdown(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_SHUTDOWN, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
			return SM_TRAN(&st_idle);
		}
		arm_wait(d, CIA402_DEVICE_STEP_TIMEOUT_MS);
		return SM_TRAN(&st_enable_read_ready);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_read_ready - 等 Ready（0x0021）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_read_ready(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

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
		next = wait_step(d, (uint16_t)r->value, CIA402_READY, &st_enable_write_switch_on);
		return SM_TRAN((next != NULL) ? next : &st_enable_read_ready);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_write_switch_on - 写 Switch on（6040 = 07）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_write_switch_on(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_SWITCH_ON, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
			return SM_TRAN(&st_idle);
		}
		arm_wait(d, CIA402_DEVICE_STEP_TIMEOUT_MS);
		return SM_TRAN(&st_enable_read_switched_on);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_read_switched_on - 等 Switched on（0x0023）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_read_switched_on(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

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
		next = wait_step(d, (uint16_t)r->value, CIA402_SWITCHED_ON,
				 &st_enable_write_enable_op);
		return SM_TRAN((next != NULL) ? next : &st_enable_read_switched_on);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_write_enable_op - 写 Enable operation（6040 = 0F）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable_write_enable_op(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case SIG_DEV_DONE:
		if (DEV_DONE_EVT(e)->status != 0) {
			dev_fail(d, DEV_DONE_EVT(e));
			return SM_TRAN(&st_idle);
		}
		arm_wait(d, CIA402_DEVICE_STEP_TIMEOUT_MS);
		return SM_TRAN(&st_enable_read_enabled);

	default:
		return SM_SUPER(&st_running);
	}
}

/**
 * st_enable_read_enabled - 等 Operation enabled（0x0027）
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 到位迁回 st_idle 并报成功。
 */
sm_ret_t st_enable_read_enabled(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

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
		next = wait_step(d, (uint16_t)r->value, CIA402_OP_ENABLED, NULL);
		return SM_TRAN((next != NULL) ? next : &st_enable_read_enabled);
	}

	default:
		return SM_SUPER(&st_running);
	}
}

static sm_state_t route_from(struct cia402_device *d, enum cia402_pds_state pds)
{
	switch (pds) {
	case CIA402_FAULT:
		return &st_enable_reset_low;
	case CIA402_NOT_READY:
	case CIA402_SWITCH_ON_DISABLED:
		return &st_enable_write_shutdown;
	case CIA402_READY:
		return &st_enable_write_switch_on;
	case CIA402_SWITCHED_ON:
		return &st_enable_write_enable_op;
	case CIA402_OP_ENABLED:
		dev_finish(d, CIA402_RESULT_DONE, CIA402_FAIL_NONE); /* 已经在使能态：不用再发帧 */
		return &st_idle;
	default:
		dev_finish(d, CIA402_RESULT_FAILED, CIA402_FAIL_DRIVER);
		return &st_idle;
	}
}

static sm_state_t wait_step(struct cia402_device *d, uint16_t sw, enum cia402_pds_state want,
			    sm_state_t next)
{
	enum cia402_pds_state pds = cia402_parse(sw);

	dev_cache_statusword(d, sw);
	if (pds == want) {
		if (next != NULL) {
			return next;
		}
		dev_finish(d, CIA402_RESULT_DONE, CIA402_FAIL_NONE); /* 最后一步：等到了就是使能完成 */
		return &st_idle;
	}
	if (pds == CIA402_FAULT) {
		return &st_enable_reset_low; /* 中途报故障：转复位链，不判失败 */
	}
	if ((int32_t)(d->now_ms - d->job.deadline) >= 0) {
		dev_finish(d, CIA402_RESULT_FAILED, CIA402_FAIL_TIMEOUT);
		return &st_idle;
	}
	return NULL; /* 还没到：调用方重入本状态再读一次 */
}

static void arm_wait(struct cia402_device *d, uint32_t timeout_ms)
{
	d->job.deadline = d->now_ms + timeout_ms;
}
