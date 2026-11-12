/*
 * state_idle.c — 空闲与反馈轮转
 *
 * st_idle 是流程的收口：从任何流程迁回来时，由它的 ENTRY 按取消原因决定
 * 这条流程要报失败还是作废。st_monitor 只读一项反馈就回 IDLE，
 * 四个对象轮完再等一个间隔，避免与业务流程争用同一条对象通道。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_idle - 无流程状态
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 新请求迁流程状态；到刷新间隔迁 st_monitor。
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
		pthread_mutex_lock(&d->lock);
		busy = (d->outbox.phase != CIA402_PHASE_IDLE);
		d->outbox.phase = CIA402_PHASE_IDLE;
		pthread_mutex_unlock(&d->lock);
		d->operation.cancel = CIA402_CANCEL_NONE;
		d->sdo.cw_next_valid = 0u;

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

		pthread_mutex_lock(&d->lock);
		d->outbox.phase = dev_phase_of(ev->req.kind);
		pthread_mutex_unlock(&d->lock);

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
		/* 一轮读完（cursor 回 0）才按间隔刷新 */
		if (d->monitor.cursor == 0u &&
		    (uint32_t)(d->now_ms - d->monitor.last_ms) < CIA402_DEVICE_MON_INTERVAL_MS) {
			return SM_HANDLED();
		}
		d->monitor.last_ms = d->now_ms;
		return SM_TRAN(&st_monitor);

	case CIA402_SIG_CANCEL:
		return SM_HANDLED(); /* 空闲时无可取消 */

	default:
		return SM_SUPER(&sm_top);
	}

	return SM_HANDLED();
}

/**
 * st_monitor - 读一项反馈
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 读完（含失败）迁回 st_idle。
 */
sm_ret_t st_monitor(struct sm_hsm *const me, const struct sm_event *const e)
{
	static const uint16_t kMonitorIndex[4] = {CIA402_OD_STATUSWORD, CIA402_OD_POSITION_ACTUAL,
						  CIA402_OD_VELOCITY_ACTUAL, CIA402_OD_ERROR_CODE};
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, kMonitorIndex[d->monitor.cursor], CIA402_DEVICE_MON_TIMEOUT_MS);
		break;

	case CIA402_SIG_SDO_DONE: {
		const struct sdo_result *r = DEV_SDO_RESULT(e);

		if (r->status == 0) {
			pthread_mutex_lock(&d->lock);
			switch (d->monitor.cursor) {
			case 0:
				d->feedback.statusword = (uint16_t)(r->value & 0xFFFFu);
				break;
			case 1:
				d->feedback.position = (int32_t)r->value;
				break;
			case 2:
				d->feedback.velocity = (int32_t)r->value;
				break;
			default:
				d->feedback.error_code = (uint16_t)(r->value & 0xFFFFu);
				break;
			}
			pthread_mutex_unlock(&d->lock);
		}
		/* 读失败也往下轮：保留旧值，不因此报故障 */
		d->monitor.cursor = (uint8_t)((d->monitor.cursor + 1u) % 4u);
		return SM_TRAN(&st_idle);
	}

	case CIA402_SIG_CANCEL:
		d->monitor.cursor = (uint8_t)((d->monitor.cursor + 1u) % 4u);
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&sm_top);
	}

	return SM_HANDLED();
}
