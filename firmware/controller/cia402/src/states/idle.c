/*
 * idle.c — 无流程状态与流程收尾
 *
 * st_idle 是流程的收口：从任何流程迁回来时，由它的 ENTRY 按取消原因决定
 * 这条流程要报失败还是作废。外部反馈读取不属于本模块。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_idle - 无流程状态
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 新请求迁对应流程状态；其余事件按状态层次处理。
 */
sm_ret_t st_idle(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t reason = d->operation.cancel;
		int busy;

		/* phase 还停在流程上 = 流程是被打断退出的（自己收尾的已由
		 * dev_finish 归位），此时按 reason 决定要不要报失败。 */
		busy = (d->outbox.phase != CIA402_PHASE_IDLE);
		d->outbox.phase = CIA402_PHASE_IDLE;
		d->operation.cancel = CIA402_CANCEL_NONE;
		d->cw.pending_valid = 0u;

		if (busy == 0) {
			break; /* 本来就在空闲 */
		}
		if (reason == CIA402_CANCEL_TIMEOUT) {
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_TIMEOUT);
		}
		/* 被抢占：这条流程的结论作废，不留下结果 */
		break;
	}

	case CIA402_SIG_REQUEST: {
		const struct request_evt *ev = SM_EVT_CAST(struct request_evt, e);

		d->outbox.phase = dev_phase_of(ev->req.kind);

		/* 流程参数整块拷进 operation：状态函数只读 operation，不读 mailbox */
		d->operation.kind = ev->req.kind;
		d->operation.cancel = CIA402_CANCEL_NONE;
		d->operation.deadline = 0u;
		d->operation.pp.target = ev->req.pp.target;
		d->operation.pp.velocity = ev->req.pp.velocity;
		d->operation.pp.accel = ev->req.pp.accel;
		d->operation.pp.decel = ev->req.pp.decel;
		d->operation.pp.issued = 0;
		d->operation.pp.seen_low = 0u;

		switch (ev->req.kind) {
		case CIA402_REQ_CONFIG:
			return SM_TRAN(&st_config_write_mode);
		case CIA402_REQ_ENABLE:
			return SM_TRAN(&st_enable_read_pds);
		case CIA402_REQ_DISABLE:
			return SM_TRAN(&st_disable_write);
		case CIA402_REQ_MOVE:
			return SM_TRAN(&st_pp_write_velocity);
		case CIA402_REQ_HALT:
			return SM_TRAN(&st_halt_write);
		case CIA402_REQ_QUICK_STOP:
			return SM_TRAN(&st_qstop_write);
		default:
			dev_finish(d, CIA402_OP_FAILED, CIA402_OP_FAIL_NONE);
			return SM_HANDLED();
		}
	}

	case CIA402_SIG_TICK:
	case CIA402_SIG_CANCEL:
		return SM_HANDLED(); /* 空闲时不主动读反馈，也无可取消流程 */

	default:
		return SM_SUPER(&sm_top);
	}

	return SM_HANDLED();
}
