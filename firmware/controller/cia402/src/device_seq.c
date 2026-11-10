/*
 * device_seq.c — 推进配置、PDS 使能、断使能和停止状态
 *
 * 每个状态对应一笔异步对象事务，状态字响应决定下一条控制字。
 * 业务等待期限与单次 SDO 超时分别计算。
 *
 * Author: zager
 */

#include "device_internal.h"

static void enable_route(struct cia402_device *d, enum cia402_pds_state pds);
static void cache_statusword(struct cia402_device *d, uint16_t statusword);
static void enable_wait(struct cia402_device *d, uint16_t statusword,
			enum cia402_pds_state expected, enum cia402_state next,
			enum cia402_state read_state);

/**
 * cia402_device_seq_begin - 启动一个非 PP 流程
 * @d: 设备对象
 * @req: 已受理的请求类型
 */
void cia402_device_seq_begin(struct cia402_device *d, enum cia402_device_req req)
{
	switch (req) {
	case CIA402_DEVICE_REQ_CONFIG:
		cia402_device_start_write(d, CIA402_ST_CONFIG_WRITE_MODE, CIA402_OD_MODE,
					  CIA402_MODE_PP, 1u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_DEVICE_REQ_ENABLE:
		cia402_device_start_read(d, CIA402_ST_ENABLE_READ_PDS, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_DEVICE_REQ_DISABLE:
		cia402_device_start_write(d, CIA402_ST_DISABLE_WRITE, CIA402_OD_CONTROLWORD,
					  CIA402_CW_DISABLE, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_DEVICE_REQ_HALT:
		cia402_device_start_write(d, CIA402_ST_HALT_WRITE, CIA402_OD_CONTROLWORD,
					  d->controlword | CIA402_CW_HALT, 2u,
					  CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_DEVICE_REQ_QUICK_STOP:
		cia402_device_start_write(d, CIA402_ST_QUICK_STOP_WRITE, CIA402_OD_CONTROLWORD,
					  CIA402_CW_QUICK_STOP, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	default:
		cia402_device_finish(d, -1, 0u);
		break;
	}
}

/**
 * cia402_device_seq_complete - 处理一次成功的对象事务
 * @d: 设备对象
 * @result: 已完成的结果，读事务携带最新值
 */
void cia402_device_seq_complete(struct cia402_device *d, const struct sdo_result *result)
{
	switch (d->state) {
	case CIA402_ST_CONFIG_WRITE_MODE:
		cia402_device_start_read(d, CIA402_ST_CONFIG_READ_MODE, CIA402_OD_MODE_ACTUAL,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_CONFIG_READ_MODE:
		cia402_device_finish(d, (result->value & 0xFFu) == CIA402_MODE_PP ? 0 : -1, 0u);
		break;
	case CIA402_ST_ENABLE_READ_PDS:
		enable_route(d, cia402_parse((uint16_t)result->value));
		break;
	case CIA402_ST_ENABLE_READ_RESET: {
		enum cia402_pds_state pds = cia402_parse((uint16_t)result->value);

		cache_statusword(d, (uint16_t)result->value);
		if (pds != CIA402_FAULT) {
			enable_route(d, pds);
		} else if ((int32_t)(d->now_ms - d->wait_deadline) >= 0) {
			cia402_device_finish(d, -1, 0u);
		} else {
			cia402_device_start_read(d, CIA402_ST_ENABLE_READ_RESET,
						 CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		break;
	}
	case CIA402_ST_ENABLE_RESET_LOW:
		cia402_device_start_write(d, CIA402_ST_ENABLE_RESET_HIGH, CIA402_OD_CONTROLWORD,
					  CIA402_CW_FAULT_RESET, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_ENABLE_RESET_HIGH:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS * 2u;
		cia402_device_start_read(d, CIA402_ST_ENABLE_READ_RESET, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_ENABLE_WRITE_SHUTDOWN:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_ENABLE_READ_READY, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_ENABLE_READ_READY:
		enable_wait(d, (uint16_t)result->value, CIA402_READY,
			    CIA402_ST_ENABLE_WRITE_SWITCH_ON, CIA402_ST_ENABLE_READ_READY);
		break;
	case CIA402_ST_ENABLE_WRITE_SWITCH_ON:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_ENABLE_READ_SWITCHED_ON,
					 CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_ENABLE_READ_SWITCHED_ON:
		enable_wait(d, (uint16_t)result->value, CIA402_SWITCHED_ON,
			    CIA402_ST_ENABLE_WRITE_ENABLE_OP, CIA402_ST_ENABLE_READ_SWITCHED_ON);
		break;
	case CIA402_ST_ENABLE_WRITE_ENABLE_OP:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_ENABLE_READ_ENABLED, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_ENABLE_READ_ENABLED:
		enable_wait(d, (uint16_t)result->value, CIA402_OP_ENABLED,
			    CIA402_ST_IDLE, CIA402_ST_ENABLE_READ_ENABLED);
		break;
	case CIA402_ST_DISABLE_WRITE:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_STEP_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_DISABLE_READ, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_DISABLE_READ: {
		enum cia402_pds_state pds = cia402_parse((uint16_t)result->value);

		if (pds == CIA402_SWITCH_ON_DISABLED) {
			cia402_device_finish(d, 0, 0u);
		} else if ((int32_t)(d->now_ms - d->wait_deadline) >= 0) {
			cia402_device_finish(d, -1, 0u);
		} else {
			cia402_device_start_read(d, CIA402_ST_DISABLE_READ, CIA402_OD_STATUSWORD,
						 CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		break;
	}
	case CIA402_ST_HALT_WRITE:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_HALT_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_HALT_READ, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_HALT_READ:
		cache_statusword(d, (uint16_t)result->value);
		if ((result->value & CIA402_SW_TARGET_REACHED) != 0u) {
			cia402_device_start_write(d, CIA402_ST_HALT_CLEAR, CIA402_OD_CONTROLWORD,
						  d->controlword & (uint16_t)~CIA402_CW_HALT, 2u,
						  CIA402_DEVICE_SDO_TIMEOUT_MS);
		} else if (cia402_parse((uint16_t)result->value) == CIA402_FAULT ||
			   (int32_t)(d->now_ms - d->wait_deadline) >= 0) {
			cia402_device_finish(d, -1, 0u);
		} else {
			cia402_device_start_read(d, CIA402_ST_HALT_READ, CIA402_OD_STATUSWORD,
						 CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		break;
	case CIA402_ST_HALT_CLEAR:
		cia402_device_finish(d, 0, 0u);
		break;
	case CIA402_ST_QUICK_STOP_WRITE:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_QUICK_STOP_READ, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_QUICK_STOP_READ: {
		enum cia402_pds_state pds = cia402_parse((uint16_t)result->value);

		cache_statusword(d, (uint16_t)result->value);
		if (pds == CIA402_SWITCH_ON_DISABLED || pds == CIA402_NOT_READY ||
		    (int32_t)(d->now_ms - d->wait_deadline) >= 0) {
			cia402_device_start_write(d, CIA402_ST_QUICK_STOP_DISABLE,
						  CIA402_OD_CONTROLWORD, CIA402_CW_DISABLE, 2u,
						  CIA402_DEVICE_SDO_TIMEOUT_MS);
		} else {
			cia402_device_start_read(d, CIA402_ST_QUICK_STOP_READ,
						 CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		break;
	}
	case CIA402_ST_QUICK_STOP_DISABLE:
		cia402_device_finish(d, 0, 0u);
		break;
	default:
		cia402_device_finish(d, -1, 0u);
		break;
	}
}

/**
 * cia402_device_seq_on_failure - 处理需要继续收尾的停止失败
 * @d: 设备对象
 *
 * Quick Stop 写失败仍读状态，状态读取失败仍尝试断使能。
 *
 * Return: 1 已发起后续动作；0 当前流程应直接失败。
 */
int cia402_device_seq_on_failure(struct cia402_device *d)
{
	switch (d->state) {
	case CIA402_ST_QUICK_STOP_WRITE:
		d->wait_deadline = d->now_ms + CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_QUICK_STOP_READ, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		return 1;
	case CIA402_ST_QUICK_STOP_READ:
		cia402_device_start_write(d, CIA402_ST_QUICK_STOP_DISABLE, CIA402_OD_CONTROLWORD,
					  CIA402_CW_DISABLE, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		return 1;
	default:
		return 0;
	}
}

static void enable_route(struct cia402_device *d, enum cia402_pds_state pds)
{
	switch (pds) {
	case CIA402_FAULT:
		cia402_device_start_write(d, CIA402_ST_ENABLE_RESET_LOW, CIA402_OD_CONTROLWORD,
					  CIA402_CW_DISABLE, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_NOT_READY:
	case CIA402_SWITCH_ON_DISABLED:
		cia402_device_start_write(d, CIA402_ST_ENABLE_WRITE_SHUTDOWN, CIA402_OD_CONTROLWORD,
					  CIA402_CW_SHUTDOWN, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_READY:
		cia402_device_start_write(d, CIA402_ST_ENABLE_WRITE_SWITCH_ON, CIA402_OD_CONTROLWORD,
					  CIA402_CW_SWITCH_ON, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_SWITCHED_ON:
		cia402_device_start_write(d, CIA402_ST_ENABLE_WRITE_ENABLE_OP, CIA402_OD_CONTROLWORD,
					  CIA402_CW_ENABLE_OP, 2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_OP_ENABLED:
		cia402_device_finish(d, 0, 0u);
		break;
	default:
		cia402_device_finish(d, -1, 0u);
		break;
	}
}

static void enable_wait(struct cia402_device *d, uint16_t statusword,
			enum cia402_pds_state expected, enum cia402_state next,
			enum cia402_state read_state)
{
	enum cia402_pds_state pds = cia402_parse(statusword);

	cache_statusword(d, statusword);
	if (pds == expected) {
		if (next == CIA402_ST_IDLE) {
			cia402_device_finish(d, 0, 0u);
		} else if (next == CIA402_ST_ENABLE_WRITE_SWITCH_ON) {
			cia402_device_start_write(d, next, CIA402_OD_CONTROLWORD, CIA402_CW_SWITCH_ON,
						  2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		} else {
			cia402_device_start_write(d, next, CIA402_OD_CONTROLWORD, CIA402_CW_ENABLE_OP,
						  2u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
	} else if (pds == CIA402_FAULT) {
		enable_route(d, pds);
	} else if ((int32_t)(d->now_ms - d->wait_deadline) >= 0) {
		cia402_device_finish(d, -1, 0u);
	} else {
		cia402_device_start_read(d, read_state, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
}

static void cache_statusword(struct cia402_device *d, uint16_t statusword)
{
	pthread_mutex_lock(&d->lock);
	d->statusword = statusword;
	pthread_mutex_unlock(&d->lock);
}
