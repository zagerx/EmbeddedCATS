/*
 * device.c — device.h 的实现：装配、投递、推进、取结果
 *
 * 本文件与 device.h 一一对应，只实现对外接口：
 *   cia402_init()     装配
 *   cia402_request()  投递（只回答收不收）
 *   cia402_poll()     推进一拍并交出回执
 *   cia402_get_result() / cia402_take_result() 读取回执 / 消费回执
 *
 * 内部原语（dev_read / dev_write / dev_finish …）在 device_internal.c，
 * 状态骨架在 states/，操作流程在 operations/。本文件只负责把「邮箱、事务结果、取消」
 * 翻成 hsm 事件。
 *
 * 一次 poll 最多发起一笔对象事务：这条纪律让工作线程能在多个设备之间
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
static void dispatch_cancel(struct cia402_device *d, uint8_t reason,
			    const struct sdo_result *result);
static void dispatch_tick(struct cia402_device *d);
static void snapshot(const struct cia402_device *d, struct cia402_operation_result *out);

/**
 * cia402_init - 装配一台设备
 * @d: 设备对象
 * @access: 对象事务通道，由上层注入
 *
 * 装配后设备处于空闲；hsm 只保留初始转移。
 */
void cia402_init(struct cia402_device *d, const struct sdo_access *access)
{
	memset(d, 0, sizeof(*d));
	d->access = access;
	sm_hsm_init(&d->hsm, &device_initial);
}

/**
 * cia402_request - 投递一条请求
 * @d: 设备对象
 * @req: 请求；HALT / QUICK_STOP 会抢占在途流程
 *
 * 仅 CANopen 工作线程调用；请求直接登记，状态迁移由 poll 统一完成。
 *
 * Return: 见 enum cia402_response；HALT / QUICK_STOP 不受 BUSY 限制。
 */
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req)
{
	if (d == NULL || req == NULL) {
		return CIA402_INVALID;
	}
	if (req->kind > CIA402_REQ_ESTOP_CLEAR) {
		return CIA402_INVALID;
	}
	if (req->kind == CIA402_REQ_MOVE &&
	    (req->pp.velocity <= 0 || req->pp.accel <= 0 || req->pp.decel <= 0)) {
		return CIA402_INVALID; /* 速度/加速度非正等于原地不动，驱动器会报错 */
	}
	return admit(d, req);
}

/**
 * cia402_poll - 推进一拍并交出回执
 * @d: 设备对象
 * @now_ms: 当前单调毫秒时刻
 * @out: 回执输出；可为 NULL（只推进）
 *
 * 仅工作线程调用。out 在事件分发之后填，所以本拍刚产生的结论立刻可见。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms, struct cia402_operation_result *out)
{
	struct sdo_result result;
	struct cia402_request req;
	enum sdo_poll state;
	uint8_t cancel;

	if (d == NULL || d->access == NULL) {
		if (out != NULL) {
			memset(out, 0, sizeof(*out));
		}
		return;
	}
	d->now_ms = now_ms;

	/* 读取本轮取消原因，后续事件分发统一使用这个副本 */
	cancel = d->mailbox.cancel;

	/* 抢占：先打断在途事务，结论由取消事件统一给出 */
	if (cancel != CIA402_CANCEL_NONE) {
		d->access->abort(d->access->ctx);
	}

	state = d->access->poll(d->access->ctx, &result);
	if (state == SDO_POLL_DONE) {
		if (cancel != CIA402_CANCEL_NONE) {
			dispatch_cancel(d, cancel, &result); /* 结论作废，abort 码留着 */
		} else if (result.status == SDO_STATUS_TIMEOUT) {
			dispatch_cancel(d, CIA402_CANCEL_TIMEOUT, &result);
		} else {
			dispatch_sdo_done(d, &result);
		}
		goto done;
	}
	if (state == SDO_POLL_WAITING) {
		/* 有在途事务（含抢占后等 abort 结果）：本拍不推进流程 */
		goto done;
	}

	/* 通道空闲：先把取消结清，再受理新请求，最后才走一拍空转 */
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
 * cia402_get_result - 读取最近一次操作的回执
 * @d: 设备对象；NULL 安全
 * @out: 输出回执；NULL 时直接返回
 *
 * 仅 CANopen 工作线程调用；跨线程查询使用控制层发布快照。
 */
void cia402_get_result(const struct cia402_device *d, struct cia402_operation_result *out)
{
	if (d == NULL || out == NULL) {
		return;
	}
	snapshot(d, out);
}

/**
 * cia402_take_result - 取走最近一次操作结论
 * @d: 设备对象；NULL 安全
 *
 * 取走后结果恢复 NONE，失败原因和 abort 码保留用于诊断。
 *
 * Return: DONE、FAILED 或 NONE；@d 为 NULL 时返回 NONE。
 */
enum cia402_outcome cia402_take_result(struct cia402_device *d)
{
	enum cia402_outcome result;

	if (d == NULL) {
		return CIA402_OUTCOME_NONE;
	}
	result = d->outbox.outcome;
	d->outbox.outcome = CIA402_OUTCOME_NONE;
	return result;
}

static int admit(struct cia402_device *d, const struct cia402_request *req)
{
	int urgent = (req->kind == CIA402_REQ_HALT || req->kind == CIA402_REQ_QUICK_STOP);

	/* 只判断上一件是否还在跑；流程前提由调用方负责 */
	if (!urgent && (d->operation.running != 0u || d->mailbox.pending != 0u)) {
		return CIA402_BUSY; /* 忙就拒，不排队 */
	}

	d->mailbox.req = *req;
	d->mailbox.pending = 1u;
	/* 新流程开始：清空上一条的回执 */
	d->outbox.outcome = CIA402_OUTCOME_NONE;
	d->outbox.fail = CIA402_FAIL_NONE;
	if (urgent) {
		d->mailbox.cancel = CIA402_CANCEL_REQUEST; /* 抢占在途流程 */
	}
	return CIA402_ACCEPTED;
}

static int pop_request(struct cia402_device *d, struct cia402_request *out)
{
	int has;

	has = d->mailbox.pending;
	if (has != 0) {
		*out = d->mailbox.req;
		d->mailbox.pending = 0u;
	}
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
	if (d->cw.pending_valid != 0u) {
		if (result->status == 0) {
			d->cw.confirmed = d->cw.pending;
		}
		d->cw.pending_valid = 0u;
	}
	evt.super.sig = CIA402_SIG_SDO_DONE;
	evt.result = *result;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_cancel(struct cia402_device *d, uint8_t reason,
			    const struct sdo_result *result)
{
	struct cancel_evt evt;

	d->mailbox.cancel = CIA402_CANCEL_NONE;
	d->operation.cancel = reason;
	if (result != NULL) {
		evt.result = *result;
	} else {
		memset(&evt.result, 0, sizeof(evt.result));
	}
	evt.super.sig = CIA402_SIG_CANCEL;
	evt.reason = reason;
	sm_hsm_dispatch(&d->hsm, &evt.super);
}

static void dispatch_tick(struct cia402_device *d)
{
	static const struct sm_event evt_tick = {CIA402_SIG_TICK};

	sm_hsm_dispatch(&d->hsm, &evt_tick);
}

/* 只拷贝回执三项 */
static void snapshot(const struct cia402_device *d, struct cia402_operation_result *out)
{
	out->outcome = d->outbox.outcome;
	out->fail = d->outbox.fail;
	out->abort_code = d->outbox.abort_code;
}
