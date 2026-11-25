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
 * Author: zager
 */

#include "../states/states.h"

/* 记下这条回零要报的失败原因，由收尾链最后一步统一落回执 */
static void home_fail(struct cia402_device *d, enum cia402_fail fail);

/**
 * st_home_active - 回零各步骤的公共父状态
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 被暂停或急停抢占时不直接退出：先停电机、切回 PP 再收尾，否则驱动器会
 * 带着 bit4 继续找原点。被抢占的中止只取消流程，不产生执行回执。
 */
sm_ret_t st_home_active(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case CIA402_SIG_CANCEL:
		if (DEV_CANCEL_REASON(e) == CIA402_CANCEL_TIMEOUT) {
			home_fail(d, CIA402_FAIL_TIMEOUT);
		} else {
			d->operation.home.cancelled = 1u;
			d->operation.home.failed = 0u; /* 旧失败属于已取消流程 */
		}
		return SM_TRAN(&st_home_halt);

	default:
		return SM_SUPER(&st_operation_active);
	}
}

/**
 * st_home_write_mode - 切到回零模式（6060 = 6）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_home_write_mode(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_write(d, CIA402_OD_MODE, CIA402_MODE_HM, 1u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_home_read_mode);

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_read_mode - 回读 6061 确认已进入 HM
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_home_read_mode(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_read(d, CIA402_OD_MODE_ACTUAL, CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status != 0) {
			dev_fail(d, r);
			return SM_TRAN(&st_idle);
		}
		if ((uint8_t)r->value != CIA402_MODE_HM) {
			home_fail(d, CIA402_FAIL_CHECK); /* 模式没进去，后面的启动无意义 */
			return SM_TRAN(&st_home_restore_cw);
		}
		return SM_TRAN(&st_home_start);
	}

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_start - 置 6040 bit4 启动回零（此后一直保持）
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_home_start(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       (uint32_t)(d->cw.confirmed | CIA402_CW_HOMING_START), 2u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		d->operation.deadline = d->now_ms + CIA402_DEVICE_HOMING_TIMEOUT_MS;
		return SM_TRAN(&st_home_wait);

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_wait - 反复读 6041 等回零结论
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 只看两位：bit12 = 1 回零完成，bit13 = 1 回零错误。
 * 不看 bit11（"正负限位生效"）：这台驱动器在 PP 模式且已使能时该位恒为 1，
 * 与限位输入无关（60FD 的 NOT/POT 均为 0），拿它当判据会让回零一开始就误判。
 */
sm_ret_t st_home_wait(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);
		uint16_t sw;

		if (r->status != 0) {
			home_fail(d, CIA402_FAIL_TIMEOUT);
			return SM_TRAN(&st_home_halt);
		}
		sw = (uint16_t)r->value;
		if ((sw & CIA402_SW_HOMING_ATTAINED) != 0u) {
			return SM_TRAN(&st_home_restore_cw); /* 成功：位置已由驱动器置 0 */
		}
		if ((sw & CIA402_SW_HOMING_ERROR) != 0u) {
			/* bit13 是驱动器判的回零错误（开关信号过冲等），不是 PDS 故障 */
			home_fail(d, CIA402_FAIL_HOMING);
			return SM_TRAN(&st_home_restore_cw);
		}
		if (cia402_parse(sw) == CIA402_FAULT) {
			home_fail(d, CIA402_FAIL_DRIVER);
			return SM_TRAN(&st_home_halt);
		}
		if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			home_fail(d, CIA402_FAIL_TIMEOUT);
			return SM_TRAN(&st_home_halt);
		}
		return SM_TRAN(&st_home_wait);
	}

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_halt - 中止回零：落 bit4 并置 bit8 减速停
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 必须先从镜像里清掉 bit4 再置 halt：halt 链本身只动 bit8，bit4 若还挂着，
 * 下一次写控制字会把它带回来，驱动器会重启回零。
 */
sm_ret_t st_home_halt(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       (d->cw.confirmed & (uint16_t)~CIA402_CW_HOMING_START) |
					       CIA402_CW_HALT,
				       2u, CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			return SM_TRAN(&st_home_restore_mode); /* 停不下来也得收尾 */
		}
		d->operation.deadline = d->now_ms + CIA402_DEVICE_HALT_TIMEOUT_MS;
		return SM_TRAN(&st_home_halt_wait);

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_halt_wait - 等电机停住
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * bit10（目标到达）或 3 秒期满即视为停下；停不下来也照样收尾。
 */
sm_ret_t st_home_halt_wait(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status != 0) {
			return SM_TRAN(&st_home_restore_cw);
		}
		if ((r->value & CIA402_SW_TARGET_REACHED) != 0u ||
		    (int32_t)(d->now_ms - d->operation.deadline) >= 0) {
			return SM_TRAN(&st_home_restore_cw);
		}
		return SM_TRAN(&st_home_halt_wait);
	}

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_restore_cw - 落 bit4、清 halt，回到可操作但不启动
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 从镜像里清两位，而不是硬写 0x0F：镜像里可能有别的位（bit9 等），
 * 硬写会把它们一起抹掉。
 */
sm_ret_t st_home_restore_cw(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_write(
			d, CIA402_OD_CONTROLWORD,
			d->cw.confirmed & (uint16_t) ~(CIA402_CW_HALT | CIA402_CW_HOMING_START), 2u,
			CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		return SM_TRAN(&st_home_restore_mode);

	default:
		return SM_SUPER(&st_home_active);
	}
}

/**
 * st_home_restore_mode - 切回 PP 模式并给出回执
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_home_restore_mode(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		return dev_start_write(d, CIA402_OD_MODE, CIA402_MODE_PP, 1u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
			return SM_TRAN(&st_idle);
		}
		if (d->operation.home.failed != 0u) {
			dev_finish(d, (enum cia402_fail)d->operation.home.fail_kind);
		} else if (d->operation.home.cancelled == 0u) {
			dev_done(d);
		}
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&st_home_active);
	}
}

static void home_fail(struct cia402_device *d, enum cia402_fail fail)
{
	d->operation.home.failed = 1u;
	d->operation.home.fail_kind = (uint8_t)fail;
}
