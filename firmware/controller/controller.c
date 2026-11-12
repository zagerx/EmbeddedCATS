/*
 * controller.c — 装配控制层并提供 I/O、控制与服务推进入口
 *
 * 组合根持有主站、每轴对象访问通道、CiA 402 设备、轴和业务状态机。
 * 状态图动作集中在 controller_actions.c，指令与查询接口位于 control_api.c。
 * 本文件不创建线程，CAN 通道与节拍由宿主编排。
 *
 * Author: zager
 */

#include "controller_internal.h"

#include <master/master.h>

static struct controller controller_singleton;

/* 主站实例：生命周期与进程相同，控制器只存指针 */
static struct master master_instance;

/* 汇总所有已装配轴的心跳事实，供每次状态机推进共用 */
static uint8_t heartbeat_fact(const struct controller *ctl);

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
 * controller_init - 装配控制层并进入初始业务状态
 * @bus: 已打开的 CAN 通道，由宿主持有
 *
 * 只能在控制线程调用且只能成功初始化一次；主站在最终对象中原地初始化。
 *
 * Return: CONTROLLER_OK 成功；CONTROLLER_ERR_PARAM 参数无效；CONTROLLER_ERR_STATE 重复初始化。
 */
enum controller_result controller_init(struct can_io *bus)
{
	uint8_t i;

	if (bus == NULL) {
		return CONTROLLER_ERR_PARAM;
	}
	if (controller_singleton.master != NULL) {
		return CONTROLLER_ERR_STATE; /* 只初始化一次 */
	}

	controller_singleton.master = &master_instance;
	master_init(&master_instance, bus);
	(void)master_add_node(&master_instance, 1);
	(void)master_add_node(&master_instance, 2);

	/* 轴 = 主站上已登记的从站，站点号只在装配层声明一次，这里读回来 */
	controller_singleton.axis_count = master_node_count(&master_instance) < CONTROLLER_MAX_AXES
						  ? master_node_count(&master_instance)
						  : CONTROLLER_MAX_AXES;
	for (i = 0; i < controller_singleton.axis_count; i++) {
		uint8_t node_id = master_node_id(&master_instance, i);
		CO_SDOclient_t *sdo = master_sdo(&master_instance, node_id);

		/* 每轴一条异步 SDO 通道，由 I/O 与服务线程共同推进 */
		sdo_client_init(&controller_singleton.axes[i].sdo, sdo);
		cia402_init(&controller_singleton.axes[i].dev,
			    sdo_client_access(&controller_singleton.axes[i].sdo), node_id);
		axis_init(&controller_singleton.axes[i], node_id);
	}

	controller_stm_init(&controller_singleton);
	controller_publish();
	/* 线程由宿主起（见 control.h 的三个推进入口说明） */
	return CONTROLLER_OK;
}

/**
 * controller_poll - 推进一次业务状态机并发布快照
 *
 * 只能由控制线程调用；本函数不执行总线收发或阻塞式对象访问。
 *
 * Return: CONTROLLER_OK 推进成功；CONTROLLER_ERR_STATE 尚未初始化。
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

	/* 停止和急停插在队头，外部事件先于本拍节拍处理。 */
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

	/* 外部事件处理完后投递一次周期节拍。 */
	before = sm_hsm_state(&ctl->hsm);
	sm_hsm_dispatch(&ctl->hsm, &evt_tick);
	if (sm_hsm_state(&ctl->hsm) != before) {
		ctl->state_since_ms = now;
	}

	controller_publish();
	return CONTROLLER_OK;
}

/**
 * controller_io_poll - 推进一次总线收发与各轴 SDO 通道
 *
 * 只能由独占总线的 I/O 线程调用；收帧、事务推进和发帧的顺序固定。
 */
void controller_io_poll(void)
{
	struct controller *ctl = &controller_singleton;
	uint32_t now;
	uint8_t i;

	if (ctl->master == NULL) {
		return;
	}
	now = controller_now_ms(ctl);

	/* 收：应答落到各从站的事务槽里；搬：服务请求交给协议栈；
	 * 发：请求在当拍发出，不用多等一拍。 */
	master_recv_step(ctl->master, now);
	for (i = 0u; i < ctl->axis_count; i++) {
		sdo_client_io_poll(&ctl->axes[i].sdo, now);
	}
	master_send_step(ctl->master, now);
}

/**
 * controller_svc_poll - 推进一次各轴非阻塞 CiA 402 状态机
 *
 * 只能由服务线程调用；每次只推进到下一个异步事件。
 */
void controller_svc_poll(void)
{
	struct controller *ctl = &controller_singleton;
	uint32_t now;
	uint8_t i;

	if (ctl->master == NULL) {
		return;
	}
	now = controller_now_ms(ctl);
	for (i = 0u; i < ctl->axis_count; i++) {
		axis_poll(&ctl->axes[i], now);
	}
}

static struct controller controller_singleton = {.lock = PTHREAD_MUTEX_INITIALIZER};

static uint8_t heartbeat_fact(const struct controller *ctl)
{
	uint8_t ok = ctl->axis_count > 0 ? 1u : 0u;
	uint8_t i;

	/* 轴数为 0 说明没装配上：没有可确认的节点，不能报健康 */
	for (i = 0u; i < ctl->axis_count; i++) {
		if (master_node_alive(ctl->master, ctl->axes[i].node_id) != 0) {
			ok = 0u;
			break;
		}
	}
	return ok;
}
