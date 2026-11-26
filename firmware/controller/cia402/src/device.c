/*
 * device.c — device.h 的实现：装配、投递、推进、取结果
 *
 * 本文件与 device.h 一一对应，只实现对外接口：
 *   cia402_init()     装配
 *   cia402_request()  投递（只回答收不收）
 *   cia402_poll()     推进协议流程
 *   cia402_failure_of() / cia402_take_result() 读归类 / 消费结局
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
#include <stdio.h>
#include <string.h>

static int admit(struct cia402_device *d, const struct cia402_request *req);
static int pop_request(struct cia402_device *d, struct cia402_request *out);
static void dispatch_request(struct cia402_device *d, const struct cia402_request *req);
static void dispatch_sdo_done(struct cia402_device *d, const struct sdo_result *result);
static uint32_t cia402_flow_timeout_ms(enum cia402_kind kind);
static void dispatch_cancel(struct cia402_device *d, uint8_t reason,
			    const struct sdo_result *result);
static void dispatch_tick(struct cia402_device *d);

/**
 * cia402_init - 装配一台设备
 * @d: 设备对象
 * @client: 对象事务客户端，由上层注入
 *
 * 装配后设备处于空闲；hsm 只保留初始转移。
 */
void cia402_init(struct cia402_device *d, struct sdo_client *client)
{
	memset(d, 0, sizeof(*d));
	d->client = client;
	sm_hsm_init(&d->hsm, &device_initial);
}

/**
 * cia402_request - 投递一条请求
 * @d: 设备对象
 * @req: 请求；HALT / QUICK_STOP 会抢占在途流程，MOVE 会改在途 PP 的目标
 *
 * 仅 CANopen 工作线程调用；请求直接登记，状态迁移由 poll 统一完成。
 *
 * Return: 见 enum cia402_response；HALT / QUICK_STOP 不受 BUSY 限制，
 *         在途 PP 的新目标也不受 BUSY 限制。
 */
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req)
{
	if (d == NULL || req == NULL) {
		return CIA402_INVALID;
	}
	if (req->kind > CIA402_REQ_HOME) {
		return CIA402_INVALID;
	}
	if (req->kind == CIA402_REQ_MOVE &&
	    (req->pp.velocity <= 0 || req->pp.accel <= 0 || req->pp.decel <= 0)) {
		return CIA402_INVALID; /* 速度/加速度非正等于原地不动，驱动器会报错 */
	}
	return admit(d, req);
}

/**
 * cia402_poll - 推进一拍
 * @d: 设备对象
 * @now_ms: 当前单调毫秒时刻
 *
 * 仅工作线程调用；成功或失败通过 cia402_take_result() 取走。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms)
{
	struct sdo_result result;
	struct cia402_request req;
	enum sdo_poll state;
	uint8_t cancel;

	if (d == NULL || d->client == NULL) {
		return;
	}
	d->now_ms = now_ms;
	cancel = d->mailbox.cancel;

	if (d->sdo_unstarted != 0u) {
		/* 未受理已在发起时判过失败；这里只把状态机从实际子状态带出去 */
		d->sdo_unstarted = 0u;
		dispatch_cancel(d, cancel != CIA402_CANCEL_NONE ? cancel : CIA402_CANCEL_REQUEST,
				NULL);
		return;
	}

	if (d->sdo_pending != 0u) {
		if (cancel != CIA402_CANCEL_NONE) {
			sdo_client_abort(d->client);
		}
		state = sdo_client_poll(d->client, &result);
		if (state == SDO_POLL_WAITING) {
			return;
		}
		if (state != SDO_POLL_DONE) {
			/* 已受理事务不能无声丢失：这是实现错误，不是驱动器超时 */
			fprintf(stderr, "[cia402] 已受理事务丢失: kind=%d\n",
				(int)d->operation.kind);
			d->operation.running = 0u;
			d->mailbox.pending = 0u;
			d->mailbox.cancel = CIA402_CANCEL_NONE;
			d->cw.pending_valid = 0u;
			d->sdo_pending = 0u;
			return;
		}
		d->sdo_pending = 0u;
		if (cancel != CIA402_CANCEL_NONE) {
			dispatch_cancel(d, cancel, &result);
			return;
		}
		if (result.status == SDO_STATUS_CANCELLED) {
			/* 未请求取消却收到取消结果，不能冒充新操作的失败 */
			d->cw.pending_valid = 0u;
			return;
		}
		if (result.status == SDO_STATUS_TIMEOUT) {
			dispatch_cancel(d, CIA402_CANCEL_TIMEOUT, &result);
			return;
		}
		dispatch_sdo_done(d, &result);
		return;
	}

	if (cancel != CIA402_CANCEL_NONE) {
		dispatch_cancel(d, cancel, NULL);
		return;
	}
	if (pop_request(d, &req) != 0) {
		dispatch_request(d, &req);
		return;
	}
	dispatch_tick(d);
}

/**
 * cia402_is_running - 查询是否还有待执行或取消清理的流程
 * @d: 设备对象
 * Return: 非 0 表示已受理未出邮箱，或仍在执行；NULL 为空闲。
 */
int cia402_is_running(const struct cia402_device *d)
{
	/* 有明确流程身份才属于正在等待；后继安全请求不延长被取消的原流程 */
	return d != NULL && (d->operation.running != 0u ||
			     (d->mailbox.pending != 0u && d->mailbox.cancel == CIA402_CANCEL_NONE));
}

/**
 * cia402_failure_of - 读取最近一次执行失败的归类
 * @d: 设备对象；NULL 安全
 * @out: 归类输出；NULL 时直接返回
 */
void cia402_failure_of(const struct cia402_device *d, enum cia402_fail *out)
{
	if (d != NULL && out != NULL) {
		*out = d->failure;
	}
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
	result = d->outbox;
	d->outbox = CIA402_OUTCOME_NONE;
	return result;
}

/**
 * cia402_flow_timeout_ms - 一条流程从受理到收尾最多允许多久
 * @kind: 请求类型
 *
 * 比该流程内最长的单步期限再留一点余量，只作为父状态看门狗的上限：
 * 任何一步静默卡住（拿不到事务结果）都不会超过这个时间。
 *
 * Return: 期限毫秒数。
 */
static uint32_t cia402_flow_timeout_ms(enum cia402_kind kind)
{
	switch (kind) {
	case CIA402_REQ_MOVE:
		return CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS + 5000u;
	case CIA402_REQ_HOME:
		return CIA402_DEVICE_HOMING_TIMEOUT_MS + 5000u;
	case CIA402_REQ_HALT:
	case CIA402_REQ_QUICK_STOP:
		return CIA402_DEVICE_HALT_TIMEOUT_MS + 2000u;
	default:
		/* 配置、使能、断使能、解除急停：几步 SDO，给足余量 */
		return 10000u;
	}
}

static int admit(struct cia402_device *d, const struct cia402_request *req)
{
	int urgent = (req->kind == CIA402_REQ_HALT || req->kind == CIA402_REQ_QUICK_STOP);

	/*
	 * PP 运动中的新目标：驱动器支持在线变位（6040 = 0x103F），能在不停车、
	 * 不重复起步的前提下换目标。所以这里不打断流程，只把新目标存起来，等
	 * “读到状态字”那一步生效。结论仍归这一条流程：上层看到的是一段连续
	 * 运动，走到最后一个目标为止。
	 */
	if (req->kind == CIA402_REQ_MOVE && d->operation.running != 0u &&
	    d->operation.kind == CIA402_REQ_MOVE && d->mailbox.pending == 0u) {
		d->operation.retarget.target = req->pp.target;
		d->operation.retarget.velocity = req->pp.velocity;
		d->operation.retarget.accel = req->pp.accel;
		d->operation.retarget.decel = req->pp.decel;
		d->operation.retarget_valid = 1u; /* 后到的覆盖先到的：只认最新目标 */
		/* 新目标重新给足整条流程的余量，长距离跟踪不会被看门狗误判 */
		d->operation.flow_deadline = d->now_ms + cia402_flow_timeout_ms(req->kind);
		return CIA402_ACCEPTED;
	}

	/* 只判断上一件是否还在跑；流程前提由调用方负责 */
	if (!urgent && (d->operation.running != 0u || d->mailbox.pending != 0u)) {
		return CIA402_BUSY; /* 忙就拒，不排队 */
	}

	d->mailbox.req = *req;
	d->mailbox.pending = 1u;
	/* 新流程只清掉未取的旧结局；取消不生成任何新结局 */
	d->outbox = CIA402_OUTCOME_NONE;
	/* 整条流程的总期限：任何一步静默卡住都由父状态看门狗在此兜底 */
	d->operation.flow_deadline = d->now_ms + cia402_flow_timeout_ms(req->kind);
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
