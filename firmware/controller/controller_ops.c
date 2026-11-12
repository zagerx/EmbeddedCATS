/*
 * controller_ops.c — 为状态图适配主站能力并集中完成初始化
 *
 * 回调把控制器的 NMT 与时钟需求映射到主站和总线。
 * 状态图只使用 stm_ops 接口，不认识 CANopenNode 或 SocketCAN。
 *
 * Author: zager
 */

#include "controller_internal.h"

/* 向状态图注入 NMT Start 请求 */
static void nmt_start_cb(void *ctx);

/* 向状态图注入 NMT Reset Communication 请求 */
static void nmt_reset_cb(void *ctx);

/* 向状态图注入总线侧单调毫秒时钟 */
static uint32_t now_ms_cb(void *ctx);

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
 * controller_stm_init - 初始化状态图并注入主站、时钟能力
 * @ctl: 已完成主站与轴装配的控制上下文
 *
 * 逐字段初始化，不清空整个控制器，以免破坏主站、轴和互斥锁。
 */
void controller_stm_init(struct controller *ctl)
{
	ctl->ops = (struct stm_ops){
		.nmt_start = nmt_start_cb,
		.nmt_reset_comm = nmt_reset_cb,
		.now_ms = now_ms_cb,
		.ctx = ctl,
	};
	ctl->hb_ok = 0u;
	ctl->move = (struct cats_move_cmd){0};
	ctl->move_done = 0u;
	ctl->fault_code = CATS_FAULT_NONE;
	cats_evtq_init(&ctl->evtq);

	/* 初始转移沿 ROOT 进入 PREPARING，并从其 ENTRY 发出通信复位。 */
	sm_hsm_init(&ctl->hsm, &initial);
	ctl->state_since_ms = now_ms(ctl);
}

static void nmt_start_cb(void *ctx)
{
	struct controller *ctl = ctx;

	(void)master_set_state(&ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_START);
}

static void nmt_reset_cb(void *ctx)
{
	struct controller *ctl = ctx;

	(void)master_set_state(&ctl->master, MASTER_NMT_ALL_NODES, MASTER_NMT_RESET_COMM);
}

static uint32_t now_ms_cb(void *ctx)
{
	struct controller *ctl = ctx;
	struct can_io *bus = master_bus(&ctl->master);

	return bus != NULL ? can_io_now_ms(bus) : 0u;
}
