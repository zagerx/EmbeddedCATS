/*
 * controller.c — 装配控制层并提供 I/O 与服务推进入口
 *
 * 组合根连接主站、每轴对象访问通道、CiA 402 设备、轴和业务状态机。
 * 状态机指令与查询接口位于 control_api.c；本文件不创建线程，节拍由宿主编排。
 *
 * Author: zager
 */

#include "controller_internal.h"

static struct controller cats_controller;

/* 向状态机注入 NMT Start 请求 */
static void nmt_start_cb(void *ctx);

/* 向状态机注入 NMT Reset Communication 请求 */
static void nmt_reset_cb(void *ctx);

/* 向状态机注入总线侧单调毫秒时钟 */
static uint32_t now_ms_cb(void *ctx);

/**
 * control_ctx - 获取控制层单例上下文
 *
 * Return: 全局控制上下文指针，进程生命周期内有效。
 */
struct controller *control_ctx(void)
{
	return &cats_controller;
}

/**
 * now_ms - 读取控制层当前时刻
 * @ctl: 控制上下文
 *
 * Return: 总线侧单调毫秒计数；未绑定总线时返回 0。
 */
uint32_t now_ms(struct controller *ctl)
{
	return now_ms_cb(ctl);
}

/**
 * controller_init - 装配控制层并进入初始业务状态
 * @master: 已绑定总线并登记节点的主站，由调用方持有
 *
 * 只能在控制线程调用且只能成功初始化一次；不创建工作线程。
 *
 * Return: CATS_OK 成功；CATS_ERR_PARAM 参数无效；CATS_ERR_STATE 重复初始化。
 */
enum cats_result controller_init(struct master *master)
{
	uint8_t i;

	if (master == NULL) {
		return CATS_ERR_PARAM;
	}
	if (cats_controller.master != NULL) {
		return CATS_ERR_STATE; /* 只初始化一次 */
	}

	cats_controller.master = master;
	/* 轴 = 主站上已登记的从站，站点号只在装配层声明一次，这里读回来 */
	cats_controller.axis_count = master_node_count(master) < CATS_MAX_AXES
					     ? master_node_count(master)
					     : CATS_MAX_AXES;
	for (i = 0; i < cats_controller.axis_count; i++) {
		uint8_t node_id = master_node_id(master, i);
		CO_SDOclient_t *sdo = master_sdo(master, node_id);

		/* 每轴一条 SDO 通道：I/O 线程执行，服务线程阻塞等待 */
		sdo_client_init(&cats_controller.sdo[i], sdo);
		cia402_init(&cats_controller.axes[i].dev,
			    sdo_client_access(&cats_controller.sdo[i]), node_id);
		axis_init(&cats_controller.axes[i], node_id);
	}

	/* 初态：initial() 返回 SM_TRAN(&st_preparing)，内核把 sm_top → ROOT → PREPARING
	 * 的 ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	{
		struct stm_ops ops;

		ops.nmt_start = nmt_start_cb;
		ops.nmt_reset_comm = nmt_reset_cb;
		ops.now_ms = now_ms_cb;
		ops.ctx = &cats_controller;
		cats_controller.stm = stm_instance();
		stm_init(cats_controller.stm, cats_controller.axes, cats_controller.axis_count,
			 &ops);
	}

	controller_publish();
	/* 线程由宿主起（见 control.h 的三个推进入口说明） */
	return CATS_OK;
}

/**
 * controller_io_poll - 推进一次总线收发与各轴 SDO 通道
 *
 * 只能由独占总线的 I/O 线程调用；收帧、事务推进和发帧的顺序固定。
 */
void controller_io_poll(void)
{
	struct controller *ctl = &cats_controller;
	uint32_t now;
	uint8_t i;

	if (ctl->master == NULL) {
		return;
	}
	now = now_ms(ctl);

	/* 收：应答落到各从站的事务槽里；搬：服务请求交给协议栈；
	 * 发：请求在当拍发出，不用多等一拍。 */
	master_recv_step(ctl->master, now);
	for (i = 0u; i < ctl->axis_count; i++) {
		sdo_client_io_poll(&ctl->sdo[i], now);
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
	struct controller *ctl = &cats_controller;
	uint32_t now;
	uint8_t i;

	if (ctl->master == NULL) {
		return;
	}
	now = now_ms(ctl);
	for (i = 0u; i < ctl->axis_count; i++) {
		axis_poll(&ctl->axes[i], now);
	}
}

static void nmt_start_cb(void *ctx)
{
	struct controller *ctl = ctx;

	(void)master_set_state(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_START);
}

static void nmt_reset_cb(void *ctx)
{
	struct controller *ctl = ctx;

	(void)master_set_state(ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_RESET_COMM);
}

static uint32_t now_ms_cb(void *ctx)
{
	struct controller *ctl = ctx;
	struct can_io *bus = master_bus(ctl->master);

	return bus != NULL ? can_io_now_ms(bus) : 0u;
}

static struct controller cats_controller = {.lock = PTHREAD_MUTEX_INITIALIZER};
