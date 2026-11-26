/*
 * controller_fault.c — 锁存真实执行故障与多轴失败集合
 *
 * Author: zager
 */

#include "controller_internal.h"
#include "controller_log.h"

/**
 * controller_set_fault - 锁存首个故障并累积失败轴
 * @ctl: 控制上下文；NULL 安全
 * @stage: 失败时所处状态
 * @reason: 失败原因；NONE 表示不是故障，直接返回
 * @axis: 失败轴号；超出已装配范围时记作未定位
 */
void controller_set_fault(struct controller *ctl, enum controller_state stage,
			  enum controller_fault_reason reason, uint8_t axis)
{
	if (ctl == NULL || reason == CONTROLLER_FAULT_REASON_NONE) {
		return;
	}
	controller_log_fault(stage, reason, axis);
	pthread_mutex_lock(&ctl->info_lock);
	if (ctl->info.fault.reason == CONTROLLER_FAULT_REASON_NONE) {
		ctl->info.fault.stage = stage;
		ctl->info.fault.reason = reason;
		ctl->info.fault.first_axis =
			(axis < CONTROLLER_AXIS_COUNT) ? axis : CONTROLLER_FAULT_NO_AXIS;
	}
	if (axis < CONTROLLER_AXIS_COUNT) {
		ctl->info.fault.axis_mask |= (uint8_t)(1u << axis);
	}
	pthread_mutex_unlock(&ctl->info_lock);
}

/**
 * controller_set_axes_fault - 保存集合动作中所有真正失败的轴
 * @ctl: 控制上下文；NULL 安全
 * @stage: 失败时业务状态
 * @failure: 本拍收集到的完整失败集合；NULL 安全
 */
void controller_set_axes_fault(struct controller *ctl, enum controller_state stage,
			       const struct controller_axes_failure *failure)
{
	enum controller_fault_reason reason;
	uint8_t i;

	if (ctl == NULL || failure == NULL || failure->mask == 0u) {
		return;
	}
	reason = controller_fault_reason_of(failure->first.fail);
	controller_log_fault(stage, reason, failure->first_axis);
	pthread_mutex_lock(&ctl->info_lock);
	if (ctl->info.fault.reason == CONTROLLER_FAULT_REASON_NONE) {
		ctl->info.fault.stage = stage;
		ctl->info.fault.reason = reason;
		ctl->info.fault.first_axis = failure->first_axis;
	}
	for (i = 0u; i < CONTROLLER_AXIS_COUNT; i++) {
		if ((failure->mask & (uint8_t)(1u << i)) != 0u) {
			ctl->info.fault.axis_mask |= (uint8_t)(1u << i);
		}
	}
	pthread_mutex_unlock(&ctl->info_lock);
}

/**
 * controller_fault_reason_of - 映射设备层失败为业务故障原因
 * @fail: cia402 回执里的失败归类
 *
 * Return: 对应的业务故障原因。
 */
enum controller_fault_reason controller_fault_reason_of(enum cia402_fail fail)
{
	switch (fail) {
	case CIA402_FAIL_TIMEOUT:
		return CONTROLLER_FAULT_REASON_DEV_TIMEOUT;
	case CIA402_FAIL_DRIVER:
		return CONTROLLER_FAULT_REASON_DRIVER;
	case CIA402_FAIL_HOMING:
		return CONTROLLER_FAULT_REASON_HOMING;
	case CIA402_FAIL_SDO:
		return CONTROLLER_FAULT_REASON_SDO;
	case CIA402_FAIL_CHECK:
		return CONTROLLER_FAULT_REASON_CHECK;
	default:
		return CONTROLLER_FAULT_REASON_DEV_TIMEOUT;
	}
}
