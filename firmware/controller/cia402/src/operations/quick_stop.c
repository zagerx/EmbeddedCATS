/*
 * quick_stop.c — 快速停与解除急停
 *
 * 急停只写一次 6040 = 02：减速曲线与停在哪个状态全由驱动器的 605A 决定。
 * 本台 605A = 2 = “通过 6085 减速停止后 switch on disable，断使能”，
 * 抱闸随后自动抱住。主站不读状态字、不等减速、不补断使能。
 * 写失败只留 abort 码并报失败，不重试。
 *
 * 解除急停不碰驱动器：断电与抱闸在急停那一帧就完成了，这里只把设备流程
 * 释放回空闲。再使能必须由操作员刻意发起——“复位不得导致重启”。
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
 * 驱动器此刻已按 605A = 2 停住、断使能并抱闸。解除请求只释放流程，
 * 不写任何控制字，也不替操作员重新使能。
 *
 * Return: 解除请求报 DONE 并迁 st_idle；其余请求与节拍被消费。
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
		/* 断电已在急停那一帧完成，这里只交回执并退出终态 */
		dev_finish(d, CIA402_OUTCOME_DONE, CIA402_FAIL_NONE);
		return SM_TRAN(&st_idle);
	}

	case CIA402_SIG_TICK:
	case CIA402_SIG_CANCEL:
	case CIA402_SIG_SDO_DONE:
		return SM_HANDLED();

	default:
		DEV_IGNORE(); /* 进入/退出事件直接忽略 */
	}
}
