/*
 * controller_ops.c — 为业务状态机适配主站能力并集中完成装配
 *
 * 回调把状态机的 NMT 与时钟需求映射到主站和总线。
 * 状态机只保存 stm_ops 接口，不认识 CANopenNode 或 SocketCAN。
 *
 * Author: zager
 */

#include "controller_internal.h"

/* 向状态机注入 NMT Start 请求 */
static void nmt_start_cb(void *ctx);

/* 向状态机注入 NMT Reset Communication 请求 */
static void nmt_reset_cb(void *ctx);

/* 向状态机注入总线侧单调毫秒时钟 */
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
 * controller_stm_init - 装配业务状态机及其主站、时钟能力
 * @ctl: 已完成轴装配的控制上下文
 *
 * 状态机只接收能力回调与当前控制上下文，不直接依赖主站实现。
 */
void controller_stm_init(struct controller *ctl)
{
	struct stm_ops ops = {
		.nmt_start = nmt_start_cb,
		.nmt_reset_comm = nmt_reset_cb,
		.now_ms = now_ms_cb,
		.ctx = ctl,
	};

	ctl->stm = stm_instance();
	stm_init(ctl->stm, ctl->axes, ctl->axis_count, &ops);
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
