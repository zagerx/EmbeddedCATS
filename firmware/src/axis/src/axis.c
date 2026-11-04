/*
 * axis.c — 轴集合：把每拍推进分发给各轴
 *
 * 一个轴 = 一个节点，每个节点有一条自己的 SDO 通道（OD 的 1280h/1281h 各绑
 * 一个站号），所以轴与轴之间不需要排队：同时发起、各走各的，谁先出结论谁
 * 先上报，一个轴失败不影响另一个轴。
 *
 * 因此这里没有仲裁状态（没有"轮到谁"），失败由 axis_group_any_failed()
 * 一并上报给 app。
 */

#include <axis/axis.h>

#include <stddef.h>

void axis_init(struct cats_axis *a, struct co_master *m, uint8_t node_id)
{
	a->node_id = node_id;
	a->pulses_per_rev = 0;
	a->um_per_rev = 0;
	cia402_init(&a->drv, m, node_id);
}

void axis_set_scale(struct cats_axis *a, int32_t pulses_per_rev, int32_t um_per_rev)
{
	if (a == NULL) {
		return;
	}
	a->pulses_per_rev = pulses_per_rev;
	a->um_per_rev = um_per_rev;
}

/* µm → pulse（中间用 64 位，避免 1 m × 10000 脉冲就溢出） */
static int32_t scale_um(const struct cats_axis *a, int32_t um)
{
	int64_t p = (int64_t)um * (int64_t)a->pulses_per_rev / (int64_t)a->um_per_rev;

	if (p > INT32_MAX) {
		return INT32_MAX;
	}
	if (p < -INT32_MAX) {
		return -INT32_MAX;
	}
	return (int32_t)p;
}

int axis_move_to(struct cats_axis *a, int32_t um, int32_t um_s, int32_t um_s2)
{
	if (a == NULL || a->pulses_per_rev <= 0 || a->um_per_rev <= 0) {
		return -1; /* 没配机械参数就不接指令：宁可不动，也不算错位置 */
	}
	return cia402_pp_start(&a->drv, scale_um(a, um), scale_um(a, um_s), scale_um(a, um_s2),
			       scale_um(a, um_s2));
}

int axis_halt(struct cats_axis *a)
{
	return (a == NULL) ? -1 : cia402_halt(&a->drv);
}

enum axis_motion axis_take_motion(struct cats_axis *a)
{
	enum cia402_result r;

	if (a == NULL) {
		return AXIS_MOTION_NONE;
	}
	r = cia402_take_result(&a->drv);
	if (r == CIA402_RESULT_DONE) {
		return AXIS_MOTION_DONE;
	}
	if (r == CIA402_RESULT_FAILED) {
		return AXIS_MOTION_FAILED;
	}
	return cia402_busy(&a->drv) ? AXIS_MOTION_BUSY : AXIS_MOTION_NONE;
}

void axis_group_init(struct axis_group *g, struct cats_axis *axes, uint8_t count)
{
	g->axes = axes;
	g->count = count;
}

int axis_group_enable(struct axis_group *g)
{
	uint8_t i;

	if (g == NULL || g->axes == NULL || g->count == 0U) {
		return -1;
	}
	for (i = 0; i < g->count; i++) {
		/* 1 = 已经要求过（不重复发起），不算错 */
		(void)cia402_request_enable(&g->axes[i].drv);
	}
	return 0;
}

void axis_group_poll(struct axis_group *g, uint32_t now_ms)
{
	uint8_t i;

	if (g == NULL || g->axes == NULL) {
		return;
	}
	for (i = 0; i < g->count; i++) {
		cia402_poll(&g->axes[i].drv, now_ms);
	}
}

void axis_group_quick_stop(struct axis_group *g)
{
	uint8_t i;

	if (g == NULL || g->axes == NULL) {
		return;
	}
	for (i = 0; i < g->count; i++) {
		(void)cia402_quick_stop(&g->axes[i].drv);
	}
}

int axis_group_all_enabled(const struct axis_group *g)
{
	uint8_t i;

	if (g == NULL || g->axes == NULL || g->count == 0U) {
		return 0;
	}
	for (i = 0; i < g->count; i++) {
		if (!cia402_is_enabled(&g->axes[i].drv)) {
			return 0;
		}
	}
	return 1;
}

int axis_group_any_failed(const struct axis_group *g)
{
	uint8_t i;

	if (g == NULL || g->axes == NULL) {
		return 0;
	}
	for (i = 0; i < g->count; i++) {
		if (cia402_failed(&g->axes[i].drv)) {
			return 1;
		}
	}
	return 0;
}
