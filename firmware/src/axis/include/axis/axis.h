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

/* 一个轴 = 一个节点 + 该节点的 402 推进器。
 * 机械换算参数是编译期常量（config/cats_mech.h），不属于轴实例状态。 */
struct cats_axis {
	uint8_t node_id;
	struct cia402_node drv; /* 402 的知识全在里面，本层只问它结果 */
};

/* 业务单位：位置 mm，速度 mm·s⁻¹，加速度 mm·s⁻²（float） */

/* 实时反馈样本：由 cia402 的 50 ms 后台监控读数换算而来。
 * valid 的位见 cia402.h 的 CIA402_MON_VALID_*；位为 0 时对应值无意义。 */
struct axis_sample {
	float position_mm;
	float velocity_mm_s;
	uint16_t statusword;
	uint8_t valid;
};

/* 运动结论汇总（已从 402 取走，不会重复上报） */
enum axis_motion {
	AXIS_MOTION_NONE = 0, /* 没有运动在途 */
	AXIS_MOTION_BUSY,     /* 还在走 */
	AXIS_MOTION_DONE,     /* 到了 / 停稳了 */
	AXIS_MOTION_FAILED    /* 超时 / abort / 驱动器故障 */
};

void axis_init(struct cats_axis *a, struct co_master *m, uint8_t node_id);

/* 机械换算（每转脉冲数 / 每转毫米）见 config/cats_mech.h，编译期固定 */

/* 绝对位置运动（mm / mm·s⁻¹ / mm·s⁻²）。0 = 已受理；1 = 轴上有过程在跑；-1 = 不允许 */
int axis_move_to(struct cats_axis *a, float mm, float mm_s, float mm_s2);

/* 减速停在当前位置，保持使能。0 = 已受理 */
int axis_halt(struct cats_axis *a);

/* 取上一段动作的结论（取走后回 NONE） */
enum axis_motion axis_take_motion(struct cats_axis *a);

/* 本轴最近一次失败时驱动器回的 SDO abort；0 = 不是 abort 引起的 */
uint32_t axis_abort_code(const struct cats_axis *a);

/* 取实时反馈样本（不触发传输，只搬运 50 ms 监测到的值） */
void axis_sample(const struct cats_axis *a, struct axis_sample *out);

/* ---- 批量动作：对一组轴逐个施加（每轴有自己的 SDO 通道，互不阻塞） ---- */

/* 请求所有轴写参数（上电一次：模式等）：0 = 已受理；-1 = 没有轴或参数不对 */
int axis_config_all(struct cats_axis *axes, uint8_t count);

/* 请求所有轴使能：0 = 已受理；-1 = 没有轴或参数不对 */
int axis_enable_all(struct cats_axis *axes, uint8_t count);

/* 急停所有轴：quick stop → 断使能（由各自的异步推进器走完） */
void axis_quick_stop_all(struct cats_axis *axes, uint8_t count);

/* 每拍推进各轴 */
void axis_poll_all(struct cats_axis *axes, uint8_t count, uint32_t now_ms);

int axis_all_configured(const struct cats_axis *axes, uint8_t count);
int axis_all_enabled(const struct cats_axis *axes, uint8_t count);
int axis_any_failed(const struct cats_axis *axes, uint8_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AXIS_AXIS_H */
