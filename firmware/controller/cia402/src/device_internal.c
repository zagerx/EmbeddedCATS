/*
 * device_internal.c — 设备层内部原语（只给 states/ 用）
 *
 * 这里实现的函数全部声明在 device_internal.h：模块外拿不到，
 * 所以它们不属于 device.c（device.c 只对应 device.h 的对外接口）。
 *
 * 两件事：
 *   1. 发起一笔对象事务（dev_read / dev_write），在途状态归通道所有；
 *   2. 收尾流程并写主站确认结果，或提供流程阶段。
 *
 * Author: zager
 */

#include "device_internal.h"

/**
 * dev_read - 发起一笔读事务
 * @d: 设备对象
 * @index: 对象索引
 * @timeout_ms: 本笔事务的期限，由通道掌握
 *
 * 事务的在途状态与期限都归通道所有：这里只发起，结果统一由 cia402_poll 派发。
 * 发起即使被拒，通道也保证交付一笔结果，状态机不会等不到事件。
 */
void dev_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms)
{
	(void)d->access->start(d->access->ctx, SDO_DIR_READ, index, 0u, 0u, 0u, timeout_ms);
}

/**
 * dev_write - 发起一笔写事务
 * @d: 设备对象
 * @index: 对象索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限，由通道掌握
 *
 * 写 6040 时先暂存期望值，事务成功后才落到镜像，
 * 免得上一次失败的控制字被后续流程当成"当前值"用。
 */
void dev_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
	       uint32_t timeout_ms)
{
	if (index == CIA402_OD_CONTROLWORD) {
		d->cw.pending = (uint16_t)value;
		d->cw.pending_valid = 1u;
	}
	(void)d->access->start(d->access->ctx, SDO_DIR_WRITE, index, 0u, value, size, timeout_ms);
}

/**
 * dev_finish - 结束当前流程并写主站确认结果
 * @d: 设备对象
 * @result: DONE 或 FAILED
 * @fail: 失败原因；成功时传 CIA402_OP_FAIL_NONE
 *
 * 模式与使能确认只由确认过的流程在这里更新，不随普通反馈读取改变。
 * 调用方随后迁回 st_idle；被取消的流程不走这里，原结论作废。
 */
void dev_finish(struct cia402_device *d, enum cia402_operation_result result,
		enum cia402_operation_fail fail)
{
	if (result == CIA402_OP_FAILED) {
		d->outbox.operation_failed = 1u;
	}
	switch (d->operation.kind) {
	case CIA402_REQ_CONFIG:
		d->outbox.mode_confirmed = (uint8_t)(result == CIA402_OP_DONE);
		break;
	case CIA402_REQ_ENABLE:
		d->outbox.enable_confirmed = (uint8_t)(result == CIA402_OP_DONE);
		break;
	case CIA402_REQ_DISABLE:
		/* 只有确认断开才改；失败时保持原值，免得上层误以为已经断了 */
		if (result == CIA402_OP_DONE) {
			d->outbox.enable_confirmed = 0u;
		}
		break;
	case CIA402_REQ_QUICK_STOP:
		if (result == CIA402_OP_DONE) {
			d->outbox.enable_confirmed = 0u;
		}
		break;
	default:
		break;
	}
	d->outbox.last.result = result;
	d->outbox.last.fail = fail;
	/* 流程自己收尾：把 phase 归位，st_idle 的 ENTRY 据此知道不必再补结论。
	 * 被打断退出（SIG_DEV_CANCEL）不走这里，phase 仍是流程的。 */
	d->outbox.phase = CIA402_PHASE_IDLE;
	d->cw.pending_valid = 0u;
}

/**
 * dev_fail - 事务失败时的统一收尾
 * @d: 设备对象
 * @result: 失败的那笔结果（取 abort 码）
 */
void dev_fail(struct cia402_device *d, const struct sdo_result *result)
{
	enum cia402_operation_fail fail = CIA402_OP_FAIL_SDO;

	if (result == NULL) {
		fail = CIA402_OP_FAIL_NONE;
	} else if (result->status == SDO_STATUS_TIMEOUT || result->abort_code == 0u) {
		/* 通道判定期限到，或没有 abort 码的失败：驱动器没按期应答 */
		fail = CIA402_OP_FAIL_TIMEOUT;
	}
	d->outbox.last.abort_code = (result != NULL) ? result->abort_code : 0u;
	dev_finish(d, CIA402_OP_FAILED, fail);
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
	}
	/* 故意不写 default：漏了某个 kind 时由 -Wswitch 报出来。
	 * 走到这里说明 kind 越界（cia402_request 已校验过），返回 IDLE 不冒充"忙"。 */
	return CIA402_PHASE_IDLE;
}
