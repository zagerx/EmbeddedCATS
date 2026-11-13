/*
 * controller_actions.c — 状态图可用的动作与状态图装配
 *
 * 状态图只通过本文件广播 NMT 命令、读单调时钟。
 * 动作与协议栈同属 CANopen 工作线程，不需要待发槽或收发线程。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <master/master.h>

#include <time.h>

/**
 * controller_nmt_start - 广播 NMT Start
 * @ctl: 控制上下文；为空时什么也不做
 *
 * 只能由工作线程调用；发送结果由后续心跳与使能结果确认。
 */
void controller_nmt_start(struct controller *ctl)
{
	if (ctl == NULL || ctl->master == NULL) {
		return;
	}
	(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_START);
}

/**
 * controller_nmt_reset_comm - 广播 NMT Reset Communication
 * @ctl: 控制上下文；为空时什么也不做
 *
 * 只复位通信层，不改对象字典里的配置。
 */
void controller_nmt_reset_comm(struct controller *ctl)
{
	if (ctl == NULL || ctl->master == NULL) {
		return;
	}
	(void)master_request_nmt(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_RESET_COMM);
}

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
	ctl->hb_ok = 0u;
	ctl->move = (struct controller_move_cmd){0};
	ctl->move_done = 0u;
	ctl->fault_code = CONTROLLER_FAULT_NONE;
	controller_evtq_init(&ctl->evtq);

	/* 初始转移沿 ROOT 进入 PREPARING，并从其 ENTRY 发出通信复位。 */
	sm_hsm_init(&ctl->hsm, &initial);
	ctl->state_since_ms = controller_now_ms(ctl);
}
