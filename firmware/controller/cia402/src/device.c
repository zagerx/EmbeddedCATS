/*
 * device.c — device.h 的实现：装配、投递、推进
 *
 * 本文件与 device.h 一一对应，只实现那三个对外动词：
 *   cia402_init()     装配
 *   cia402_request()  投递（只回答收不收）
 *   cia402_poll()     推进一拍并把事实交出去
 *
 * 内部原语（dev_read / dev_write / dev_finish …）在 device_internal.c，
 * 状态与迁移条件在 states/ 下。本文件只负责把「请求槽、事务结果、取消」
 * 翻成 hsm 事件。
 *
 * 一次 poll 最多发起一笔对象事务：这条纪律让服务线程能在多个设备之间
 * 公平轮转，任何一个设备在等应答都不会占住循环。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <stddef.h>
#include <string.h>

static int admit(struct cia402_device *d, const struct cia402_request *req);
static int pop_request(struct cia402_device *d, struct cia402_request *out);
static void dispatch_request(struct cia402_device *d, const struct cia402_request *req);
static void dispatch_sdo_done(struct cia402_device *d, const struct sdo_result *result);
static void dispatch_cancel(struct cia402_device *d, uint8_t cancel,
			    const struct sdo_result *result);
static void dispatch_tick(struct cia402_device *d);
static void set_cancel(struct cia402_device *d, uint8_t reason);
static void snapshot(struct cia402_device *d, struct cia402_status *out);

/**
 * cia402_init - 装配一台设备
 * @d: 设备对象
 * @access: 对象事务通道（设备之间共享，由上层注入）
 * @node_id: CANopen 节点号
 *
 * 装配后设备处于空闲、未配置、未使能；hsm 只保留初始转移。
 */
void cia402_init(struct cia402_device *d, const struct sdo_access *access, uint8_t node_id)
{
	memset(d, 0, sizeof(*d));
	pthread_mutex_init(&d->lock, NULL);
	d->access = access;
	d->node_id = node_id;
	sm_hsm_init(&d->hsm, &device_initial);
}

/**
 * cia402_request - 投递一条请求
 * @d: 设备对象
 * @req: 请求；HALT / QUICK_STOP 会抢占在途流程
 *
 * Return: CIA402_ACCEPTED 已受理；BUSY 上一条没结束；INVALID 请求不合法；
 * REFUSED 当前不允许（未配置就使能、未使能就运动）。
 */
enum cia402_ret cia402_request(struct cia402_device *d, const struct cia402_request *req)
{
	if (d == NULL || req == NULL) {
		return CIA402_INVALID;
	}
	if (req->kind > CIA402_REQ_QUICK_STOP) {
		return CIA402_INVALID;
	}
	if (req->kind == CIA402_REQ_MOVE &&
	    (req->pp.velocity <= 0 || req->pp.accel <= 0 || req->pp.decel <= 0)) {
		return CIA402_INVALID; /* 速度/加速度非正等于原地不动，驱动器会报错 */
	}
	return admit(d, req);
}

/**
 * cia402_poll - 推进一拍并交出这一刻的事实
 * @d: 设备对象
 * @now_ms: 当前单调毫秒时刻
 * @out: 事实输出；可为 NULL（只要推进）
 *
 * 仅服务线程调用。out 在事件分发之后填，所以本拍刚产生的结论立刻可见。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms, struct cia402_status *out)
{
	struct sdo_result result;
	struct cia402_request req;
	uint8_t cancel;

	if (d == NULL || d->access == NULL) {
		if (out != NULL) {
			memset(out, 0, sizeof(*out));
		}
		return;
	}
	d->now_ms = now_ms;

	/* 取消值在一个短临界区里取走：后面全用副本，锁内不做 I/O 也不分发 */
	pthread_mutex_lock(&d->lock);
	cancel = d->mailbox.cancel;
	pthread_mutex_unlock(&d->lock);

	/* 抢占：先打断在途事务，让 take_result 尽快给出结论 */
	if (cancel != CIA402_CANCEL_NONE && d->sdo.pending != 0u) {
		d->access->abort(d->access->ctx);
	}

	/* 上一拍发起的动作被拒：补一个事件，让状态机照常往前走 */
	if (d->sdo.rejected != 0u) {
		d->sdo.rejected = 0u;
		if (cancel != CIA402_CANCEL_NONE) {
			/* 已被抢占：旧流程的结论作废，不能再发完成事件，
			 * 否则上层会把这次旧失败当成新一轮停车的失败。 */
			set_cancel(d, cancel);
			dispatch_cancel(d, cancel, NULL);
		} else {
			result = d->sdo.result;
			dispatch_sdo_done(d, &result);
		}
		goto done;
	}

	if (d->sdo.pending != 0u) {
		if (d->access->take_result(d->access->ctx, &result) != 0) {
			/* 结果还没到：只判单笔事务的期限 */
			if (cancel == CIA402_CANCEL_NONE &&
			    (int32_t)(now_ms - d->sdo.deadline) >= 0) {
				set_cancel(d, CIA402_CANCEL_TIMEOUT);
				d->access->abort(d->access->ctx);
			}
			dispatch_tick(d);
			goto done;
		}
		d->sdo.pending = 0u;
		if (cancel != CIA402_CANCEL_NONE) {
			dispatch_cancel(d, cancel, &result); /* 结论作废，abort 码留着 */
		} else {
			dispatch_sdo_done(d, &result);
		}
		goto done;
	}

	if (cancel != CIA402_CANCEL_NONE) {
		dispatch_cancel(d, cancel, NULL);
		goto done;
	}

	if (pop_request(d, &req) != 0) {
		dispatch_request(d, &req);
		goto done;
	}

	dispatch_tick(d);

done:
	if (out != NULL) {
		snapshot(d, out);
	}
}

/**
 * cia402_get_feedback - 读取最新原始反馈
 * @d: 设备对象；NULL 安全
 * @out: 输出反馈；NULL 时直接返回
 *
 * 任意线程均可调用，不触发对象事务。
 */
void cia402_get_feedback(const struct cia402_device *d, struct cia402_feedback *out)
{
	if (d == NULL || out == NULL) {
		return;
	}
	pthread_mutex_lock((pthread_mutex_t *)&d->lock);
	*out = d->feedback;
	pthread_mutex_unlock((pthread_mutex_t *)&d->lock);
}

static int admit(struct cia402_device *d, const struct cia402_request *req)
{
	int urgent = (req->kind == CIA402_REQ_HALT || req->kind == CIA402_REQ_QUICK_STOP);
	int rc = CIA402_ACCEPTED;

	pthread_mutex_lock(&d->lock);

	/* 状态前提：未配置不能使能，未使能不能运动 */
	if (req->kind == CIA402_REQ_ENABLE && d->outbox.configured == 0u) {
		rc = CIA402_REFUSED;
	} else if (req->kind == CIA402_REQ_MOVE && d->outbox.enabled == 0u) {
		rc = CIA402_REFUSED;
	} else if (!urgent && (d->mailbox.executing != 0u || d->mailbox.pending != 0u)) {
		rc = CIA402_BUSY; /* 普通请求排队：忙就拒，由调用方重试 */
	}

	if (rc == CIA402_ACCEPTED) {
		d->mailbox.req = *req;
		d->mailbox.pending = 1u;
		/* 新流程开始：上一条的结论作废，免得被当成这一条的结论读走 */
		d->outbox.result = CIA402_RESULT_NONE;
		d->outbox.fail = CIA402_FAIL_NONE;
		if (urgent) {
			d->mailbox.cancel = CIA402_CANCEL_REQUEST; /* 抢占在途流程 */
		}
	}
	pthread_mutex_unlock(&d->lock);
	return rc;
}

static int pop_request(struct cia402_device *d, struct cia402_request *out)
{
	int has;

	pthread_mutex_lock(&d->lock);
	has = d->mailbox.pending;
	if (has != 0) {
		*out = d->mailbox.req;
		d->mailbox.pending = 0u;
	}
	pthread_mutex_unlock(&d->lock);
	return has;
}

static void dispatch_request(struct cia402_device *d, const struct cia402_request *req)
{
	struct request_evt evt;

	evt.super.sig = CIA402_SIG_REQUEST;
	evt.req = *req;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_sdo_done(struct cia402_device *d, const struct sdo_result *result)
{
	struct sdo_done_evt evt;

	/* 控制字镜像只在事务成功后落地：所有写 6040 的状态共用这一处 */
	if (d->sdo.cw_next_valid != 0u) {
		if (result->status == 0) {
			d->sdo.cw_ok = d->sdo.cw_next;
		}
		d->sdo.cw_next_valid = 0u;
	}
	evt.super.sig = CIA402_SIG_SDO_DONE;
	evt.result = *result;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_cancel(struct cia402_device *d, uint8_t cancel,
			    const struct sdo_result *result)
{
	struct cancel_evt evt;

	if (result != NULL) {
		d->sdo.result = *result; /* 留 abort 码给 st_idle */
	} else {
		memset(&d->sdo.result, 0, sizeof(d->sdo.result));
	}
	pthread_mutex_lock(&d->lock);
	d->mailbox.cancel = CIA402_CANCEL_NONE;
	pthread_mutex_unlock(&d->lock);
	d->operation.cancel = cancel;
	evt.super.sig = CIA402_SIG_CANCEL;
	evt.reason = cancel;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_tick(struct cia402_device *d)
{
	static const struct sm_event evt_tick = {CIA402_SIG_TICK};

	sm_hsm_dispatch(&d->hsm, &evt_tick);
}

static void set_cancel(struct cia402_device *d, uint8_t reason)
{
	pthread_mutex_lock(&d->lock);
	d->mailbox.cancel = reason;
	pthread_mutex_unlock(&d->lock);
}

static void snapshot(struct cia402_device *d, struct cia402_status *out)
{
	pthread_mutex_lock(&d->lock);
	*out = d->outbox; /* 内部就是同一类型，整块抄走 */
	pthread_mutex_unlock(&d->lock);
}
