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

/* 事务没被受理：当场收尾，并回传要迁移到的状态 */
static sm_state_t dev_start_failed(struct cia402_device *d, enum sdo_start_result started);

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
	const struct sdo_request req = {
		.dir = SDO_DIR_READ,
		.index = index,
		.timeout_ms = timeout_ms,
	};
	enum sdo_start_result started;

	started = sdo_client_start(d->client, &req);
	if (started == SDO_START_ACCEPTED) {
		d->txn.index = index;
		d->txn.subindex = 0u;
		d->txn.dir = (uint8_t)SDO_DIR_READ;
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
	const struct sdo_request req = {
		.dir = SDO_DIR_WRITE,
		.index = index,
		.subindex = sub,
		.size = size,
		.value = value,
		.timeout_ms = timeout_ms,
	};
	enum sdo_start_result started;

	started = sdo_client_start(d->client, &req);
	if (started == SDO_START_ACCEPTED) {
		d->txn.index = index;
		d->txn.subindex = sub;
		d->txn.dir = (uint8_t)SDO_DIR_WRITE;
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
	const struct sdo_request req = {
		.dir = SDO_DIR_WRITE,
		.index = CIA402_OD_CONTROLWORD,
		.size = 2u,
		.value = (uint32_t)value,
		.timeout_ms = timeout_ms,
	};
	enum sdo_start_result started;

	started = sdo_client_start(d->client, &req);
	if (started == SDO_START_ACCEPTED) {
		d->txn.index = CIA402_OD_CONTROLWORD;
		d->txn.subindex = 0u;
		d->txn.dir = (uint8_t)SDO_DIR_WRITE;
	}
	return started;
}

/**
 * dev_start_read - 发起读事务
 * @d: 设备对象
 * @index: 对象索引
 * @timeout_ms: 本笔事务的期限
 *
 * Return: NULL 已受理，等结果事件；非 NULL 未受理，已当场收尾，返回要迁移到的状态。
 */
sm_state_t dev_start_read(struct cia402_device *d, uint16_t index, uint32_t timeout_ms)
{
	enum sdo_start_result started = dev_read(d, index, timeout_ms);

	return (started == SDO_START_ACCEPTED) ? NULL : dev_start_failed(d, started);
}

/**
 * dev_start_write - 发起写事务
 * @d: 设备对象
 * @index: 对象索引
 * @value: 待写值
 * @size: 字节数
 * @timeout_ms: 本笔事务的期限
 *
 * Return: NULL 已受理；非 NULL 未受理，已当场收尾。
 */
sm_state_t dev_start_write(struct cia402_device *d, uint16_t index, uint32_t value, uint8_t size,
			   uint32_t timeout_ms)
{
	enum sdo_start_result started = dev_write(d, index, value, size, timeout_ms);

	return (started == SDO_START_ACCEPTED) ? NULL : dev_start_failed(d, started);
}

/**
 * dev_start_pulse - 发起一次性控制字写
 * @d: 设备对象
 * @value: 控制字值
 * @timeout_ms: 本笔事务的期限
 *
 * Return: NULL 已受理；非 NULL 未受理，已当场收尾。
 */
sm_state_t dev_start_pulse(struct cia402_device *d, uint16_t value, uint32_t timeout_ms)
{
	enum sdo_start_result started = dev_write_pulse(d, value, timeout_ms);

	return (started == SDO_START_ACCEPTED) ? NULL : dev_start_failed(d, started);
}

/**
 * dev_done - 成功结束当前流程
 * @d: 设备对象
 */
void dev_done(struct cia402_device *d)
{
	d->outbox = CIA402_OP_RESULT_DONE;
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
	d->outbox = CIA402_OP_RESULT_FAILED;
	d->failure = fail;
	d->operation.running = 0u;
	d->cw.pending_valid = 0u;
}

/**
 * dev_fail - 已受理事务失败时的统一收尾
 * @d: 设备对象
 * @result: 失败的那笔结果（取 abort 码）
 *
 * 超时统一走取消路径在 st_idle 落账，这里只区分“对端拒绝”与“无原因失败”。
 */
void dev_fail(struct cia402_device *d, const struct sdo_result *result)
{
	if (result == NULL) {
		return;
	}
	if (result->abort_code != 0u) {
		/* abort 码是 CiA 301 的协议知识，只在本层用掉，不往上层传 */
		fprintf(stderr, "[cia402] %s %04Xh:%02X 被对端拒绝 abort=0x%08X\n",
			d->txn.dir == SDO_DIR_WRITE ? "写" : "读", d->txn.index,
			(unsigned)d->txn.subindex, (unsigned)result->abort_code);
	} else {
		fprintf(stderr, "[cia402] %s %04Xh:%02X 失败，无 abort 码\n",
			d->txn.dir == SDO_DIR_WRITE ? "写" : "读", d->txn.index,
			(unsigned)d->txn.subindex);
	}
	dev_finish(d, CIA402_FAIL_SDO);
}

/* 没发起就没有结果事件，不能留在流程里等；这里只收尾，迁移由调用方执行 */
static sm_state_t dev_start_failed(struct cia402_device *d, enum sdo_start_result started)
{
	fprintf(stderr, "[cia402] 事务未发起: kind=%d, reason=%s\n", (int)d->operation.kind,
		started == SDO_START_BUSY ? "CHANNEL_BUSY" : "INVALID");
	dev_finish(d, CIA402_FAIL_NOT_STARTED);
	return &st_idle;
}
