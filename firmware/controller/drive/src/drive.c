/*
 * drive.c — 请求受理、服务执行、反馈刷新
 *
 * 三条规则：
 *   1. 请求只往槽里放（不阻塞调用者），由服务线程取；
 *   2. 一次只执行一件事：槽里已有请求或正在执行 → 后来者得到 1（忙）；
 *   3. 反馈轮转只在"没有事务在跑"时进行，避免和业务事务抢同一条通道。
 */

#include "drive_internal.h"

#include <ds402/od.h>

#include <stddef.h>
#include <string.h>

/* ---- 装配 ---- */

void drive_init(struct drive *d, struct co_sdo *sdo, const struct od_access *od, uint8_t node_id)
{
	memset(d, 0, sizeof(*d));
	pthread_mutex_init(&d->lock, NULL);
	d->sdo = sdo;
	d->od = od;
	d->node_id = node_id;
	d->cfg_state = DRIVE_CFG_PENDING;
	d->en_state = DRIVE_EN_NOT_ENABLED;
	d->result = DRIVE_RESULT_NONE;
	d->controlword = DS402_CW_DISABLE;
	d->mode = mode_pp_ops(); /* 上电默认 PP；配好 6060 后由 drive_mode 换表 */
}

/* ---- 请求 ---- */

static int submit(struct drive *d, enum drive_req req)
{
	int rc;

	if (d == NULL || d->od == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->busy != 0u || d->req != DRIVE_REQ_NONE) {
		rc = 1; /* 有一件在处理：调用方自己决定何时重试 */
	} else {
		d->req = req;
		rc = 0;
	}
	pthread_mutex_unlock(&d->lock);
	return rc;
}

int drive_request_config(struct drive *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->cfg_state != DRIVE_CFG_PENDING) {
			pthread_mutex_unlock(&d->lock);
			return 1; /* 已经配过 / 已失败过，不重复发起 */
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, DRIVE_REQ_CONFIG);
}

int drive_request_enable(struct drive *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->en_state != DRIVE_EN_NOT_ENABLED) {
			pthread_mutex_unlock(&d->lock);
			return 1;
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, DRIVE_REQ_ENABLE);
}

int drive_request_disable(struct drive *d)
{
	return submit(d, DRIVE_REQ_DISABLE);
}

/*
 * 抢占式提交：仅供"停"这类安全动作使用。
 * 作废在途 PP（少一次到位判定），把请求覆盖成 STOP 类动作。
 * 正在执行的那一步不会被中断（阻塞调用最多几百毫秒），但下一拍就执行新请求。
 */
static int submit_urgent(struct drive *d, enum drive_req req)
{
	if (d == NULL || d->od == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->active == DRIVE_REQ_PP) {
		/* PP 被抢占：结论作废（调用方已经转到 HALTING/ESTOP，不会再取它） */
		d->pp_wait = 0u;
		d->active = DRIVE_REQ_NONE;
		d->busy = 0u;
		d->result = DRIVE_RESULT_NONE;
	}
	d->req = req;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

int drive_pp_start(struct drive *d, int32_t target, int32_t vel, int32_t acc, int32_t dec)
{
	int rc;

	if (d == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->en_state != DRIVE_EN_ENABLED) {
		pthread_mutex_unlock(&d->lock);
		return -1; /* 没使能就不接受运动指令 */
	}
	d->pp_target = target;
	d->pp_vel = vel;
	d->pp_acc = acc;
	d->pp_dec = dec;
	pthread_mutex_unlock(&d->lock);

	rc = submit(d, DRIVE_REQ_PP);
	if (rc == 0) {
		pthread_mutex_lock(&d->lock);
		d->failed = 0u; /* 新过程开始：上次的失败事实作废 */
		pthread_mutex_unlock(&d->lock);
	}
	return rc;
}

int drive_halt(struct drive *d)
{
	return submit_urgent(d, DRIVE_REQ_HALT);
}

int drive_quick_stop(struct drive *d)
{
	return submit_urgent(d, DRIVE_REQ_QUICK_STOP);
}

/* ---- 服务执行 ---- */

/* 收尾：把结论写进事实（cfg/en/result/failed/abort_code） */
static void finish_request(struct drive *d, int ok)
{
	pthread_mutex_lock(&d->lock);
	if (ok != 0) {
		d->abort_code = (d->od->last_error != NULL) ? d->od->last_error(d->od->ctx) : 0u;
		d->failed = 1u;
	}
	switch (d->active) {
	case DRIVE_REQ_CONFIG:
		d->cfg_state = (ok == 0) ? DRIVE_CFG_CONFIGURED : DRIVE_CFG_FAILED;
		break;
	case DRIVE_REQ_ENABLE:
		d->en_state = (ok == 0) ? DRIVE_EN_ENABLED : DRIVE_EN_FAILED;
		break;
	case DRIVE_REQ_DISABLE:
	case DRIVE_REQ_QUICK_STOP:
		d->en_state = DRIVE_EN_NOT_ENABLED;
		break;
	default:
		break;
	}
	d->result = (ok == 0) ? DRIVE_RESULT_DONE : DRIVE_RESULT_FAILED;
	d->active = DRIVE_REQ_NONE;
	d->busy = 0u;
	pthread_mutex_unlock(&d->lock);
}

/* 配置：写模式并读回校验（判据是"读回值对不对"，不是"写成功"） */
static int do_config(struct drive *d)
{
	uint32_t v = 0u;
	uint8_t len = 0u;

	if (drive_od_write(d, DS402_OD_MODE, 0u, DS402_MODE_PP, 1u) != 0) {
		return -1;
	}
	if (drive_od_read(d, DS402_OD_MODE_ACTUAL, 0u, &v, &len) != 0) {
		return -1;
	}
	return ((v & 0xFFu) == DS402_MODE_PP) ? 0 : -1;
}

static int do_halt(struct drive *d)
{
	const uint32_t deadline = d->od->now_ms(d->od->ctx) + DRIVE_HALT_TIMEOUT_MS;

	if (drive_write_controlword(d, (uint16_t)(d->controlword | DS402_CW_HALT)) != 0) {
		return -1;
	}
	for (;;) {
		uint32_t v = 0u;
		uint8_t len = 0u;

		if (d->od->read(d->od->ctx, DS402_OD_STATUSWORD, 0u, &v, &len,
				DRIVE_MON_TIMEOUT_MS) != 0) {
			return -1;
		}
		d->statusword = (uint16_t)(v & 0xFFFFu);
		d->fb_valid |= DRIVE_FB_STATUSWORD;
		if (ds402_status_parse(d->statusword) == DS402_FAULT) {
			return -1;
		}
		if ((d->statusword & DS402_SW_TARGET_REACHED) != 0u) {
			break;
		}
		if ((int32_t)(d->od->now_ms(d->od->ctx) - deadline) >= 0) {
			return -1;
		}
	}
	/* 停稳后清 bit8：保持使能（保持力矩），可以立刻接下一句运动 */
	return drive_write_controlword(d, (uint16_t)(d->controlword & (uint16_t)~DS402_CW_HALT));
}

static int do_quick_stop(struct drive *d)
{
	const uint32_t deadline = d->od->now_ms(d->od->ctx) + DRIVE_QUICK_STOP_TIMEOUT_MS;

	(void)drive_write_controlword(d, DS402_CW_QUICK_STOP); /* 写不进去也继续往下 */

	for (;;) {
		uint32_t v = 0u;
		uint8_t len = 0u;
		enum ds402_state st;

		if (d->od->read(d->od->ctx, DS402_OD_STATUSWORD, 0u, &v, &len,
				DRIVE_MON_TIMEOUT_MS) != 0) {
			break; /* 读不到也要断使能 */
		}
		d->statusword = (uint16_t)(v & 0xFFFFu);
		d->fb_valid |= DRIVE_FB_STATUSWORD;
		st = ds402_status_parse(d->statusword);
		if (st == DS402_SWITCH_ON_DISABLED || st == DS402_NOT_READY) {
			break; /* 已无输出力矩 */
		}
		if ((int32_t)(d->od->now_ms(d->od->ctx) - deadline) >= 0) {
			break;
		}
	}
	/* 断使能：出口是"没有力矩" */
	return drive_write_controlword(d, DS402_CW_DISABLE);
}

/* 反馈轮转：4 个对象轮流读，50 ms 一轮 */
static void monitor_step(struct drive *d, uint32_t now_ms)
{
	static const uint16_t kIndex[4] = {DS402_OD_STATUSWORD, DS402_OD_POSITION_ACTUAL,
					   DS402_OD_VELOCITY_ACTUAL, DS402_OD_ERROR_CODE};
	uint32_t v = 0u;
	uint8_t len = 0u;
	uint8_t slot = d->mon_slot;

	if (d->fb_valid == (uint8_t)0x0Fu &&
	    (uint32_t)(now_ms - d->mon_last_ms) < DRIVE_MON_INTERVAL_MS) {
		return; /* 都读到过了：按间隔刷新 */
	}
	d->mon_last_ms = now_ms;
	if (d->od->read(d->od->ctx, kIndex[slot], 0u, &v, &len, DRIVE_MON_TIMEOUT_MS) != 0) {
		d->mon_slot = (uint8_t)((slot + 1u) % 4u); /* 读失败保留旧值 */
		return;
	}

	switch (slot) {
	case 0:
		d->statusword = (uint16_t)(v & 0xFFFFu);
		d->fb_valid |= DRIVE_FB_STATUSWORD;
		break;
	case 1:
		d->position = (int32_t)v;
		d->fb_valid |= DRIVE_FB_POSITION;
		break;
	case 2:
		d->velocity = (int32_t)v;
		d->fb_valid |= DRIVE_FB_VELOCITY;
		break;
	default:
		d->error_code = (uint16_t)(v & 0xFFFFu);
		d->fb_valid |= DRIVE_FB_ERROR_CODE;
		break;
	}
	d->mon_done_slot = slot;
	d->mon_slot = (uint8_t)((slot + 1u) % 4u);
}

void drive_service(struct drive *d, uint32_t now_ms)
{
	enum drive_req req;
	int32_t t, v, a, dc;
	int busy;

	if (d == NULL || d->od == NULL) {
		return;
	}

	pthread_mutex_lock(&d->lock);
	req = d->req;
	if (req != DRIVE_REQ_NONE) {
		d->req = DRIVE_REQ_NONE;
		d->active = req;
		d->busy = 1u;
	}
	t = d->pp_target;
	v = d->pp_vel;
	a = d->pp_acc;
	dc = d->pp_dec;
	pthread_mutex_unlock(&d->lock);

	if (req != DRIVE_REQ_NONE) {
		int ok = 1;

		switch (req) {
		case DRIVE_REQ_CONFIG:
			ok = do_config(d);
			break;
		case DRIVE_REQ_ENABLE:
			ok = drive_sm_run_to(d, DS402_OP_ENABLED, DRIVE_SM_TIMEOUT_MS);
			break;
		case DRIVE_REQ_DISABLE:
			ok = drive_sm_run_to(d, DS402_SWITCH_ON_DISABLED, DRIVE_SM_TIMEOUT_MS);
			break;
		case DRIVE_REQ_PP:
			ok = (d->mode != NULL) ? d->mode->begin(d, t, v, a, dc, now_ms) : -1;
			break;
		case DRIVE_REQ_HALT:
			ok = do_halt(d);
			break;
		case DRIVE_REQ_QUICK_STOP:
			ok = do_quick_stop(d);
			break;
		default:
			break;
		}
		/* PP 的结论要等到位之后才出，其余到此结束 */
		if (req != DRIVE_REQ_PP || ok < 0) {
			finish_request(d, ok);
		}
	}

	/* PP 到位检查（非阻塞，每拍读一次状态字） */
	if (d->mode != NULL && d->mode->busy != NULL && d->mode->busy(d) != 0) {
		int r = d->mode->poll(d, now_ms);

		if (r != 0) {
			finish_request(d, (r > 0) ? 0 : -1);
		}
	}

	/* 空闲才刷反馈：避免和业务事务抢同一对 COB-ID */
	pthread_mutex_lock(&d->lock);
	busy = d->busy;
	pthread_mutex_unlock(&d->lock);
	if (busy == 0u) {
		monitor_step(d, now_ms);
	}
}

/* ---- 事实 ---- */

int drive_is_configured(const struct drive *d)
{
	return d != NULL && d->cfg_state == DRIVE_CFG_CONFIGURED;
}

int drive_is_enabled(const struct drive *d)
{
	return d != NULL && d->en_state == DRIVE_EN_ENABLED;
}

int drive_failed(const struct drive *d)
{
	return d != NULL && d->failed != 0u;
}

int drive_busy(const struct drive *d)
{
	if (d == NULL) {
		return 1;
	}
	return d->busy != 0u || d->req != DRIVE_REQ_NONE ||
	       (d->mode != NULL && d->mode->busy != NULL && d->mode->busy(d) != 0);
}

enum drive_result drive_take_result(struct drive *d)
{
	enum drive_result r;

	if (d == NULL) {
		return DRIVE_RESULT_NONE;
	}
	pthread_mutex_lock(&d->lock);
	r = d->result;
	d->result = DRIVE_RESULT_NONE;
	pthread_mutex_unlock(&d->lock);
	return r;
}

uint32_t drive_abort_code(const struct drive *d)
{
	return (d == NULL) ? 0u : d->abort_code;
}

uint16_t drive_feedback_statusword(const struct drive *d)
{
	return (d == NULL) ? 0u : d->statusword;
}

int32_t drive_feedback_position(const struct drive *d)
{
	return (d == NULL) ? 0 : d->position;
}

int32_t drive_feedback_velocity(const struct drive *d)
{
	return (d == NULL) ? 0 : d->velocity;
}

uint16_t drive_feedback_error_code(const struct drive *d)
{
	return (d == NULL) ? 0u : d->error_code;
}

uint8_t drive_feedback_valid(const struct drive *d)
{
	return (d == NULL) ? 0u : d->fb_valid;
}
