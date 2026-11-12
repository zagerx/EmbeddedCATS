/*
 * statemachine.c — 初始化业务状态机并提供时钟与事实查询
 *
 * 控制层直接分发事件并推进 HSM，事件队列由控制层管理。
 * 本层只读取轴事实并下发轴请求，不接触 CANopen 或传输协议。
 *
 * Author: zager
 */

#include "statemachine_internal.h"

#include "states/states.h"

#include <stddef.h>
#include <string.h>

/**
 * stm_now_ms - 读取注入给状态机的当前时刻
 * @a: 状态机对象；NULL 安全
 *
 * Return: 注入时钟的毫秒计数；对象或时钟回调为空时返回 0。
 */
uint32_t stm_now_ms(struct controller_sm *a)
{
	return (a != NULL && a->ops.now_ms != NULL) ? a->ops.now_ms(a->ops.ctx) : 0u;
}

/**
 * stm_init - 装配并初始化业务状态机
 * @a: 状态机对象；NULL 时直接返回
 * @axes: 轴数组，由组合根持有
 * @axis_count: 轴数量
 * @ops: NMT 与时钟回调；NULL 时保留零初始化回调
 */
void stm_init(struct controller_sm *a, struct cats_axis *axes, uint8_t axis_count,
	      const struct stm_ops *ops)
{
	if (a == NULL) {
		return;
	}
	memset(a, 0, sizeof(*a));
	a->axes = axes;
	a->axis_count = axis_count;
	if (ops != NULL) {
		a->ops = *ops;
	}
	cats_evtq_init(&a->evtq);
	/* initial() 返回 SM_TRAN(&st_preparing)：内核把 top → root → preparing 的
	 * ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	sm_hsm_init(&a->hsm, &initial);
	a->state_since_ms = stm_now_ms(a);
}

/**
 * stm_state - 将当前 HSM 叶子状态映射为业务状态
 * @a: 状态机对象；NULL 安全
 *
 * Return: 当前业务状态；对象为空或状态未注册时返回 PREPARING。
 */
enum cats_state stm_state(const struct controller_sm *a)
{
	int i;

	if (a == NULL) {
		return CATS_STATE_PREPARING;
	}
	for (i = 0; i < CATS_STATE_COUNT; i++) {
		if (states[i] == sm_hsm_state(&a->hsm)) {
			return (enum cats_state)i;
		}
	}
	return CATS_STATE_PREPARING;
}

/**
 * stm_fault - 读取当前业务故障码
 * @a: 状态机对象；NULL 安全
 *
 * Return: 当前故障码；对象为空时返回 CATS_FAULT_NONE。
 */
enum cats_fault stm_fault(const struct controller_sm *a)
{
	return (a == NULL) ? CATS_FAULT_NONE : a->fault_code;
}
