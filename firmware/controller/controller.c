/*
 * controller.c — 组合根：把各层装配起来，并提供三个"推进一步"的入口
 *
 * 只做两件事：
 *   1. 装配：主站 → 每轴对象访问通道 → 驱动 → 轴 → 业务状态机（唯一装配点）；
 *   2. 推进：io_poll（收/搬/发）、svc_poll（跑同步流程）。
 *
 * 状态机相关的对外 API（指令、状态、故障、样本）在 control_api.c。
 * **本文件不创建线程**，节拍由宿主编排。
 */

#include "controller_internal.h"

static struct cats_control s_ctl = {.lock = PTHREAD_MUTEX_INITIALIZER};

struct cats_control *control_ctx(void)
{
	return &s_ctl;
}

/* ---- 注入给状态机的能力：它不认识 master，也不自己取时钟 ---- */

static void nmt_start_cb(void *ctx)
{
	struct cats_control *ctl = ctx;

	(void)master_set_state(ctl->master, CO_NMT_ALL_NODES, CO_NMT_START);
}

static void nmt_reset_cb(void *ctx)
{
	struct cats_control *ctl = ctx;

	(void)master_set_state(ctl->master, CO_NMT_ALL_NODES, CO_NMT_RESET_COMMUNICATION);
}

static uint32_t now_ms_cb(void *ctx)
{
	struct cats_control *ctl = ctx;
	struct can_io *bus = master_bus(ctl->master);

	return bus != NULL ? can_io_now_ms(bus) : 0u;
}

uint32_t now_ms(struct cats_control *ctl)
{
	return now_ms_cb(ctl);
}

/* ---- 装配 ---- */

enum cats_result cats_control_init(struct master *master)
{
	uint8_t i;

	if (master == NULL) {
		return CATS_ERR_PARAM;
	}
	if (s_ctl.master != NULL) {
		return CATS_ERR_STATE; /* 只初始化一次 */
	}

	s_ctl.master = master;
	/* 轴 = 主站上已登记的从站，站点号只在装配层声明一次，这里读回来 */
	s_ctl.axis_count = master_node_count(master) < CATS_MAX_AXES ? master_node_count(master)
								     : CATS_MAX_AXES;
	for (i = 0; i < s_ctl.axis_count; i++) {
		uint8_t node_id = master_node_id(master, i);
		struct co_sdo *sdo = master_sdo(master, node_id);

		/* 每轴一条 SDO 通道：I/O 线程执行，服务线程阻塞等待 */
		od_sdo_init(&s_ctl.sdo[i], sdo);
		s_ctl.od[i] = od_sdo_iface(&s_ctl.sdo[i]);
		drive_init(&s_ctl.axes[i].drv, sdo, &s_ctl.od[i], node_id);
		axis_init(&s_ctl.axes[i], node_id);
	}

	/* 初态：initial() 返回 SM_TRAN(&st_preparing)，内核把 sm_top → ROOT → PREPARING
	 * 的 ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	{
		struct stm_ops ops;

		ops.nmt_start = nmt_start_cb;
		ops.nmt_reset_comm = nmt_reset_cb;
		ops.now_ms = now_ms_cb;
		ops.ctx = &s_ctl;
		s_ctl.stm = stm_instance();
		stm_init(s_ctl.stm, s_ctl.axes, s_ctl.axis_count, &ops);
	}

	cats_control_publish();
	/* 线程由宿主起（见 control.h 的三个推进入口说明） */
	return CATS_OK;
}

/* ---- 推进：本层只提供"推进一步"，线程与节拍归宿主 ---- */

void cats_control_io_poll(void)
{
	struct cats_control *ctl = &s_ctl;
	uint32_t now;
	uint8_t i;

	if (ctl->master == NULL) {
		return;
	}
	now = now_ms(ctl);

	/* 一拍三件事，顺序有意义：
	 *   ① 收：应答落到各从站的事务槽里
	 *   ② 搬：服务线程的请求提交给协议 / 结果搬回 od_sdo
	 *   ③ 发：该发的帧当拍发出去（放在 ② 之后，请求不用多等一拍） */
	master_recv_step(ctl->master, now);
	for (i = 0u; i < ctl->axis_count; i++) {
		od_sdo_io_step(&ctl->sdo[i]);
	}
	master_send_step(ctl->master, now);
}

void cats_control_svc_poll(void)
{
	struct cats_control *ctl = &s_ctl;
	uint32_t now;
	uint8_t i;

	if (ctl->master == NULL) {
		return;
	}
	now = now_ms(ctl);
	for (i = 0u; i < ctl->axis_count; i++) {
		drive_service(&ctl->axes[i].drv, now);
	}
}
