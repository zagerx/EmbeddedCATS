/*
 * 轴层：一个轴对应一个节点，负责 mm ↔ pulse 换算和动作请求。
 * 402 流程由 cia402 实现，业务状态由 states 管理。
 */

#ifndef AXIS_AXIS_H
#define AXIS_AXIS_H

#include <cia402/device.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一个轴 = 一个节点 + 该节点的驱动对象。
 * 机械换算参数是编译期常量（config/cats_mech.h），不属于轴实例状态。 */
struct cats_axis {
	uint8_t node_id;
	struct cia402_device dev;     /* 协议知识全在里面，本层只问它结果 */
	struct cia402_status status;  /* 上一拍流程状态，本层缓存 */
	struct cia402_feedback feedback; /* 最新原始反馈，本层缓存 */
};

/* 业务单位：位置 mm，速度 mm·s⁻¹，加速度 mm·s⁻²（float） */

/* 反馈由 cia402 设备层读取；未读到过的字段为 0。 */
struct axis_sample {
	float position_mm;
	float velocity_mm_s;
	uint16_t statusword;
	uint16_t error_code; /* 603F：驱动器自己报的错误码（不是主站的判断） */
};

/* 运动结论汇总（已从 402 取走，不会重复上报） */
enum axis_motion {
	AXIS_MOTION_NONE = 0, /* 没有运动在途 */
	AXIS_MOTION_BUSY,     /* 还在走 */
	AXIS_MOTION_DONE,     /* 到了 / 停稳了 */
	AXIS_MOTION_FAILED    /* 超时 / abort / 驱动器故障 */
};

/* 驱动对象由组合根装配（cia402_init）后再交给轴层；轴层只记站号 */
void axis_init(struct cats_axis *a, uint8_t node_id);

/* 推进一次设备状态机并刷新本层缓存的事实；仅服务线程调用 */
void axis_poll(struct cats_axis *a, uint32_t now_ms);

/* 机械换算（每转脉冲数 / 每转毫米）见 config/cats_mech.h，编译期固定 */

/* 绝对位置运动（mm / mm·s⁻¹ / mm·s⁻²）。0 = 已受理；1 = 轴上有过程在跑；-1 = 不允许 */
int axis_move_to(struct cats_axis *a, float mm, float mm_s, float mm_s2);

/* 减速停在当前位置，保持使能。0 = 已受理 */
int axis_halt(struct cats_axis *a);

/* 取上一段动作的结论（取走后回 NONE） */
enum axis_motion axis_take_motion(struct cats_axis *a);

/* 本轴最近一次失败时驱动器回的 SDO abort；0 = 不是 abort 引起的 */
uint32_t axis_abort_code(const struct cats_axis *a);

/* 读取最近一次反馈快照，不触发总线传输。 */
void axis_sample(const struct cats_axis *a, struct axis_sample *out);

/* ---- 批量动作 ---- */

/* 请求所有轴写参数（上电一次：模式等）：0 = 已受理；-1 = 没有轴或参数不对 */
int axis_config_all(struct cats_axis *axes, uint8_t count);

/* 请求所有轴使能：0 = 已受理；-1 = 没有轴或参数不对 */
int axis_enable_all(struct cats_axis *axes, uint8_t count);

/* 急停所有轴：quick stop → 断使能（由各自的异步推进器走完） */
void axis_quick_stop_all(struct cats_axis *axes, uint8_t count);

int axis_all_configured(const struct cats_axis *axes, uint8_t count);
int axis_all_enabled(const struct cats_axis *axes, uint8_t count);
int axis_any_failed(const struct cats_axis *axes, uint8_t count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AXIS_AXIS_H */
