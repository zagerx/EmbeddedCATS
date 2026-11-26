/*
 * homing.c — 回原点：切 HM 模式、启动、等结论、收尾
 *
 * 回零由驱动器自己执行（搜开关、减速反向、开关释放处标零）。方法号、两档速度
 * 与加减速（6098h / 6099h / 609Ah）是驱动器侧的持久配置，由
 * apps/tools/drive_setup 写入；主站这里只切模式、发启动、等 6041 的结论。
 *
 * 这台驱动器的 6040 bit4 是**电平触发**：写一次 0x1F 之后必须一直保持到
 * 6041 bit12 / bit13 出现，中途落回 0x0F 会让回零立即中止（2026-11-20 实测）。
 *
 * 收尾统一先落 bit4 再切回 PP 模式：在 HM 模式下带着 bit4 改模式，会被当成
 * PP 的新目标触发，所以顺序不能反。
 *
 * 主流程与中止链是两个状态、两串步骤：
 *   st_home       写模式 → 回读 → 启动 → 等结论
 *   st_home_abort 停电机 → 等停 → 落 bit4 → 切回 PP
 * 被抢占或失败后一律走中止链，才能保证驱动器不会带着 bit4 继续找原点。
 *
 * Author: zager
 */

#include "../states/states.h"

enum home_stage {
	HOME_WRITE_MODE = 0,
	HOME_READ_MODE,
	HOME_START,
	HOME_WAIT,
};

enum home_abort_stage {
	HOME_ABORT_HALT = 0,
	HOME_ABORT_WAIT_STOP,
	HOME_ABORT_RESTORE_CW,
	HOME_ABORT_RESTORE_MODE,
};

static sm_state_t home_step(struct cia402_device *d, const struct sdo_result *result);
static sm_state_t home_judge(struct cia402_device *d, uint16_t sw);
static sm_state_t home_verdict(struct cia402_device *d, uint16_t sw);
static sm_state_t home_issue(struct cia402_device *d);

static sm_state_t home_abort_step(struct cia402_device *d, const struct sdo_result *result);
static sm_state_t home_abort_judge(struct cia402_device *d, const struct sdo_result *result);
static sm_state_t home_abort_issue(struct cia402_device *d);

static sm_state_t home_settle_done(struct cia402_device *d, enum home_abort_stage first);
static sm_state_t home_settle_fail(struct cia402_device *d, enum cia402_fail fail,
				   enum home_abort_stage first);
static void home_note_cancel(struct cia402_device *d, uint8_t reason);
static void home_publish(struct cia402_device *d);

/**
 * st_home - 回零主流程
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 被暂停或急停抢占时不直接退出：先停电机、切回 PP 再收尾，否则驱动器会
 * 带着 bit4 继续找原点。被抢占的中止只取消流程，不交付执行回执。
 */
sm_ret_t st_home(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		d->operation.home.pending = CIA402_OUTCOME_NONE;
		d->operation.home.fail_kind = 0u;
		d->operation.stage = HOME_WRITE_MODE;
		next = home_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = home_step(d, DEV_SDO_RESULT(e));
		break;

	case CIA402_SIG_CANCEL:
		home_note_cancel(d, DEV_CANCEL_REASON(e));
		d->operation.stage = HOME_ABORT_HALT;
		return SM_TRAN(&st_home_abort);

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * st_home_abort - 回零中止链：停电机、落 bit4、切回 PP、交回执
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 起点由调用方设定：有电机在转（等结论、失败）从停电机开始；
 * 还没启动或已经报错（bit13）直接从落 bit4 开始。
 */
sm_ret_t st_home_abort(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		next = home_abort_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = home_abort_step(d, DEV_SDO_RESULT(e));
		break;

	case CIA402_SIG_CANCEL:
		/* 清理链又被抢占：从头重来，旧失败不再补交 */
		home_note_cancel(d, DEV_CANCEL_REASON(e));
		d->operation.stage = HOME_ABORT_HALT;
		return SM_TRAN(&st_home_abort); /* 自迁移：重跑 EXIT/ENTRY，由 ENTRY 重新发帧 */

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * home_step - 判上一步结果并推进主流程
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入流程
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t home_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		sm_state_t end;

		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		end = home_judge(d, (uint16_t)result->value);
		if (end != NULL) {
			return end;
		}
	}
	return home_issue(d);
}

/**
 * home_judge - 判主流程一笔成功事务的结论
 * @d: 设备对象
 * @sw: 读到的原始值（写类步骤不使用）
 *
 * Return: 要迁移到的状态；NULL 表示已改好 stage，继续本流程。
 */
static sm_state_t home_judge(struct cia402_device *d, uint16_t sw)
{
	switch (d->operation.stage) {
	case HOME_WRITE_MODE:
		d->operation.stage = HOME_READ_MODE;
		return NULL;
	case HOME_READ_MODE:
		if ((uint8_t)sw != CIA402_MODE_HM) {
			/* 模式没进去，后面的启动无意义 */
			return home_settle_fail(d, CIA402_FAIL_CHECK, HOME_ABORT_RESTORE_CW);
		}
		d->operation.stage = HOME_START;
		return NULL;
	case HOME_START:
		d->operation.deadline = d->now_ms + CIA402_DEVICE_HOMING_TIMEOUT_MS;
		d->operation.stage = HOME_WAIT;
		return NULL;
	default: /* HOME_WAIT */
		return home_verdict(d, sw);
	}
}

/**
 * home_verdict - 判回零结论
 * @d: 设备对象
 * @sw: 6041 原始值
 *
 * 只看两位：bit12 = 1 回零完成，bit13 = 1 回零错误。
 * 不看 bit11（"正负限位生效"）：这台驱动器在 PP 模式且已使能时该位恒为 1，
 * 与限位输入无关（60FD 的 NOT/POT 均为 0），拿它当判据会让回零一开始就误判。
 *
 * Return: 要迁移到的状态；NULL 表示还没结论，再读一次。
 */
static sm_state_t home_verdict(struct cia402_device *d, uint16_t sw)
{
	if ((sw & CIA402_SW_HOMING_ATTAINED) != 0u) {
		/* 成功：位置已由驱动器置 0，不需要停电机 */
		return home_settle_done(d, HOME_ABORT_RESTORE_CW);
	}
	if ((sw & CIA402_SW_HOMING_ERROR) != 0u) {
		/* bit13 是驱动器判的回零错误（开关信号过冲等），不是 PDS 故障 */
		return home_settle_fail(d, CIA402_FAIL_HOMING, HOME_ABORT_RESTORE_CW);
	}
	if (cia402_parse(sw) == CIA402_FAULT) {
		return home_settle_fail(d, CIA402_FAIL_DRIVER, HOME_ABORT_HALT);
	}
	if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
		return home_settle_fail(d, CIA402_FAIL_TIMEOUT, HOME_ABORT_HALT);
	}
	return NULL;
}

/**
 * home_issue - 发起主流程当前步骤的那笔事务
 * @d: 设备对象
 */
static sm_state_t home_issue(struct cia402_device *d)
{
	switch (d->operation.stage) {
	case HOME_WRITE_MODE:
		return dev_start_write(d, CIA402_OD_MODE, CIA402_MODE_HM, 1u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	case HOME_READ_MODE:
		return dev_start_read(d, CIA402_OD_MODE_ACTUAL, CIA402_DEVICE_SDO_TIMEOUT_MS);
	case HOME_START:
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       (uint32_t)(d->cw.confirmed | CIA402_CW_HOMING_START), 2u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	default: /* HOME_WAIT */
		return dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
}

/**
 * home_abort_step - 判上一步结果并推进中止链
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入中止链
 *
 * 中止链上的事务失败不一律判离线：停不下来也要继续收尾。
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t home_abort_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		sm_state_t end = home_abort_judge(d, result);

		if (end != NULL) {
			return end;
		}
	}
	return home_abort_issue(d);
}

/**
 * home_abort_judge - 判中止链一笔事务的结论
 * @d: 设备对象
 * @result: 事务结果
 *
 * Return: 要迁移到的状态；NULL 表示已改好 stage，继续本链。
 */
static sm_state_t home_abort_judge(struct cia402_device *d, const struct sdo_result *result)
{
	switch (d->operation.stage) {
	case HOME_ABORT_HALT:
		if (result->status != 0) {
			d->operation.stage = HOME_ABORT_RESTORE_MODE; /* 停不下来也得收尾 */
			return NULL;
		}
		d->operation.deadline = d->now_ms + CIA402_DEVICE_HALT_TIMEOUT_MS;
		d->operation.stage = HOME_ABORT_WAIT_STOP;
		return NULL;
	case HOME_ABORT_WAIT_STOP:
		/* bit10（目标到达）或期限到即视为停下；停不下来也照样收尾 */
		if (result->status != 0 || (result->value & CIA402_SW_TARGET_REACHED) != 0u ||
		    (int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			d->operation.stage = HOME_ABORT_RESTORE_CW;
		}
		return NULL;
	case HOME_ABORT_RESTORE_CW:
		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		d->operation.stage = HOME_ABORT_RESTORE_MODE;
		return NULL;
	default: /* HOME_ABORT_RESTORE_MODE */
		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		home_publish(d);
		return &st_idle;
	}
}

/**
 * home_abort_issue - 发起中止链当前步骤的那笔事务
 * @d: 设备对象
 */
static sm_state_t home_abort_issue(struct cia402_device *d)
{
	switch (d->operation.stage) {
	case HOME_ABORT_HALT:
		/*
		 * 必须先从镜像里清掉 bit4 再置 halt：halt 链本身只动 bit8，
		 * bit4 若还挂着，下一次写控制字会把它带回来，驱动器会重启回零。
		 */
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       (d->cw.confirmed & (uint16_t)~CIA402_CW_HOMING_START) |
					       CIA402_CW_HALT,
				       2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
	case HOME_ABORT_WAIT_STOP:
		return dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
	case HOME_ABORT_RESTORE_CW:
		/*
		 * 清两位而不是硬写 0x0F：镜像里可能有别的位（bit9 等），
		 * 硬写会把它们一起抹掉。
		 */
		return dev_start_write(
			d, CIA402_OD_CONTROLWORD,
			d->cw.confirmed & (uint16_t) ~(CIA402_CW_HALT | CIA402_CW_HOMING_START), 2u,
			CIA402_DEVICE_SDO_TIMEOUT_MS);
	default: /* HOME_ABORT_RESTORE_MODE */
		return dev_start_write(d, CIA402_OD_MODE, CIA402_MODE_PP, 1u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
}

/* 记下"回零成功"，并把中止链起点设好 */
static sm_state_t home_settle_done(struct cia402_device *d, enum home_abort_stage first)
{
	d->operation.home.pending = CIA402_OUTCOME_DONE;
	d->operation.stage = (uint8_t)first;
	return &st_home_abort;
}

/* 记下"回零失败"，并把中止链起点设好 */
static sm_state_t home_settle_fail(struct cia402_device *d, enum cia402_fail fail,
				   enum home_abort_stage first)
{
	d->operation.home.pending = CIA402_OUTCOME_FAILED;
	d->operation.home.fail_kind = (uint8_t)fail;
	d->operation.stage = (uint8_t)first;
	return &st_home_abort;
}

/* 被抢占或单笔事务超时：超时属于真实失败，抢占则结论作废 */
static void home_note_cancel(struct cia402_device *d, uint8_t reason)
{
	if (reason == CIA402_CANCEL_TIMEOUT) {
		d->operation.home.pending = CIA402_OUTCOME_FAILED;
		d->operation.home.fail_kind = (uint8_t)CIA402_FAIL_TIMEOUT;
		return;
	}
	d->operation.home.pending = CIA402_OUTCOME_NONE; /* 旧失败属于已取消流程 */
}

/* 中止链走完：按登记的结论交付回执 */
static void home_publish(struct cia402_device *d)
{
	switch (d->operation.home.pending) {
	case CIA402_OUTCOME_DONE:
		dev_done(d);
		break;
	case CIA402_OUTCOME_FAILED:
		dev_finish(d, (enum cia402_fail)d->operation.home.fail_kind);
		break;
	default:
		break; /* 被抢占：这条回零不交付结局 */
	}
	d->operation.home.pending = CIA402_OUTCOME_NONE;
}
