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

#include <axis_map.h>
#include <master/master.h>
#include <pdo/tpdo.h>

#include <string.h>

static struct controller controller_singleton = {
	.evtq.lock = PTHREAD_MUTEX_INITIALIZER,
	.info_lock = PTHREAD_MUTEX_INITIALIZER,
};

static struct master master_instance = {
	.ep = {.epoll_fd = -1, .event_fd = -1, .timer_fd = -1},
};

/* 推进一次业务状态机 */
static enum controller_result controller_poll(uint32_t now_ms);

/* 把每轴的两路 TPDO 接收挂到当前协议栈上 */
static int controller_tpdo_attach(struct controller *ctl);

/* 装配失败：释放主站并恢复空状态 */
static enum controller_result controller_init_fail(struct controller *ctl);

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
	ctl->master = &master_instance;

	/* 按映射表登记节点：表顺序即登记顺序，登记结果必须与表一致 */
	ctl->axis_count = AXIS_MAP_COUNT;
	for (i = 0u; i < ctl->axis_count; i++) {
		const struct axis_map_entry *b = &AXIS_MAP[i];
		CO_SDOclient_t *sdo;

		if (b->axis != (enum controller_axis)i) {
			return controller_init_fail(ctl); /* 表顺序与轴号不一致 */
		}
		if (!master_add_node(&master_instance, b->node_id) ||
		    master_node_id(&master_instance, i) != b->node_id) {
			return controller_init_fail(ctl); /* 登记失败或顺序被改 */
		}
		sdo = master_sdo(&master_instance, b->node_id);
		if (sdo == NULL) {
			return controller_init_fail(ctl);
		}
		sdo_client_init(&ctl->axes[i].sdo, sdo);
		cia402_init(&ctl->axes[i].dev, &ctl->axes[i].sdo);
	}
	if (controller_tpdo_attach(ctl) != 0) {
		return controller_init_fail(ctl); /* 占不到接收槽：不可能有反馈 */
	}
	controller_feedback_reset();
	/* 反馈轮询关闭期间对外只报 0：这里清一次，之后没有写入方 */
	pthread_mutex_lock(&ctl->info_lock);
	memset(ctl->info.feedback, 0, sizeof(ctl->info.feedback));
	pthread_mutex_unlock(&ctl->info_lock);
	controller_stm_init(ctl);
	controller_info_update(ctl);
	return CONTROLLER_OK;
}

/**
 * controller_deinit - 释放控制层运行资源
 *
 * 宿主 join 工作线程后调用。静态队列和信息锁保留，可再次初始化。
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
	pthread_mutex_lock(&ctl->info_lock);
	ctl->info.state = CONTROLLER_STATE_PREPARING;
	memset(&ctl->info.fault, 0, sizeof(ctl->info.fault));
	ctl->info.fault.first_axis = CONTROLLER_FAULT_NO_AXIS;
	memset(ctl->info.feedback, 0, sizeof(ctl->info.feedback));
	memset(ctl->info.abort, 0, sizeof(ctl->info.abort));
	pthread_mutex_unlock(&ctl->info_lock);
	return CONTROLLER_OK;
}

/**
 * controller_event_poll - 等待并处理一轮完整事件
 *
 * CAN 帧先进入协议栈，SDO 使用真实时间差推进，再处理轴和整机。
 * 本机站号也在这条总线上，广播的复位命令会被自己收到：无论通信复位还是
 * 复位节点，都只重建本机通信服务，不重新分配已绑定的客户端对象。
 *
 * Return: OK 本轮完成；STATE 未初始化、请求退出或协议重建失败。
 */
enum controller_result controller_event_poll(void)
{
	struct controller *ctl = &controller_singleton;
	struct master *m = ctl->master;
	CO_NMT_reset_cmd_t reset;
	uint32_t now;
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
		/* 本轮只取一次时钟：反馈、设备状态机与业务状态机用同一时刻 */
		now = controller_now_ms(ctl);
		/* 先收反馈结果，再让 402 发起下一笔，避免结果滞留被当成忙 */
		controller_feedback_poll(ctl, now);
		controller_axes_poll(ctl->axes, ctl->axis_count, now);
		(void)controller_poll(now);
		controller_info_update(ctl);
	} else if ((reset != CO_RESET_COMM && reset != CO_RESET_APP) ||
		   master_reset_communication(m) != 0 || controller_tpdo_attach(ctl) != 0) {
		CO_epoll_processLast(&m->ep);
		return CONTROLLER_ERR_STATE;
	}
	CO_epoll_processLast(&m->ep);
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

static int controller_tpdo_attach(struct controller *ctl)
{
	uint8_t i;

	for (i = 0u; i < ctl->axis_count; i++) {
		const uint8_t node_id = master_node_id(ctl->master, i);

		if (tpdo_attach(&ctl->axes[i].tpdo, ctl->master, node_id) != 0) {
			return -1;
		}
	}
	return 0;
}

static enum controller_result controller_init_fail(struct controller *ctl)
{
	master_deinit(ctl->master);
	ctl->master = NULL;
	ctl->axis_count = 0u;
	return CONTROLLER_ERR_STATE;
}

static enum controller_result controller_poll(uint32_t now_ms)
{
	static const struct sm_event evt_tick = {CONTROLLER_SIG_TICK};
	struct controller *ctl = control_ctx();
	struct controller_evt evt;
	sm_state_t before;
	unsigned n;

	if (ctl->master == NULL) {
		return CONTROLLER_ERR_STATE;
	}
	for (n = 0u; n < CONTROLLER_EVTQ_DRAIN_MAX; n++) {
		if (!controller_evtq_pop(&ctl->evtq, &evt)) {
			break;
		}
		before = sm_hsm_state(&ctl->hsm);
		sm_hsm_dispatch(&ctl->hsm, &evt.super);
		if (sm_hsm_state(&ctl->hsm) != before) {
			ctl->state_since_ms = now_ms;
		}
	}
	before = sm_hsm_state(&ctl->hsm);
	sm_hsm_dispatch(&ctl->hsm, &evt_tick);
	if (sm_hsm_state(&ctl->hsm) != before) {
		ctl->state_since_ms = now_ms;
	}
	return CONTROLLER_OK;
}