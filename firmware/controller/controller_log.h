/*
 * controller_log.h — 控制层请求与故障日志
 *
 * Author: zager
 */

#ifndef CONTROLLER_LOG_H
#define CONTROLLER_LOG_H

#include "controller_internal.h"

/* 未执行的原因只用于日志与提交结果，不属于故障分类 */
enum controller_not_executed {
	CONTROLLER_NOT_EXECUTED_QUEUE_FULL,
	CONTROLLER_NOT_EXECUTED_INVALID_ARGUMENT,
	CONTROLLER_NOT_EXECUTED_INVALID_STATE,
	CONTROLLER_NOT_EXECUTED_DEVICE_BUSY,
	CONTROLLER_NOT_EXECUTED_NOT_INITIALIZED,
	CONTROLLER_NOT_EXECUTED_TERMINAL,
	CONTROLLER_NOT_EXECUTED_ALREADY_HOLDING,
};

void controller_log_not_executed(struct controller *ctl, enum controller_sig sig, uint8_t axis,
				 enum controller_not_executed reason);
void controller_log_queue_cleared(struct controller *ctl);
uint8_t controller_log_event_axis(const struct controller_evt *evt, uint8_t fallback);
void controller_log_fault(enum controller_state stage, enum controller_fault_reason reason,
			  uint8_t axis, uint32_t abort_code);

#endif /* CONTROLLER_LOG_H */
