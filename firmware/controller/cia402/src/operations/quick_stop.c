/*
 * quick_stop.c — 快速停与解除急停
 *
 * 急停只写一次 6040 = 02：减速曲线与最终停在哪个状态由驱动器自己的 605A
 * （Quick stop 代码）决定，本层不读状态字、不等减速过程、不再断使能。
 * 写失败只留 abort 码并报失败，不重试。
 *
 * 急停终态只有一条出口：收到解除急停请求后写 6040 = 0F（转移 16），
 * 让驱动器回到可操作状态，然后恢复受理普通请求。
 *
 * Author: zager
 */

#include "../states/states.h"

/**
 * st_qstop_write - 写快速停控制字，整条急停只有这一帧
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 写成功报 DONE、失败报 FAILED；两者都进 st_qstop_hold，不再发帧。
 */
sm_ret_t st_qstop_write(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_QUICK_STOP, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e)); /* 只记 abort 码，不重发 */
		} else {
			dev_finish(d, CIA402_OUTCOME_DONE, CIA402_FAIL_NONE);
		}
		return SM_TRAN(&st_qstop_hold);

	default:
		return SM_SUPER(&st_operation_active);
	}
}

/**
 * st_qstop_hold - 急停终态：不再发帧，只等一条解除急停请求
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 停在哪里由驱动器的 605A 决定（出厂 6 = quick stop 状态、仍保持使能），
 * 因此本层不推断、不跟踪，只保证开关量只有那一笔写入。
 *
 * Return: 解除请求迁 st_qstop_clear；其余请求与节拍被消费。
 */
sm_ret_t st_qstop_hold(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case CIA402_SIG_REQUEST: {
		const struct request_evt *ev = SM_EVT_CAST(struct request_evt, e);

		if (ev->req.kind != CIA402_REQ_ESTOP_CLEAR) {
			return SM_HANDLED(); /* 急停中：其他请求一律丢弃 */
		}
		/* 先占住流程：解除那一笔在途时，别的请求必须拿到 BUSY */
		d->operation.kind = ev->req.kind;
		d->operation.running = 1u;
		d->operation.cancel = CIA402_CANCEL_NONE;
		return SM_TRAN(&st_qstop_clear);
	}

	case CIA402_SIG_TICK:
	case CIA402_SIG_CANCEL:
	case CIA402_SIG_SDO_DONE:
		return SM_HANDLED();

	default:
		DEV_IGNORE(); /* 进入/退出事件直接忽略 */
	}
}

/**
 * st_qstop_clear - 解除急停：写 6040 = 0x0F 让驱动器回可操作
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * 0x0F 是 CiA 402 从 quick stop 状态回到 Operation enabled 的转移 16。
 * 驱动器若本来就不在 quick stop 状态，这一帧不起作用也不报错，
 * 后续使能链会按实际状态字选路。
 *
 * Return: 写成功报 DONE、失败报 FAILED；两者都回 st_idle 恢复受理。
 */
sm_ret_t st_qstop_clear(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_write(d, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP, 2u,
			  CIA402_DEVICE_SDO_TIMEOUT_MS);
		return SM_HANDLED();

	case CIA402_SIG_SDO_DONE:
		if (DEV_SDO_RESULT(e)->status != 0) {
			dev_fail(d, DEV_SDO_RESULT(e));
		} else {
			dev_finish(d, CIA402_OUTCOME_DONE, CIA402_FAIL_NONE);
		}
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&st_operation_active);
	}
}
