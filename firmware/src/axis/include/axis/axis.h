/*
 * axis.h — 轴层：轴集合 + 业务语义
 *
 * 一个"轴"在本项目里就是协议上的一个节点（双轴驱动器 = 两个节点）。
 * 本层负责：
 *   - 把轴组织成集合，并决定**谁现在能用 SDO**（通道只有一条 ⇒ 依次使能）；
 *   - 业务单位与协议单位之间的换算（脉冲当量 6092h）——等做运动时再加。
 *
 * 本层不知道：控制字序列、状态位含义、SDO 通道（都在 cia402）；
 *             应用状态机 PREPARING / RUNNING（在 app）。
 */

#ifndef AXIS_AXIS_H
#define AXIS_AXIS_H

#include <cia402/cia402.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一个轴 = 一个节点 + 该节点的 402 推进器 */
struct cats_axis {
	uint8_t node_id;
	struct cia402_node drv; /* 402 的知识全在里面，本层只问它结果 */
};

/*
 * 轴集合。active 是"当前允许用 SDO 的轴下标"，之所以要有它：
 * SDO 客户端只有一个（OD_CNT_SDO_CLI = 1），两个轴不能同时传，所以
 * 前一个使能完成/失败之后才轮下一个。
 */
struct axis_group {
	struct cats_axis *axes;
	uint8_t count;
	uint8_t active; /* == count 表示没有轴在推进 */
};

void axis_init(struct cats_axis *a, struct co_master *m, uint8_t node_id);

void axis_group_init(struct axis_group *g, struct cats_axis *axes, uint8_t count);

/* 依次使能（从第 0 个轴开始）：0 = 已受理；-1 = 没有轴或参数不对 */
int axis_group_enable(struct axis_group *g);

/* 每拍推进当前轴；该轴有结论后自动轮到下一个 */
void axis_group_poll(struct axis_group *g, uint32_t now_ms);

int axis_group_all_enabled(const struct axis_group *g);
int axis_group_any_failed(const struct axis_group *g);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AXIS_AXIS_H */
