/*
 * axis.c — 轴集合的推进与"谁在用 SDO"的排队
 *
 * 只有一条 SDO 通道，所以这里用最简单的策略：一个一个来。
 * 前一个轴出了结论（使能成功或失败）才轮到下一个；失败也照样往下走，
 * 好让每个轴都拿到自己的结论，最后由 axis_group_any_failed() 一并上报。
 *
 * 排队放在这一层而不是 cia402：cia402 是"一个节点的推进器"，它不该知道
 * 还有别的节点；而"轴之间怎么排"是轴集合的业务决定。
 */

#include <axis/axis.h>

#include <stddef.h>

void axis_init(struct cats_axis *a, struct co_master *m, uint8_t node_id)
{
	a->node_id = node_id;
	cia402_init(&a->drv, m, node_id);
}

void axis_group_init(struct axis_group *g, struct cats_axis *axes, uint8_t count)
{
	g->axes = axes;
	g->count = count;
	g->active = count; /* 没有轴在推进 */
}

int axis_group_enable(struct axis_group *g)
{
	if (g == NULL || g->axes == NULL || g->count == 0U) {
		return -1;
	}
	g->active = 0;
	return cia402_request_enable(&g->axes[0].drv) < 0 ? -1 : 0;
}

void axis_group_poll(struct axis_group *g, uint32_t now_ms)
{
	struct cia402_node *drv;

	if (g == NULL || g->axes == NULL || g->active >= g->count) {
		return; /* 没有轴在推进 */
	}

	drv = &g->axes[g->active].drv;
	cia402_poll(drv, now_ms);

	if (!cia402_is_enabled(drv) && !cia402_failed(drv)) {
		return; /* 当前轴还没结论 */
	}
	g->active++;
	if (g->active < g->count) {
		(void)cia402_request_enable(&g->axes[g->active].drv);
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
