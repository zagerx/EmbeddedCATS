/*
 * 轴层：一个轴对应一个节点，负责下行命令的 mm → pulse 换算和动作请求。
 * 402 流程由 cia402 实现，业务状态由 states 管理。
 */

#ifndef CONTROLLER_AXIS_AXIS_H
#define CONTROLLER_AXIS_AXIS_H

#include <cia402/device_instance.h>
#include <pdo/tpdo.h>
#include <transport/sdo_client.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一个轴 = 一个节点 + 该节点的驱动对象、独享 SDO 通道与 TPDO 反馈。
 * 机械换算参数是编译期常量（config/cats_mech.h），不属于轴实例状态。 */
struct axis {
	struct cia402_device dev; /* 协议知识全在里面，本层只问它结果 */
	struct sdo_client sdo;    /* 本轴独享的异步对象访问通道 */
	struct tpdo_node tpdo;    /* 本轴的周期反馈通道 */
};

/* 业务单位：位置 mm，速度 mm·s⁻¹，加速度 mm·s⁻²（float） */

/* 运动结论汇总（已从 402 取走，不会重复上报）；NONE = 还没结论，还在跑或未发起 */
enum axis_motion {
	AXIS_MOTION_NONE = 0,
	AXIS_MOTION_DONE,  /* 到了 / 停稳了 */
	AXIS_MOTION_FAILED /* 超时 / abort / 驱动器故障 */
};

/* 一次失败的主站归类，来自 cia402 回执；仅 AXIS_MOTION_FAILED 时有意义 */
struct axis_failure {
	enum cia402_fail fail; /* 超时 / 驱动器故障 / abort / 回读不符 */
	uint32_t abort_code;   /* 仅 fail == CIA402_FAIL_SDO 时有效；0 = 没有 */
};

/* 推进一次设备状态机；仅 CANopen 工作线程调用 */
void axis_poll(struct axis *a, uint32_t now_ms);

/* 机械换算（每转脉冲数 / 每转毫米）见 config/cats_mech.h，编译期固定 */

/* 绝对位置运动（mm / mm·s⁻¹ / mm·s⁻²）。0 = 已受理；1 = 轴上有过程在跑；-1 = 不允许 */
int axis_move_to(struct axis *a, float mm, float mm_s, float mm_s2);

/* 减速停在当前位置，保持使能。0 = 已受理 */
int axis_halt(struct axis *a);

/* 回原点（原点开关法，方法号与速度见 cats_mech.h）。0 = 已受理；1 = 设备忙；-1 = 不允许 */
int axis_home(struct axis *a);

/* 取上一段动作的结论（取走后回 NONE）；failure 可为 NULL，失败时带回原因 */
enum axis_motion axis_take_motion(struct axis *a, struct axis_failure *failure);

/* 本轴最近一次失败时驱动器回的 SDO abort；0 = 不是 abort 引起的 */
uint32_t axis_abort_code(const struct axis *a);

/* 写运动模式（PP）：已配过的轴会被设备层拒绝，不算错 */
int axis_config_movemode(struct axis *a);

/* 请求使能：已使能时会被内部拒绝，不算错 */
int axis_enable(struct axis *a);

/* quick stop → 只写一次控制字，驱动器自行停机 */
int axis_quick_stop(struct axis *a);

/* 解除急停：让驱动器从 quick stop 状态回到可操作 */
int axis_clear_estop(struct axis *a);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONTROLLER_AXIS_AXIS_H */
