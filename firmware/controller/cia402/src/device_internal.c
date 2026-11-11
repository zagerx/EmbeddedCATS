/*
 * device_internal.c — 设备层内部原语（只给 states/ 用）
 *
 * 这里实现的函数全部声明在 device_internal.h：模块外拿不到，
 * 所以它们不属于 device.c（device.c 只对应 device.h 的对外三个动词）。
 *
 * 三件事：
 *   1. 发起一笔对象事务（dev_read / dev_write）；
 *   2. 收尾一个流程（dev_finish / dev_fail）；
 *   3. states/ 需要读的几个事实（状态字、流程类别、阶段）。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <string.h>

/* 发起被拒：登记一个失败事件，由下一拍 poll 分发 */
static void pend_failure(struct cia402_device *d);

/**
 * dev_read - 发起一笔读事务
 * @d: 设备对象
 * @index: 对象索引
 * @timeout_ms: 本笔事务的期限
 *
 * 被拒时不在这里改状态机：登记一个失败事件，由下一拍 poll 分发。
 * 这样状态函数永远只在 ENTRY / DONE 里做一次动作，不会在 ENTRY 里递归迁移。
 */
void dev_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms)
{
	d->sdo.deadline = d->now_ms + timeout_ms;
	d->sdo.cw_next_valid = 0u;
	if (d->access->start(d->access->ctx, SDO_DIR_READ, index, 0u, 0u, 0u, timeout_ms) != 0) {
		pend_failure(d);
		return;
	}
	d->sdo.pending = 1u;
}

/**
 * dev_write - 发起一笔写事务
 * @d: 设备对象
 * @index: 对象索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限
 *
 * 写 6040 时先暂存期望值，事务成功后才落到镜像，
 * 免得上一次失败的控制字被后续流程当成"当前值"用。
 */
void dev_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
	       uint32_t timeout_ms)
{
	d->sdo.deadline = d->now_ms + timeout_ms;
	if (index == CIA402_OD_CONTROLWORD) {
		d->sdo.cw_next = (uint16_t)value;
		d->sdo.cw_next_valid = 1u;
	} else {
		d->sdo.cw_next_valid = 0u;
	}
	if (d->access->start(d->access->ctx, SDO_DIR_WRITE, index, 0u, value, size, timeout_ms) !=
	    0) {
		d->sdo.cw_next_valid = 0u;
		pend_failure(d);
		return;
	}
	d->sdo.pending = 1u;
}

/**
 * dev_finish - 结束当前流程并写结论
 * @d: 设备对象
 * @result: DONE 或 FAILED
 * @fail: 失败原因；成功时传 CIA402_FAIL_NONE
 *
 * 调用方随后迁回 st_idle。被取消的流程不走这里（结论作废）。
 */
void dev_finish(struct cia402_device *d, enum cia402_result result, enum cia402_fail fail)
{
	pthread_mutex_lock(&d->lock);
	if (result == CIA402_RESULT_FAILED) {
		d->outbox.status.failed = 1u;
	}
	switch (d->running.kind) {
	case CIA402_REQ_CONFIG:
		d->outbox.status.configured = (uint8_t)(result == CIA402_RESULT_DONE);
		break;
	case CIA402_REQ_ENABLE:
		d->outbox.status.enabled = (uint8_t)(result == CIA402_RESULT_DONE);
		break;
	case CIA402_REQ_DISABLE:
		/* 只有确认断开才改；失败时保持原值，免得上层误以为已经断了 */
		if (result == CIA402_RESULT_DONE) {
			d->outbox.status.enabled = 0u;
		}
		break;
	case CIA402_REQ_QUICK_STOP:
		if (result == CIA402_RESULT_DONE) {
			d->outbox.status.enabled = 0u;
		}
		break;
	default:
		break;
	}
	d->outbox.status.result = result;
	d->outbox.status.fail = fail;
	d->mailbox.executing = 0u;
	pthread_mutex_unlock(&d->lock);
	d->sdo.cw_next_valid = 0u;
}

/**
 * dev_fail - 事务失败时的统一收尾
 * @d: 设备对象
 * @result: 失败的那笔结果（取 abort 码）
 */
void dev_fail(struct cia402_device *d, const struct sdo_result *result)
{
	enum cia402_fail fail = CIA402_FAIL_SDO;

	if (result == NULL) {
		fail = CIA402_FAIL_NONE;
	} else if (result->abort_code == 0u) {
		fail = CIA402_FAIL_TIMEOUT; /* 没有 abort 码的失败多半是超时 */
	}
	pthread_mutex_lock(&d->lock);
	d->outbox.status.abort_code = (result != NULL) ? result->abort_code : 0u;
	pthread_mutex_unlock(&d->lock);
	dev_finish(d, CIA402_RESULT_FAILED, fail);
}

/**
 * dev_cache_statusword - 缓存最近一次状态字
 * @d: 设备对象
 * @statusword: 6041 的值
 */
void dev_cache_statusword(struct cia402_device *d, uint16_t statusword)
{
	pthread_mutex_lock(&d->lock);
	d->outbox.status.statusword = statusword;
	pthread_mutex_unlock(&d->lock);
}

/**
 * dev_running_kind - 取当前在执行的请求类别
 * @d: 设备对象
 *
 * Return: 当前流程类别；没有流程时返回 CIA402_REQ_CONFIG（调用方应先用
 * mailbox.executing 判断）。
 */
enum cia402_kind dev_running_kind(const struct cia402_device *d)
{
	return d->running.kind;
}

/**
 * dev_phase_of - 请求类别映射为对外报告的阶段
 * @kind: 请求类别
 *
 * Return: 对应的阶段；未知类别返回 CIA402_PHASE_IDLE。
 */
enum cia402_phase dev_phase_of(enum cia402_kind kind)
{
	switch (kind) {
	case CIA402_REQ_CONFIG:
		return CIA402_PHASE_CONFIG;
	case CIA402_REQ_ENABLE:
		return CIA402_PHASE_ENABLE;
	case CIA402_REQ_DISABLE:
		return CIA402_PHASE_DISABLE;
	case CIA402_REQ_MOVE:
		return CIA402_PHASE_MOVE;
	case CIA402_REQ_HALT:
		return CIA402_PHASE_HALT;
	case CIA402_REQ_QUICK_STOP:
		return CIA402_PHASE_QUICK_STOP;
	default:
		return CIA402_PHASE_IDLE;
	}
}

static void pend_failure(struct cia402_device *d)
{
	memset(&d->sdo.result, 0, sizeof(d->sdo.result));
	d->sdo.result.status = -1;
	d->sdo.rejected = 1u;
	d->sdo.cw_next_valid = 0u;
}
