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

/* 反馈轮转顺序：状态字 → 位置 → 速度 → 错误码 */
static const uint16_t kMonitorIndex[4] = {CIA402_OD_STATUSWORD, CIA402_OD_POSITION_ACTUAL,
					  CIA402_OD_VELOCITY_ACTUAL, CIA402_OD_ERROR_CODE};

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
		uint8_t reason = d->job.cancel;
		int busy;

		pthread_mutex_lock(&d->lock);
		busy = d->mailbox.executing;
		pthread_mutex_unlock(&d->lock);
		d->job.cancel = CIA402_CANCEL_NONE;
		d->outbox.status.phase = CIA402_PHASE_IDLE;
		d->sdo.cw_next_valid = 0u;

		if (busy == 0) {
			break; /* 流程已自行收尾，或本来就在空闲 */
		}
		if (reason == CIA402_CANCEL_TIMEOUT) {
			dev_finish(d, CIA402_RESULT_FAILED, CIA402_FAIL_TIMEOUT);
		} else {
			/* 被抢占或手动取消：这条流程的结论作废，不留下结果 */
			pthread_mutex_lock(&d->lock);
			d->mailbox.executing = 0u;
			pthread_mutex_unlock(&d->lock);
		}
		break;
	}

	case SIG_DEV_START: {
		const struct dev_start_evt *ev = SM_EVT_CAST(struct dev_start_evt, e);

		pthread_mutex_lock(&d->lock);
		d->running = ev->req;
		d->mailbox.executing = 1u;
		d->outbox.status.phase = dev_phase_of(ev->req.kind);
		pthread_mutex_unlock(&d->lock);

		/* 当前流程的参数整块换新：以后状态函数只看 job，不再看请求槽 */
		d->job.kind = ev->req.kind;
		d->job.cancel = CIA402_CANCEL_NONE;
		d->job.deadline = 0u;
		d->job.pp.target = ev->req.pp.target;
		d->job.pp.velocity = ev->req.pp.velocity;
		d->job.pp.accel = ev->req.pp.accel;
		d->job.pp.decel = ev->req.pp.decel;
		d->job.pp.issued = 0;
		d->job.pp.seen_low = 0u;

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
			dev_finish(d, CIA402_RESULT_FAILED, CIA402_FAIL_NONE);
			return SM_HANDLED();
		}
	}

	case SIG_DEV_TICK:
		/* 一轮读完（slot 回 0）才按间隔刷新 */
		if (d->monitor.slot == 0u &&
		    (uint32_t)(d->now_ms - d->monitor.last_ms) < CIA402_DEVICE_MON_INTERVAL_MS) {
			return SM_HANDLED();
		}
		d->monitor.last_ms = d->now_ms;
		return SM_TRAN(&st_monitor);

	case SIG_DEV_CANCEL:
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
	struct cia402_device *d = DEV_OF(me);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		dev_read(d, kMonitorIndex[d->monitor.slot], CIA402_DEVICE_MON_TIMEOUT_MS);
		break;

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);

		if (r->status == 0) {
			pthread_mutex_lock(&d->lock);
			switch (d->monitor.slot) {
			case 0:
				d->outbox.status.statusword = (uint16_t)(r->value & 0xFFFFu);
				break;
			case 1:
				d->outbox.status.position = (int32_t)r->value;
				break;
			case 2:
				d->outbox.status.velocity = (int32_t)r->value;
				break;
			default:
				d->outbox.status.error_code = (uint16_t)(r->value & 0xFFFFu);
				break;
			}
			pthread_mutex_unlock(&d->lock);
		}
		/* 读失败也往下轮：保留旧值，不因此报故障 */
		d->monitor.slot = (uint8_t)((d->monitor.slot + 1u) % 4u);
		return SM_TRAN(&st_idle);
	}

	case SIG_DEV_CANCEL:
		d->monitor.slot = (uint8_t)((d->monitor.slot + 1u) % 4u);
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&sm_top);
	}

	return SM_HANDLED();
}
