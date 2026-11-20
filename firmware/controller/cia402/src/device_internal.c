/*
 * device_internal.c — 设备层内部原语（只给 states/ 用）
 *
 * 这里实现的函数全部声明在 device_internal.h：模块外拿不到，
 * 所以它们不属于 device.c（device.c 只对应 device.h 的对外接口）。
 *
 * 两件事：
 *   1. 发起一笔对象事务（dev_read / dev_write），在途状态归通道所有；
 *   2. 收尾流程并写下回执。
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
 * 写 6040 时先暂存到 pending，事务成功后才落到 confirmed 镜像。
 */
void dev_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
	       uint32_t timeout_ms)
{
	dev_write_sub(d, index, 0u, value, size, timeout_ms);
}

/**
 * dev_write_sub - 发起一笔带子索引的写事务
 * @d: 设备对象
 * @index: 对象索引
 * @sub: 子索引；数组类对象（如 6099h）用
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限，由通道掌握
 */
void dev_write_sub(struct cia402_device *d, uint16_t index, uint8_t sub, uint32_t value,
		   uint8_t size, uint32_t timeout_ms)
{
	if (index == CIA402_OD_CONTROLWORD) {
		d->cw.pending = (uint16_t)value;
		d->cw.pending_valid = 1u;
	}
	(void)d->access->start(d->access->ctx, SDO_DIR_WRITE, index, sub, value, size, timeout_ms);
}

/**
 * dev_finish - 结束当前流程并写下回执
 * @d: 设备对象
 * @result: DONE 或 FAILED
 * @fail: 失败原因；成功时传 CIA402_FAIL_NONE
 *
 * 写回执三项，并清 running 表示流程已自行收尾。
 * 被取消的流程不走这里。
 */
void dev_finish(struct cia402_device *d, enum cia402_outcome result, enum cia402_fail fail)
{
	d->outbox.outcome = result;
	d->outbox.fail = fail;
	/* 流程自己收尾：running 清零；被打断退出的流程仍有 running = 1。 */
	d->operation.running = 0u;
	d->cw.pending_valid = 0u;
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
	} else if (result->status == SDO_STATUS_TIMEOUT || result->abort_code == 0u) {
		/* 通道判定期限到，或没有 abort 码的失败：驱动器没按期应答 */
		fail = CIA402_FAIL_TIMEOUT;
	}
	d->outbox.abort_code = (result != NULL) ? result->abort_code : 0u;
	dev_finish(d, CIA402_OUTCOME_FAILED, fail);
}
