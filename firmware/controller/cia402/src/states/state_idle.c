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
		uint8_t reason = d->cancel_reason;
		enum cia402_device_req running;

		pthread_mutex_lock(&d->lock);
		running = d->running;
		pthread_mutex_unlock(&d->lock);
		d->cancel_reason = CIA402_CANCEL_NONE;
		d->controlword_pending = 0u;

		if (running == CIA402_DEVICE_REQ_NONE) {
			break; /* 流程已自行收尾，或本来就在空闲 */
		}
		if (reason == CIA402_CANCEL_TIMEOUT) {
			dev_finish(d, -1, d->pending_result.abort_code);
		} else {
			/* 被抢占或手动取消：这条流程的结论作废，不留下结果 */
			pthread_mutex_lock(&d->lock);
			d->running = CIA402_DEVICE_REQ_NONE;
			pthread_mutex_unlock(&d->lock);
		}
		break;
	}

	case SIG_DEV_START: {
		const struct dev_start_evt *ev = SM_EVT_CAST(struct dev_start_evt, e);

		pthread_mutex_lock(&d->lock);
		d->running = ev->req;
		pthread_mutex_unlock(&d->lock);
		d->pp_seen_low = 0u;

		switch (ev->req) {
		case CIA402_DEVICE_REQ_CONFIG:
			return SM_TRAN(&st_config_write_mode);
		case CIA402_DEVICE_REQ_ENABLE:
			return SM_TRAN(&st_enable_read_pds);
		case CIA402_DEVICE_REQ_DISABLE:
			return SM_TRAN(&st_disable_write);
		case CIA402_DEVICE_REQ_PP:
			return SM_TRAN(&st_pp_write_velocity);
		case CIA402_DEVICE_REQ_HALT:
			return SM_TRAN(&st_halt_write);
		case CIA402_DEVICE_REQ_QUICK_STOP:
			return SM_TRAN(&st_qstop_write);
		default:
			dev_finish(d, -1, 0u);
			return SM_HANDLED();
		}
	}

	case SIG_DEV_TICK:
		/* 一轮读完（slot 回 0）才按间隔刷新 */
		if (d->mon_slot == 0u &&
		    (uint32_t)(d->now_ms - d->mon_last_ms) < CIA402_DEVICE_MON_INTERVAL_MS) {
			return SM_HANDLED();
		}
		d->mon_last_ms = d->now_ms;
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
		dev_read(d, kMonitorIndex[d->mon_slot], CIA402_DEVICE_MON_TIMEOUT_MS);
		break;

	case SIG_DEV_DONE: {
		const struct sdo_result *r = DEV_DONE_EVT(e);

		if (r->status == 0) {
			pthread_mutex_lock(&d->lock);
			switch (d->mon_slot) {
			case 0:
				d->statusword = (uint16_t)(r->value & 0xFFFFu);
				break;
			case 1:
				d->position = (int32_t)r->value;
				break;
			case 2:
				d->velocity = (int32_t)r->value;
				break;
			default:
				d->error_code = (uint16_t)(r->value & 0xFFFFu);
				break;
			}
			pthread_mutex_unlock(&d->lock);
		}
		/* 读失败也往下轮：保留旧值，不因此报故障 */
		d->mon_slot = (uint8_t)((d->mon_slot + 1u) % 4u);
		return SM_TRAN(&st_idle);
	}

	case SIG_DEV_CANCEL:
		d->mon_slot = (uint8_t)((d->mon_slot + 1u) % 4u);
		return SM_TRAN(&st_idle);

	default:
		return SM_SUPER(&sm_top);
	}

	return SM_HANDLED();
}
