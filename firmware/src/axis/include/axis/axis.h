/*
 * axis.h — 轴层：轴集合 + 业务语义
 *
 * 一个"轴"在本项目里就是协议上的一个节点（双轴驱动器 = 两个节点）。
 * 本层负责：
 *   - 把轴组织成集合，每拍让每个轴各自推进（每节点一条 SDO 通道，互不阻塞）；
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

/* 一个轴 = 一个节点 + 该节点的 402 推进器 + 业务↔协议单位换算 */
struct cats_axis {
	uint8_t node_id;
	struct cia402_node drv; /* 402 的知识全在里面，本层只问它结果 */

	/* 换算参数由装配层给定（机械参数不属于协议） */
	int32_t pulses_per_rev; /* 6092h：每转多少脉冲 */
	int32_t um_per_rev;     /* 传动：每转多少微米（丝杠导程） */
};

/* 业务单位：位置 µm，速度 µm·s⁻¹，加速度 µm·s⁻² */

/* 运动结论汇总（已从 402 取走，不会重复上报） */
enum axis_motion {
	AXIS_MOTION_NONE = 0, /* 没有运动在途 */
	AXIS_MOTION_BUSY,     /* 还在走 */
	AXIS_MOTION_DONE,     /* 到了 / 停稳了 */
	AXIS_MOTION_FAILED    /* 超时 / abort / 驱动器故障 */
};

/*
 * 轴集合。只是个容器：每轴有自己的 SDO 通道（OD 1280h/1281h 绑不同站号），
 * 所以没有"轮到谁"这种状态，谁失败也就只影响自己。
 */
struct axis_group {
	struct cats_axis *axes;
	uint8_t count;
};

void axis_init(struct cats_axis *a, struct co_master *m, uint8_t node_id);

/* 装配层给换算参数；未配置（任一为 0）时运动指令一律拒绝 */
void axis_set_scale(struct cats_axis *a, int32_t pulses_per_rev, int32_t um_per_rev);

void axis_group_init(struct axis_group *g, struct cats_axis *axes, uint8_t count);

/* 绝对位置运动（µm / µm·s⁻¹ / µm·s⁻²）。0 = 已受理；1 = 轴上有过程在跑；-1 = 不允许 */
int axis_move_to(struct cats_axis *a, int32_t um, int32_t um_s, int32_t um_s2);

/* 减速停在当前位置，保持使能。0 = 已受理 */
int axis_halt(struct cats_axis *a);

/* 取上一段动作的结论（取走后回 NONE） */
enum axis_motion axis_take_motion(struct cats_axis *a);

/* 请求所有轴使能：0 = 已受理；-1 = 没有轴或参数不对 */
int axis_group_enable(struct axis_group *g);

/* 急停所有轴：quick stop → 断使能（由各自的异步推进器走完） */
void axis_group_quick_stop(struct axis_group *g);

/* 每拍推进各轴（各轴互不阻塞） */
void axis_group_poll(struct axis_group *g, uint32_t now_ms);

int axis_group_all_enabled(const struct axis_group *g);
int axis_group_any_failed(const struct axis_group *g);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AXIS_AXIS_H */
