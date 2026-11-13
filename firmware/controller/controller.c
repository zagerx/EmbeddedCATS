/*
 * controller.c — 装配控制对象并编排 CANopen 事件循环
 *
 * 主站、每轴对象访问、CiA 402 和业务状态机集中在组合根。
 * 每轮依次接收 CAN、推进 SDO 与协议栈、推进设备与整机状态图。
 * 本文件不创建线程；宿主只运行一个 CANopen 工作线程。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <master/master.h>

#include <string.h>

static struct controller controller_singleton = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.evtq.lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct master master_instance = {
	.ep = {.epoll_fd = -1, .event_fd = -1, .timer_fd = -1},
};

/* 汇总所有已装配轴的心跳事实 */
static uint8_t heartbeat_fact(const struct controller *ctl);

/* 推进各轴非阻塞设备状态机 */
static void controller_axis_poll(struct controller *ctl);

/**
 * control_ctx - 获取控制层单例上下文
 *
 * Return: 全局控制上下文指针，进程生命周期内有效。
 */
struct controller *control_ctx(void)
{
	return &controller_singleton;
}

/**
 * controller_init - 在指定接口上装配控制层
 * @can_ifname: SocketCAN 接口名，例如 can0
 *
 * 只能由 CANopen 工作线程调用；失败时不留下已初始化的主站。
 *
 * Return: OK 成功；PARAM 参数无效；STATE 重复初始化或协议装配失败。
 */
enum controller_result controller_init(const char *can_ifname)
{
	struct controller *ctl = &controller_singleton;
	uint8_t i;

	if (can_ifname == NULL || can_ifname[0] == '\0') {
		return CONTROLLER_ERR_PARAM;
	}
	if (ctl->master != NULL) {
		return CONTROLLER_ERR_STATE;
	}
	if (master_init(&master_instance, can_ifname) != 0) {
		return CONTROLLER_ERR_STATE;
	}
	for (i = 0u; i < CONTROLLER_MAX_AXES; i++) {
		if (!master_add_node(&master_instance, (uint8_t)(i + 1u))) {
			master_deinit(&master_instance);
			return CONTROLLER_ERR_STATE;
		}
	}
	ctl->axis_count = master_node_count(&master_instance);
	for (i = 0u; i < ctl->axis_count; i++) {
		uint8_t node_id = master_node_id(&master_instance, i);
		CO_SDOclient_t *sdo = master_sdo(&master_instance, node_id);

		sdo_client_init(&ctl->axes[i].sdo, sdo);
		cia402_init(&ctl->axes[i].dev, sdo_client_access(&ctl->axes[i].sdo), node_id);
		axis_init(&ctl->axes[i], node_id);
	}
	ctl->master = &master_instance;
	controller_stm_init(ctl);
	controller_publish();
	return CONTROLLER_OK;
}

/**
 * controller_deinit - 释放控制层运行资源
 *
 * 宿主 join 工作线程后调用。静态队列和快照锁保留，可再次初始化。
 *
 * Return: 始终返回 CONTROLLER_OK，未初始化时也不执行任何操作。
 */
enum controller_result controller_deinit(void)
{
	struct controller *ctl = &controller_singleton;

	if (ctl->master == NULL) {
		return CONTROLLER_OK;
	}
	master_deinit(ctl->master);
	ctl->master = NULL;
	ctl->axis_count = 0u;
	memset(ctl->axes, 0, sizeof(ctl->axes));
	controller_evtq_clear(&ctl->evtq);
	pthread_mutex_lock(&ctl->lock);
	ctl->published_state = CONTROLLER_STATE_PREPARING;
	ctl->published_fault = CONTROLLER_FAULT_NONE;
	memset(ctl->published_axes, 0, sizeof(ctl->published_axes));
	memset(ctl->published_abort, 0, sizeof(ctl->published_abort));
	pthread_mutex_unlock(&ctl->lock);
	return CONTROLLER_OK;
}

/**
 * controller_event_poll - 等待并处理一轮完整事件
 *
 * CAN 帧先进入协议栈，SDO 使用真实时间差推进，再处理轴和整机。
 * 自身收到通信复位时重建协议服务，不重新分配已绑定的客户端对象。
 *
 * Return: OK 本轮完成；STATE 未初始化、请求退出或协议重建失败。
 */
enum controller_result controller_event_poll(void)
{
	struct controller *ctl = &controller_singleton;
	struct master *m = ctl->master;
	CO_NMT_reset_cmd_t reset;
	uint8_t i;

	if (m == NULL) {
		return CONTROLLER_ERR_STATE;
	}
	CO_epoll_wait(&m->ep);
	CO_epoll_processRT(&m->ep, m->co, false);
	for (i = 0u; i < ctl->axis_count; i++) {
		sdo_client_step(&ctl->axes[i].sdo, m->ep.timeDifference_us, &m->ep.timerNext_us);
	}
	CO_epoll_processMain(&m->ep, m->co, false, &reset);
	if (reset == CO_RESET_NOT) {
		controller_axis_poll(ctl);
		(void)controller_poll();
	} else if (reset != CO_RESET_COMM || master_reset_communication(m) != 0) {
		CO_epoll_processLast(&m->ep);
		return CONTROLLER_ERR_STATE;
	}
	CO_epoll_processLast(&m->ep);
	return CONTROLLER_OK;
}

/**
 * controller_poll - 推进一次业务状态机并发布快照
 *
 * 只能由 CANopen 工作线程调用；本函数不执行总线收发。
 *
 * Return: OK 推进成功；STATE 尚未初始化。
 */
enum controller_result controller_poll(void)
{
	static const struct sm_event evt_tick = {CONTROLLER_SIG_TICK};
	struct controller *ctl = control_ctx();
	struct controller_evt evt;
	sm_state_t before;
	uint32_t now;
	unsigned n;

	if (ctl->master == NULL) {
		return CONTROLLER_ERR_STATE;
	}
	now = controller_now_ms(ctl);
	ctl->hb_ok = heartbeat_fact(ctl);
	for (n = 0u; n < CONTROLLER_EVTQ_DRAIN_MAX; n++) {
		if (!controller_evtq_pop(&ctl->evtq, &evt)) {
			break;
		}
		before = sm_hsm_state(&ctl->hsm);
		sm_hsm_dispatch(&ctl->hsm, &evt.super);
		if (sm_hsm_state(&ctl->hsm) != before) {
			ctl->state_since_ms = now;
		}
	}
	before = sm_hsm_state(&ctl->hsm);
	sm_hsm_dispatch(&ctl->hsm, &evt_tick);
	if (sm_hsm_state(&ctl->hsm) != before) {
		ctl->state_since_ms = now;
	}
	controller_publish();
	return CONTROLLER_OK;
}

/**
 * controller_wakeup - 唤醒 CANopen 工作线程
 *
 * 任意线程可调用；未初始化或已经释放时不执行任何操作。
 */
void controller_wakeup(void)
{
	master_notify(controller_singleton.master);
}

static uint8_t heartbeat_fact(const struct controller *ctl)
{
	uint8_t ok = ctl->axis_count > 0 ? 1u : 0u;
	uint8_t i;

	for (i = 0u; i < ctl->axis_count; i++) {
		if (master_node_alive(ctl->master, ctl->axes[i].node_id) != 0) {
			ok = 0u;
			break;
		}
	}
	return ok;
}

static void controller_axis_poll(struct controller *ctl)
{
	uint32_t now = controller_now_ms(ctl);
	uint8_t i;

	for (i = 0u; i < ctl->axis_count; i++) {
		axis_poll(&ctl->axes[i], now);
	}
}
