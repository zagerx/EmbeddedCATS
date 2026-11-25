/*
 * enable.c — 使能操作：故障复位 → Shutdown → Switch on → Enable operation
 *
 * 每一步都是「写一条控制字，然后反复读 6041 等它到位」。读到意外状态就按
 * PDS 现状重新选路（例如中途报故障就转复位链），只有超时才判失败。
 *
 * 状态字是从站状态的投影，本层只按 6041 决定下一步。
 *
 * 十个步骤在 operation.stage 里顺序推进：写类步骤成功后进入下一步，
 * 读类步骤读到期望状态才前进，读到意外状态则由路由重新选路。
 *
 * Author: zager
 */

#include "../states/states.h"

/* 一步一笔事务；读类步骤的下一步由 enable_route / enable_wait 决定 */
enum enable_stage {
	ENABLE_READ_PDS = 0,
	ENABLE_RESET_LOW,
	ENABLE_RESET_HIGH,
	ENABLE_WAIT_RESET,
	ENABLE_WRITE_SHUTDOWN,
	ENABLE_WAIT_READY,
	ENABLE_WRITE_SWITCH_ON,
	ENABLE_WAIT_SWITCHED_ON,
	ENABLE_WRITE_ENABLE_OP,
	ENABLE_WAIT_ENABLED,
	ENABLE_FINISH /* 只作「下一步即收尾」的哨兵，不会停在这里 */
};

static sm_state_t enable_step(struct cia402_device *d, const struct sdo_result *result);
static sm_state_t enable_judge(struct cia402_device *d, uint16_t sw);
static sm_state_t enable_route(struct cia402_device *d, enum cia402_pds_state pds);
static sm_state_t enable_wait(struct cia402_device *d, uint16_t sw, enum cia402_pds_state want,
			      enum enable_stage next);
static void enable_advance(struct cia402_device *d);
static void enable_issue(struct cia402_device *d);
static int enable_expired(const struct cia402_device *d);

/**
 * st_enable - PDS 使能链
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_enable(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		d->operation.stage = ENABLE_READ_PDS;
		next = enable_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = enable_step(d, DEV_SDO_RESULT(e));
		break;

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * enable_step - 判上一步结果并推进
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入流程
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t enable_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		sm_state_t end;

		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		end = enable_judge(d, (uint16_t)result->value);
		if (end != NULL) {
			return end;
		}
	}
	enable_issue(d);
	return NULL;
}

/**
 * enable_judge - 判一笔成功事务的结论
 * @d: 设备对象
 * @sw: 读到的原始值（写类步骤不使用）
 *
 * Return: 要迁移到的状态；NULL 表示已改好 stage，继续本流程。
 */
static sm_state_t enable_judge(struct cia402_device *d, uint16_t sw)
{
	switch (d->operation.stage) {
	case ENABLE_READ_PDS:
		return enable_route(d, cia402_parse(sw));
	case ENABLE_WAIT_RESET:
		if (cia402_parse(sw) != CIA402_FAULT) {
			return enable_route(d, cia402_parse(sw));
		}
		if (enable_expired(d)) {
			dev_finish(d, CIA402_FAIL_DRIVER); /* 一直是 FAULT：复位无效 */
			return &st_idle;
		}
		return NULL; /* 还在 FAULT：再读一次 */
	case ENABLE_WAIT_READY:
		return enable_wait(d, sw, CIA402_READY, ENABLE_WRITE_SWITCH_ON);
	case ENABLE_WAIT_SWITCHED_ON:
		return enable_wait(d, sw, CIA402_SWITCHED_ON, ENABLE_WRITE_ENABLE_OP);
	case ENABLE_WAIT_ENABLED:
		return enable_wait(d, sw, CIA402_OP_ENABLED, ENABLE_FINISH);
	default: /* 写类步骤：成功即前进 */
		enable_advance(d);
		return NULL;
	}
}

/**
 * enable_route - 按当前 PDS 状态重新选路
 * @d: 设备对象
 * @pds: 6041 解析出的 PDS 状态
 *
 * Return: 要迁移到的状态；NULL 表示已改好 stage，继续本流程。
 */
static sm_state_t enable_route(struct cia402_device *d, enum cia402_pds_state pds)
{
	switch (pds) {
	case CIA402_FAULT:
		d->operation.stage = ENABLE_RESET_LOW;
		break;
	case CIA402_NOT_READY:
	case CIA402_SWITCH_ON_DISABLED:
		d->operation.stage = ENABLE_WRITE_SHUTDOWN;
		break;
	case CIA402_READY:
		d->operation.stage = ENABLE_WRITE_SWITCH_ON;
		break;
	case CIA402_SWITCHED_ON:
		d->operation.stage = ENABLE_WRITE_ENABLE_OP;
		break;
	case CIA402_OP_ENABLED:
		dev_done(d); /* 已经在使能态：不用再发帧 */
		return &st_idle;
	default:
		dev_finish(d, CIA402_FAIL_DRIVER);
		return &st_idle;
	}
	return NULL;
}

/**
 * enable_wait - 等一个期望的 PDS 状态
 * @d: 设备对象
 * @sw: 6041 原始值
 * @want: 期望的 PDS 状态
 * @next: 等到了之后要去的步骤
 *
 * 中途报故障转复位链而不是判失败；一直等不到才按期限判超时。
 *
 * Return: 要迁移到的状态；NULL 表示还没到，再读一次。
 */
static sm_state_t enable_wait(struct cia402_device *d, uint16_t sw, enum cia402_pds_state want,
			      enum enable_stage next)
{
	enum cia402_pds_state pds = cia402_parse(sw);

	if (pds == want) {
		if (next == ENABLE_FINISH) {
			dev_done(d); /* 最后一步：等到了就是使能完成 */
			return &st_idle;
		}
		d->operation.stage = next;
		return NULL;
	}
	if (pds == CIA402_FAULT) {
		d->operation.stage = ENABLE_RESET_LOW; /* 中途报故障：转复位链，不判失败 */
		return NULL;
	}
	if (enable_expired(d)) {
		dev_finish(d, CIA402_FAIL_TIMEOUT);
		return &st_idle;
	}
	return NULL; /* 还没到：重入本状态再读一次 */
}

/**
 * enable_advance - 一笔控制字写成功后进入下一步
 * @d: 设备对象
 */
static void enable_advance(struct cia402_device *d)
{
	switch (d->operation.stage) {
	case ENABLE_RESET_LOW:
		d->operation.stage = ENABLE_RESET_HIGH;
		break;
	case ENABLE_RESET_HIGH:
		d->operation.deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS * 2u;
		d->operation.stage = ENABLE_WAIT_RESET; /* 复位退出 FAULT 稍慢，给两倍期限 */
		break;
	case ENABLE_WRITE_SHUTDOWN:
		d->operation.deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		d->operation.stage = ENABLE_WAIT_READY;
		break;
	case ENABLE_WRITE_SWITCH_ON:
		d->operation.deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		d->operation.stage = ENABLE_WAIT_SWITCHED_ON;
		break;
	default: /* ENABLE_WRITE_ENABLE_OP */
		d->operation.deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		d->operation.stage = ENABLE_WAIT_ENABLED;
		break;
	}
}

/**
 * enable_issue - 发起当前步骤的那笔事务
 * @d: 设备对象
 */
static void enable_issue(struct cia402_device *d)
{
	switch (d->operation.stage) {
	case ENABLE_READ_PDS:
	case ENABLE_WAIT_RESET:
	case ENABLE_WAIT_READY:
	case ENABLE_WAIT_SWITCHED_ON:
	case ENABLE_WAIT_ENABLED:
		dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case ENABLE_RESET_LOW:
		dev_start_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE, 2u,
				CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case ENABLE_RESET_HIGH:
		dev_start_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_FAULT_RESET, 2u,
				CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case ENABLE_WRITE_SHUTDOWN:
		dev_start_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_SHUTDOWN, 2u,
				CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case ENABLE_WRITE_SWITCH_ON:
		dev_start_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_SWITCH_ON, 2u,
				CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	default: /* ENABLE_WRITE_ENABLE_OP */
		dev_start_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP, 2u,
				CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	}
}

static int enable_expired(const struct cia402_device *d)
{
	return (int32_t)(d->now_ms - d->operation.deadline) >= 0;
}
