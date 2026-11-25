/*
 * device_internal.c — 设备层内部原语（只给 states/ 用）
 *
 * 这里实现的函数全部声明在 device_internal.h：模块外拿不到，
 * 所以它们不属于 device.c（device.c 只对应 device.h 的对外接口）。
 *
 * 两件事：
 *   1. 发起一笔对象事务（dev_read / dev_write / dev_write_pulse），在途状态归
 *      通道所有；
 *   2. 收尾流程并写下回执。
 *
 * Author: zager
 */

#include "device_internal.h"

#include <stdio.h>

/**
 * dev_read - 发起一笔读事务
 * @d: 设备对象
 * @index: 对象索引
 * @timeout_ms: 本笔事务的期限，由通道掌握
 *
 * Return: ACCEPTED 表示本设备拥有在途事务；其余返回没有结果。
 */
enum sdo_start_result dev_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms)
{
	enum sdo_start_result started;

	started = sdo_client_read(d->client, index, 0u, timeout_ms);
	if (started == SDO_START_ACCEPTED) {
		d->sdo_pending = 1u;
	}
	return started;
}

/**
 * dev_write - 发起一笔写事务
 * @d: 设备对象
 * @index: 对象索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限，由通道掌握
 *
 * Return: ACCEPTED 表示本设备拥有在途事务；其余返回没有结果。
 */
enum sdo_start_result dev_write(struct cia402_device *d, uint16_t index, uint32_t value,
				uint8_t size, uint32_t timeout_ms)
{
	return dev_write_sub(d, index, 0u, value, size, timeout_ms);
}

/**
 * dev_write_sub - 发起一笔带子索引的写事务
 * @d: 设备对象
 * @index: 对象索引
 * @sub: 子索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限，由通道掌握
 *
 * Return: ACCEPTED 表示本设备拥有在途事务；其余返回没有结果。
 */
enum sdo_start_result dev_write_sub(struct cia402_device *d, uint16_t index, uint8_t sub,
				    uint32_t value, uint8_t size, uint32_t timeout_ms)
{
	enum sdo_start_result started;

	started = sdo_client_write(d->client, index, sub, value, size, timeout_ms);
	if (started == SDO_START_ACCEPTED) {
		d->sdo_pending = 1u;
		if (index == CIA402_OD_CONTROLWORD) {
			d->cw.pending = (uint16_t)value;
			d->cw.pending_valid = 1u;
		}
	}
	return started;
}

/**
 * dev_write_pulse - 写一笔不进控制字镜像的 6040
 * @d: 设备对象
 * @value: 控制字值
 * @timeout_ms: 本笔事务的期限，由通道掌握
 *
 * Return: ACCEPTED 表示本设备拥有在途事务；其余返回没有结果。
 */
enum sdo_start_result dev_write_pulse(struct cia402_device *d, uint16_t value, uint32_t timeout_ms)
{
	enum sdo_start_result started;

	started = sdo_client_write(d->client, CIA402_OD_CONTROLWORD, 0u, (uint32_t)value, 2u,
				   timeout_ms);
	if (started == SDO_START_ACCEPTED) {
		d->sdo_pending = 1u;
	}
	return started;
}

/**
 * dev_not_started - 未受理事务的立即收尾
 * @d: 设备对象
 * @started: 客户端的未受理原因
 *
 * Return: 消费当前 ENTRY；下一拍取消流程且不交付结局。
 */
sm_ret_t dev_not_started(struct cia402_device *d, enum sdo_start_result started)
{
	fprintf(stderr, "[cia402] 请求未执行: kind=%d, reason=%s\n", (int)d->operation.kind,
		started == SDO_START_BUSY ? "CHANNEL_BUSY" : "INVALID");
	d->sdo_unstarted = 1u;
	d->cw.pending_valid = 0u;
	return SM_HANDLED();
}

/**
 * dev_start_read - 状态入口发起读并处理未受理
 * @d: 设备对象
 * @index: 对象索引
 * @timeout_ms: 本笔事务的期限
 *
 * Return: 已受理则消费事件，未受理则留待下一拍取消流程。
 */
sm_ret_t dev_start_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms)
{
	enum sdo_start_result started = dev_read(d, index, timeout_ms);

	return (started == SDO_START_ACCEPTED) ? SM_HANDLED() : dev_not_started(d, started);
}

/**
 * dev_start_write - 状态入口发起写并处理未受理
 * @d: 设备对象
 * @index: 对象索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限
 *
 * Return: 已受理则消费事件，未受理则留待下一拍取消流程。
 */
sm_ret_t dev_start_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
			 uint32_t timeout_ms)
{
	enum sdo_start_result started = dev_write(d, index, value, size, timeout_ms);

	return (started == SDO_START_ACCEPTED) ? SM_HANDLED() : dev_not_started(d, started);
}

/**
 * dev_start_pulse - 状态入口发起一次性控制字并处理未受理
 * @d: 设备对象
 * @value: 控制字值
 * @timeout_ms: 本笔事务的期限
 *
 * Return: 已受理则消费事件，未受理则留待下一拍取消流程。
 */
sm_ret_t dev_start_pulse(struct cia402_device *d, uint16_t value, uint32_t timeout_ms)
{
	enum sdo_start_result started = dev_write_pulse(d, value, timeout_ms);

	return (started == SDO_START_ACCEPTED) ? SM_HANDLED() : dev_not_started(d, started);
}

/**
 * dev_done - 成功结束当前流程
 * @d: 设备对象
 */
void dev_done(struct cia402_device *d)
{
	d->outbox = CIA402_OUTCOME_DONE;
	d->operation.running = 0u;
	d->cw.pending_valid = 0u;
}

/**
 * dev_finish - 失败结束并保存诊断
 * @d: 设备对象
 * @fail: 执行失败诊断
 */
void dev_finish(struct cia402_device *d, enum cia402_fail fail)
{
	d->outbox = CIA402_OUTCOME_FAILED;
	d->failure.fail = fail;
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
		return;
	}
	d->failure.abort_code = result->abort_code;
	if (result->status == SDO_STATUS_TIMEOUT || result->abort_code == 0u) {
		/* 通道判定期限到，或没有 abort 码的失败：驱动器没按期应答 */
		fail = CIA402_FAIL_TIMEOUT;
	}
	dev_finish(d, fail);
}
