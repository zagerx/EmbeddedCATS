/*
 * controller_actions.c — 状态图的时间基准与状态图装配
 *
 * 状态图直接使用主站的 NMT 与节点查询，本文件只提供统一的时钟源和装配入口。
 * 动作与协议栈同属 CANopen 工作线程，不需要待发槽或收发线程。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <time.h>

/**
 * controller_now_ms - 读取控制层当前时刻
 * @ctl: 控制上下文
 *
 * Return: 单调毫秒计数；未绑定主站时返回 0。
 */
uint32_t controller_now_ms(struct controller *ctl)
{
	struct timespec ts;

	if (ctl == NULL || ctl->master == NULL) {
		return 0u;
	}
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}

/**
 * controller_stm_init - 初始化状态图并复位其运行字段
 * @ctl: 已完成主站与轴装配的控制上下文
 *
 * 逐字段初始化，不清空整个控制器，以免破坏主站、轴和互斥锁。
 */
void controller_stm_init(struct controller *ctl)
{
	ctl->move = (struct controller_move_cmd){0};
	ctl->move_done = 0u;
	ctl->axis_done = 0u;
	ctl->estop_clearing = 0u;
	pthread_mutex_lock(&ctl->info_lock);
	ctl->info.fault_code = CONTROLLER_FAULT_NONE;
	pthread_mutex_unlock(&ctl->info_lock);
	controller_evtq_init(&ctl->evtq);

	/* 初始转移沿 ROOT 进入 PREPARING，并从其 ENTRY 发出通信复位。 */
	sm_hsm_init(&ctl->hsm, &initial);
	ctl->state_since_ms = controller_now_ms(ctl);
}
