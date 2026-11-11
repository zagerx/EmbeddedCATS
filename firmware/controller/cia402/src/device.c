/*
 * device.c — 设备状态机的装配、推进与原语
 *
 * 服务线程调 cia402_device_service()，本文件把「请求槽、事务结果、取消」
 * 翻成 hsm 事件分发出去；状态本身与迁移条件在 states/ 下。
 *
 * 一次 service 最多发起一笔对象事务：这条纪律让服务线程能在多个设备之间
 * 公平轮转，任何一个设备在等应答都不会占住循环。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <stddef.h>
#include <string.h>

static int submit(struct cia402_device *d, enum cia402_device_req req);
static int submit_urgent(struct cia402_device *d, enum cia402_device_req req);
static enum cia402_device_req pop_request(struct cia402_device *d);
static void dispatch_start(struct cia402_device *d, enum cia402_device_req req);
static void dispatch_done(struct cia402_device *d, const struct sdo_result *result);
static void dispatch_cancel(struct cia402_device *d, const struct sdo_result *result);
static void dispatch_tick(struct cia402_device *d);
static void pend_failure(struct cia402_device *d);

/**
 * cia402_device_init - 初始化一个设备实例
 * @d: 设备对象
 * @access: 异步对象访问接口
 * @node_id: CANopen 节点号
 */
void cia402_device_init(struct cia402_device *d, const struct sdo_access *access, uint8_t node_id)
{
	memset(d, 0, sizeof(*d));
	pthread_mutex_init(&d->lock, NULL);
	d->access = access;
	d->node_id = node_id;
	d->cfg_state = CIA402_DEVICE_CFG_PENDING;
	d->en_state = CIA402_DEVICE_EN_NOT_ENABLED;
	sm_hsm_init(&d->hsm, &device_initial);
}

/**
 * cia402_device_request_config - 投递配置请求
 * @d: 设备对象
 *
 * Return: 0 已受理；1 已配置、已失败或忙；-1 参数或装配错误。
 */
int cia402_device_request_config(struct cia402_device *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->cfg_state != CIA402_DEVICE_CFG_PENDING) {
			pthread_mutex_unlock(&d->lock);
			return 1;
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, CIA402_DEVICE_REQ_CONFIG);
}

/**
 * cia402_device_request_enable - 投递使能请求
 * @d: 设备对象
 *
 * Return: 0 已受理；1 已使能、已失败或忙；-1 参数或装配错误。
 */
int cia402_device_request_enable(struct cia402_device *d)
{
	if (d != NULL) {
		pthread_mutex_lock(&d->lock);
		if (d->en_state != CIA402_DEVICE_EN_NOT_ENABLED) {
			pthread_mutex_unlock(&d->lock);
			return 1;
		}
		pthread_mutex_unlock(&d->lock);
	}
	return submit(d, CIA402_DEVICE_REQ_ENABLE);
}

/**
 * cia402_device_request_disable - 投递断使能请求
 * @d: 设备对象
 *
 * Return: 0 已受理；1 设备忙；-1 参数或装配错误。
 */
int cia402_device_request_disable(struct cia402_device *d)
{
	return submit(d, CIA402_DEVICE_REQ_DISABLE);
}

/**
 * cia402_device_pp_start - 投递 PP 绝对位置请求
 * @d: 设备对象
 * @target: 目标位置，单位 pulse
 * @vel: 轮廓速度
 * @acc: 轮廓加速度
 * @dec: 轮廓减速度
 *
 * Return: 0 已受理；1 设备忙；-1 设备为空或未使能。
 */
int cia402_device_pp_start(struct cia402_device *d, int32_t target, int32_t vel, int32_t acc,
			   int32_t dec)
{
	if (d == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->en_state != CIA402_DEVICE_EN_ENABLED) {
		pthread_mutex_unlock(&d->lock);
		return -1;
	}
	if (d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE) {
		pthread_mutex_unlock(&d->lock);
		return 1;
	}
	d->tgt = target;
	d->vel = vel;
	d->acc = acc;
	d->dec = dec;
	d->failed = 0u;
	d->req = CIA402_DEVICE_REQ_PP;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

/**
 * cia402_device_halt - 抢占当前流程并请求减速停车
 * @d: 设备对象
 *
 * Return: 0 已受理；-1 参数或装配错误。
 */
int cia402_device_halt(struct cia402_device *d)
{
	return submit_urgent(d, CIA402_DEVICE_REQ_HALT);
}

/**
 * cia402_device_quick_stop - 抢占当前流程并请求快速停止
 * @d: 设备对象
 *
 * Return: 0 已受理；-1 参数或装配错误。
 */
int cia402_device_quick_stop(struct cia402_device *d)
{
	return submit_urgent(d, CIA402_DEVICE_REQ_QUICK_STOP);
}

/**
 * cia402_device_cancel - 取消当前流程且不启动后续请求
 * @d: 设备对象；NULL 安全
 */
void cia402_device_cancel(struct cia402_device *d)
{
	if (d == NULL) {
		return;
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != CIA402_DEVICE_REQ_NONE || d->transfer_pending != 0u) {
		d->cancel = CIA402_CANCEL_MANUAL;
	}
	pthread_mutex_unlock(&d->lock);
}

/**
 * cia402_device_service - 推进一次设备状态机
 * @d: 设备对象
 * @now_ms: 当前单调毫秒时刻
 *
 * 仅服务线程调用；每次最多消费一个传输结果或发起一笔新事务，不阻塞。
 */
void cia402_device_service(struct cia402_device *d, uint32_t now_ms)
{
	struct sdo_result result;
	enum cia402_device_req req;

	if (d == NULL || d->access == NULL) {
		return;
	}
	d->now_ms = now_ms;

	/* 上一拍发起的动作被拒：补一个失败事件，让状态机照常往前走 */
	if (d->pending_done != 0u) {
		result = d->pending_result;
		d->pending_done = 0u;
		dispatch_done(d, &result);
		return;
	}

	/* 抢占：先打断在途事务，让 take_result 尽快给出结论 */
	if (d->cancel != CIA402_CANCEL_NONE && d->transfer_pending != 0u) {
		d->access->abort(d->access->ctx);
	}

	if (d->transfer_pending != 0u) {
		if (d->access->take_result(d->access->ctx, &result) != 0) {
			/* 结果还没到：只判单笔事务的期限 */
			if (d->cancel == CIA402_CANCEL_NONE &&
			    (int32_t)(now_ms - d->deadline_ms) >= 0) {
				d->cancel = CIA402_CANCEL_TIMEOUT;
				d->access->abort(d->access->ctx);
			}
			dispatch_tick(d);
			return;
		}
		d->transfer_pending = 0u;
		if (d->cancel != CIA402_CANCEL_NONE) {
			dispatch_cancel(d, &result); /* 结论作废，abort 码留着 */
		} else {
			dispatch_done(d, &result);
		}
		return;
	}

	if (d->cancel != CIA402_CANCEL_NONE) {
		dispatch_cancel(d, NULL);
		return;
	}

	req = pop_request(d);
	if (req != CIA402_DEVICE_REQ_NONE) {
		dispatch_start(d, req);
		return;
	}

	dispatch_tick(d);
}

/**
 * cia402_device_is_configured - 查询配置事实
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 已配置；0 其他情况。
 */
int cia402_device_is_configured(const struct cia402_device *d)
{
	int ok;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	ok = d->cfg_state == CIA402_DEVICE_CFG_CONFIGURED;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return ok;
}

/**
 * cia402_device_is_enabled - 查询使能事实
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 已使能；0 其他情况。
 */
int cia402_device_is_enabled(const struct cia402_device *d)
{
	int ok;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	ok = d->en_state == CIA402_DEVICE_EN_ENABLED;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return ok;
}

/**
 * cia402_device_failed - 查询流程失败事实
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 有失败记录；0 其他情况。
 */
int cia402_device_failed(const struct cia402_device *d)
{
	int failed;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	failed = d->failed != 0u;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return failed;
}

/**
 * cia402_device_busy - 查询设备是否有请求或流程在途
 * @d: 设备对象；NULL 安全
 *
 * Return: 1 忙或设备为空；0 空闲。
 */
int cia402_device_busy(const struct cia402_device *d)
{
	int busy;

	if (d == NULL) {
		return 1;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	busy = d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return busy;
}

/**
 * cia402_device_take_result - 取走最近一次流程结论
 * @d: 设备对象；NULL 安全
 *
 * Return: 当前结论，取走后恢复 NONE。
 */
enum cia402_device_result cia402_device_take_result(struct cia402_device *d)
{
	enum cia402_device_result result;

	if (d == NULL) {
		return CIA402_DEVICE_RESULT_NONE;
	}
	pthread_mutex_lock(&d->lock);
	result = d->result;
	d->result = CIA402_DEVICE_RESULT_NONE;
	pthread_mutex_unlock(&d->lock);
	return result;
}

/**
 * cia402_device_abort_code - 读取最近一次失败的 SDO abort 码
 * @d: 设备对象；NULL 安全
 *
 * Return: abort 码；无记录或设备为空时返回 0。
 */
uint32_t cia402_device_abort_code(const struct cia402_device *d)
{
	uint32_t code;

	if (d == NULL) {
		return 0u;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	code = d->abort_code;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return code;
}

/**
 * cia402_device_feedback_statusword - 读取缓存的状态字
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次状态字；设备为空时返回 0。
 */
uint16_t cia402_device_feedback_statusword(const struct cia402_device *d)
{
	uint16_t value;

	if (d == NULL) {
		return 0u;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->statusword;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/**
 * cia402_device_feedback_position - 读取缓存的实际位置
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次位置；设备为空时返回 0。
 */
int32_t cia402_device_feedback_position(const struct cia402_device *d)
{
	int32_t value;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->position;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/**
 * cia402_device_feedback_velocity - 读取缓存的实际速度
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次速度；设备为空时返回 0。
 */
int32_t cia402_device_feedback_velocity(const struct cia402_device *d)
{
	int32_t value;

	if (d == NULL) {
		return 0;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->velocity;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/**
 * cia402_device_feedback_error_code - 读取缓存的驱动器错误码
 * @d: 设备对象；NULL 安全
 *
 * Return: 最近一次错误码；设备为空时返回 0。
 */
uint16_t cia402_device_feedback_error_code(const struct cia402_device *d)
{
	uint16_t value;

	if (d == NULL) {
		return 0u;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	value = d->error_code;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
	return value;
}

/* ---- 状态函数共用原语 ---- */

/**
 * dev_read - 发起一笔读事务
 * @d: 设备对象
 * @index: 对象索引
 * @timeout_ms: 本笔事务的期限
 *
 * 被拒时不在这里改状态机：登记一个失败事件，由下一拍 service 分发。
 * 这样状态函数永远只在 ENTRY / DONE 里做一次动作，不会在 ENTRY 里递归迁移。
 */
void dev_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms)
{
	d->deadline_ms = d->now_ms + timeout_ms;
	d->controlword_pending = 0u;
	if (d->access->start(d->access->ctx, SDO_DIR_READ, index, 0u, 0u, 0u, timeout_ms) != 0) {
		pend_failure(d);
		return;
	}
	d->transfer_pending = 1u;
}

/**
 * dev_write - 发起一笔写事务
 * @d: 设备对象
 * @index: 对象索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限
 *
 * 写 6040 时先暂存期望值，事务成功后才落到 controlword，
 * 免得上一次失败的控制字被后续流程当成"当前值"用。
 */
void dev_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
	       uint32_t timeout_ms)
{
	d->deadline_ms = d->now_ms + timeout_ms;
	if (index == CIA402_OD_CONTROLWORD) {
		d->pending_controlword = (uint16_t)value;
		d->controlword_pending = 1u;
	} else {
		d->controlword_pending = 0u;
	}
	if (d->access->start(d->access->ctx, SDO_DIR_WRITE, index, 0u, value, size, timeout_ms) !=
	    0) {
		d->controlword_pending = 0u;
		pend_failure(d);
		return;
	}
	d->transfer_pending = 1u;
}

/**
 * dev_finish - 结束当前流程并写结论事实
 * @d: 设备对象
 * @ok: 0 成功，非 0 失败
 * @abort_code: 失败时的 SDO abort 码
 *
 * 调用方随后迁回 st_idle。被取消的流程不走这里（结论作废）。
 */
void dev_finish(struct cia402_device *d, int ok, uint32_t abort_code)
{
	pthread_mutex_lock(&d->lock);
	if (ok != 0) {
		d->abort_code = abort_code;
		d->failed = 1u;
	}
	switch (d->running) {
	case CIA402_DEVICE_REQ_CONFIG:
		d->cfg_state = (ok == 0) ? CIA402_DEVICE_CFG_CONFIGURED : CIA402_DEVICE_CFG_FAILED;
		break;
	case CIA402_DEVICE_REQ_ENABLE:
		d->en_state = (ok == 0) ? CIA402_DEVICE_EN_ENABLED : CIA402_DEVICE_EN_FAILED;
		break;
	case CIA402_DEVICE_REQ_DISABLE:
		d->en_state = CIA402_DEVICE_EN_NOT_ENABLED;
		break;
	case CIA402_DEVICE_REQ_QUICK_STOP:
		d->en_state = (ok == 0) ? CIA402_DEVICE_EN_NOT_ENABLED : CIA402_DEVICE_EN_FAILED;
		break;
	default:
		break;
	}
	d->result = (ok == 0) ? CIA402_DEVICE_RESULT_DONE : CIA402_DEVICE_RESULT_FAILED;
	d->running = CIA402_DEVICE_REQ_NONE;
	pthread_mutex_unlock(&d->lock);
	d->controlword_pending = 0u;
}

/**
 * dev_fail - 事务失败时的统一收尾
 * @d: 设备对象
 * @result: 失败的那笔结果（取 abort 码）
 */
void dev_fail(struct cia402_device *d, const struct sdo_result *result)
{
	dev_finish(d, -1, (result != NULL) ? result->abort_code : 0u);
}

/**
 * dev_cache_statusword - 缓存最近一次状态字
 * @d: 设备对象
 * @statusword: 6041 的值
 */
void dev_cache_statusword(struct cia402_device *d, uint16_t statusword)
{
	pthread_mutex_lock(&d->lock);
	d->statusword = statusword;
	pthread_mutex_unlock(&d->lock);
}

/* ---- 事件构造与分发 ---- */

static void dispatch_start(struct cia402_device *d, enum cia402_device_req req)
{
	struct dev_start_evt evt;

	evt.super.sig = SIG_DEV_START;
	evt.req = req;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_done(struct cia402_device *d, const struct sdo_result *result)
{
	struct dev_done_evt evt;

	/* 控制字缓存只在事务成功后落地：所有写 6040 的状态共用这一处 */
	if (d->controlword_pending != 0u) {
		if (result->status == 0) {
			d->controlword = d->pending_controlword;
		}
		d->controlword_pending = 0u;
	}
	evt.super.sig = SIG_DEV_DONE;
	evt.result = *result;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_cancel(struct cia402_device *d, const struct sdo_result *result)
{
	struct dev_cancel_evt evt;

	if (result != NULL) {
		d->pending_result = *result; /* 留 abort 码给 st_idle */
	} else {
		memset(&d->pending_result, 0, sizeof(d->pending_result));
	}
	d->cancel_reason = d->cancel;
	d->cancel = CIA402_CANCEL_NONE;
	evt.super.sig = SIG_DEV_CANCEL;
	evt.reason = d->cancel_reason;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_tick(struct cia402_device *d)
{
	static const struct sm_event evt_tick = {SIG_DEV_TICK};

	sm_hsm_dispatch(&d->hsm, &evt_tick);
}

static void pend_failure(struct cia402_device *d)
{
	memset(&d->pending_result, 0, sizeof(d->pending_result));
	d->pending_result.status = -1;
	d->pending_done = 1u;
	d->controlword_pending = 0u;
}

static int submit(struct cia402_device *d, enum cia402_device_req req)
{
	int rc;

	if (d == NULL || d->access == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	if (d->running != CIA402_DEVICE_REQ_NONE || d->req != CIA402_DEVICE_REQ_NONE) {
		rc = 1;
	} else {
		d->req = req;
		rc = 0;
	}
	pthread_mutex_unlock(&d->lock);
	return rc;
}

static int submit_urgent(struct cia402_device *d, enum cia402_device_req req)
{
	if (d == NULL || d->access == NULL) {
		return -1;
	}
	pthread_mutex_lock(&d->lock);
	d->req = req;
	d->cancel = CIA402_CANCEL_REQUEST;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

static enum cia402_device_req pop_request(struct cia402_device *d)
{
	enum cia402_device_req req;

	pthread_mutex_lock(&d->lock);
	req = d->req;
	d->req = CIA402_DEVICE_REQ_NONE;
	pthread_mutex_unlock(&d->lock);
	return req;
}
