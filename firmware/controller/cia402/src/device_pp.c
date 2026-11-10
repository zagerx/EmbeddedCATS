/*
 * device_pp.c — 按状态执行 CiA 402 PP 绝对位置运动
 *
 * 每笔对象事务完成后才进入下一状态；到位等待由后续服务节拍推进。
 * 同一服务线程可以公平地轮转多个设备实例。
 *
 * Author: zager
 */

#include "device_internal.h"

/**
 * cia402_device_pp_begin - 启动 PP 参数写入状态链
 * @d: 设备对象，目标和轮廓参数已在请求槽中备好
 */
void cia402_device_pp_begin(struct cia402_device *d)
{
	cia402_device_start_write(d, CIA402_ST_PP_WRITE_VELOCITY, CIA402_OD_PROFILE_VELOCITY,
				  (uint32_t)d->vel, 4u, CIA402_DEVICE_SDO_TIMEOUT_MS);
}

/**
 * cia402_device_pp_complete - 处理一次 PP 对象事务
 * @d: 设备对象
 * @result: 已完成的结果，读事务携带最新值
 */
void cia402_device_pp_complete(struct cia402_device *d, const struct sdo_result *result)
{
	switch (d->state) {
	case CIA402_ST_PP_WRITE_VELOCITY:
		cia402_device_start_write(d, CIA402_ST_PP_WRITE_ACCEL, CIA402_OD_PROFILE_ACCEL,
					  (uint32_t)d->acc, 4u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_PP_WRITE_ACCEL:
		cia402_device_start_write(d, CIA402_ST_PP_WRITE_DECEL, CIA402_OD_PROFILE_DECEL,
					  (uint32_t)d->dec, 4u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_PP_WRITE_DECEL:
		d->pp_tgt = d->tgt;
		cia402_device_start_write(d, CIA402_ST_PP_WRITE_TARGET, CIA402_OD_TARGET_POSITION,
					  (uint32_t)d->tgt, 4u, CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_PP_WRITE_TARGET:
		cia402_device_start_write(d, CIA402_ST_PP_TRIGGER_HIGH, CIA402_OD_CONTROLWORD,
					  d->controlword | CIA402_CW_NEW_SETPOINT, 2u,
					  CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_PP_TRIGGER_HIGH:
		cia402_device_start_write(d, CIA402_ST_PP_TRIGGER_LOW, CIA402_OD_CONTROLWORD,
					  d->controlword & (uint16_t)~CIA402_CW_NEW_SETPOINT, 2u,
					  CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_PP_TRIGGER_LOW:
		d->pp_seen_low = 0u;
		d->pp_deadline = d->now_ms + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
		cia402_device_start_read(d, CIA402_ST_PP_READ_STATUS, CIA402_OD_STATUSWORD,
					 CIA402_DEVICE_SDO_TIMEOUT_MS);
		break;
	case CIA402_ST_PP_READ_STATUS:
		pthread_mutex_lock(&d->lock);
		d->statusword = (uint16_t)result->value;
		pthread_mutex_unlock(&d->lock);
		if (cia402_parse((uint16_t)result->value) == CIA402_FAULT ||
		    (int32_t)(d->now_ms - d->pp_deadline) >= 0) {
			cia402_device_finish(d, -1, 0u);
		} else if ((result->value & CIA402_SW_TARGET_REACHED) == 0u) {
			d->pp_seen_low = 1u;
			cia402_device_start_read(d, CIA402_ST_PP_READ_STATUS, CIA402_OD_STATUSWORD,
						 CIA402_DEVICE_SDO_TIMEOUT_MS);
		} else if (d->pp_seen_low != 0u) {
			cia402_device_finish(d, 0, 0u);
		} else {
			cia402_device_start_read(d, CIA402_ST_PP_READ_POSITION,
						 CIA402_OD_POSITION_ACTUAL, CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		break;
	case CIA402_ST_PP_READ_POSITION:
		pthread_mutex_lock(&d->lock);
		d->position = (int32_t)result->value;
		pthread_mutex_unlock(&d->lock);
		if ((int32_t)result->value == d->pp_tgt) {
			cia402_device_finish(d, 0, 0u);
		} else if ((int32_t)(d->now_ms - d->pp_deadline) >= 0) {
			cia402_device_finish(d, -1, 0u);
		} else {
			cia402_device_start_read(d, CIA402_ST_PP_READ_STATUS, CIA402_OD_STATUSWORD,
						 CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		break;
	default:
		cia402_device_finish(d, -1, 0u);
		break;
	}
}
