/*
 * drive.c — 意图受理、服务执行、反馈刷新
 *
 * 三条规则：
 *   1. 意图只往槽里放（不阻塞调用者），由服务线程取；
 *   2. 一次只执行一件事：槽里已有请求或正在执行 → 后来者得到 1（忙）；
 *      安全动作（halt / quick stop）不同：先 cancel 在途流程，再占槽；
 *   3. 反馈轮转只在"没有流程在跑"时进行，避免和业务事务抢同一条通道。
 */

#include "drive_internal.h"

#include <ds402/od.h>
#include <ds402/pds.h>

#include <stddef.h>
#include <string.h>

static void monitor_step(struct drive *d, uint32_t now_ms);

/* ---- 装配 ---- */

void drive_init(struct drive *d, const struct od_access *od, uint8_t node_id)
{
	memset(d, 0, sizeof(*d));
	pthread_mutex_init(&d->lock, NULL);
	d->od = od;
	d->node_id = node_id;
	d->cfg_state = DRIVE_CFG_PENDING;
	d->en_state = DRIVE_EN_NOT_ENABLED;
	d->result = DRIVE_RESULT_NONE;
	d->controlword = DS402_CW_DISABLE;
}

/* ---- 意图 ---- */

static int submit(struct drive *d, enum drive_req req)
{
	int rc;

	if (d == NULL || d->od == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != DRIVE_REQ_NONE || d->req != DRIVE_REQ_NONE) {
		rc = 1; /* 有一件在处理：调用方自己决定何时重试 */
	} else {
		d->req = req;
		rc = 0;
	}
	pthread_mutex_unlock(&d->lock);
	return rc;
}

/*
 * 抢占式提交：仅供"停"这类安全动作使用。
 * 先把在途流程 cancel 掉（在等的对象事务立刻返回），再把请求覆盖进槽位；
 * 服务线程退出当前流程后会立刻取新的这一件。
 */
static int submit_urgent(struct drive *d, enum drive_req req)
{
	if (d == NULL || d->od == NULL) {
		return -1;
	}
	if (d->od->abort != NULL) {
		d->od->abort(d->od->ctx); /* 先让阻塞的等待醒来 */
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != DRIVE_REQ_NONE) {
		d->cancel = 1u; /* 当前流程尽快退出（结论作废） */
	}
	d->req = req;
	pthread_mutex_unlock(&d->lock);
	return 0;
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
	d->tgt = target;
	d->vel = vel;
	d->acc = acc;
	d->dec = dec;
	d->failed = 0u; /* 新过程开始：上次的失败事实作废 */
	pthread_mutex_unlock(&d->lock);

	rc = submit(d, DRIVE_REQ_PP);
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

void drive_cancel(struct drive *d)
{
	if (d == NULL) {
		return;
	}
	if (d->od != NULL && d->od->abort != NULL) {
		d->od->abort(d->od->ctx);
	}
	pthread_mutex_lock(&d->lock);
	d->cancel = 1u;
	pthread_mutex_unlock(&d->lock);
}

/* ---- 服务执行 ---- */

/* 收尾：把结论写进事实；被取消的流程不留结论（调用方已转走） */
static void finish_request(struct drive *d, int ok)
{
	int cancelled;

	pthread_mutex_lock(&d->lock);
	cancelled = d->cancel != 0u;
	d->cancel = 0u;
	if (cancelled != 0) {
		d->result = DRIVE_RESULT_NONE;
		d->running = DRIVE_REQ_NONE;
		pthread_mutex_unlock(&d->lock);
		return;
	}
	if (ok != 0) {
		d->abort_code = (d->od != NULL && d->od->last_error != NULL)
					? d->od->last_error(d->od->ctx)
					: 0u;
		d->failed = 1u;
	}
	switch (d->running) {
	case DRIVE_REQ_CONFIG:
		d->cfg_state = (ok == 0) ? DRIVE_CFG_CONFIGURED : DRIVE_CFG_FAILED;
		break;
	case DRIVE_REQ_ENABLE:
		d->en_state = (ok == 0) ? DRIVE_EN_ENABLED : DRIVE_EN_FAILED;
		break;
	case DRIVE_REQ_DISABLE:
		d->en_state = DRIVE_EN_NOT_ENABLED;
		break;
	case DRIVE_REQ_QUICK_STOP:
		d->en_state = (ok == 0) ? DRIVE_EN_NOT_ENABLED : DRIVE_EN_FAILED;
		break;
	default:
		break;
	}
	d->result = (ok == 0) ? DRIVE_RESULT_DONE : DRIVE_RESULT_FAILED;
	d->running = DRIVE_REQ_NONE;
	pthread_mutex_unlock(&d->lock);
}

void drive_service(struct drive *d, uint32_t now_ms)
{
	enum drive_req req;
	int32_t t, v, a, dc;
	int running;

	if (d == NULL || d->od == NULL) {
		return;
	}

	pthread_mutex_lock(&d->lock);
	req = d->req;
	if (req != DRIVE_REQ_NONE) {
		d->req = DRIVE_REQ_NONE;
		d->running = req;
		d->cancel = 0u;
	}
	t = d->tgt;
	v = d->vel;
	a = d->acc;
	dc = d->dec;
	running = d->running != DRIVE_REQ_NONE;
	pthread_mutex_unlock(&d->lock);

	if (req != DRIVE_REQ_NONE) {
		int ok = -1;

		/* 下面这些调用会阻塞（等状态字/等应答），但都是睡眠不是空转 */
		switch (req) {
		case DRIVE_REQ_CONFIG:
			ok = drive_do_config(d);
			break;
		case DRIVE_REQ_ENABLE:
			ok = drive_do_enable(d);
			break;
		case DRIVE_REQ_DISABLE:
			ok = drive_do_disable(d);
			break;
		case DRIVE_REQ_PP:
			ok = drive_pp_move(d, t, v, a, dc);
			break;
		case DRIVE_REQ_HALT:
			ok = drive_do_halt(d);
			break;
		case DRIVE_REQ_QUICK_STOP:
			ok = drive_do_quick_stop(d);
			break;
		default:
			break;
		}
		finish_request(d, ok);
		running = 0;
	}

	/* 空闲才刷反馈：避免和业务事务抢同一对 COB-ID */
	if (running == 0) {
		monitor_step(d, now_ms);
	}
}

/* ---- 反馈轮转：4 个对象轮流读，50 ms 一轮 ---- */

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

	pthread_mutex_lock(&d->lock);
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
	d->mon_slot = (uint8_t)((slot + 1u) % 4u);
	pthread_mutex_unlock(&d->lock);
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
	return d->running != DRIVE_REQ_NONE || d->req != DRIVE_REQ_NONE;
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
