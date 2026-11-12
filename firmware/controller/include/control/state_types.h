/*
 * state_types.h — 业务层的公共枚举（不依赖任何协议层）
 *
 * 从 control.h 搬出来：状态与故障码属于"业务语义"，控制层只是转发布。
 */

#ifndef CONTROL_STATE_TYPES_H
#define CONTROL_STATE_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

/* 整机最多几条轴（= 主站最多登记几个节点中参与控制的） */
#define CATS_MAX_AXES 2

enum cats_result {
	CATS_OK = 0,
	CATS_ERR_STATE, /* 未初始化或重复初始化 */
	CATS_ERR_PARAM, /* 参数不合法：通道名为空 / 轴号越界 */
	CATS_ERR_BUSY   /* 事件队列已满：这条指令没进队，请稍后重试 */
};

/* 业务运行状态（与 NMT 状态、402 状态三者不混） */
enum cats_state {
	CATS_STATE_PREPARING = 0,
	CATS_STATE_CONFIGURING,
	CATS_STATE_ENABLING,
	CATS_STATE_HOLDING,
	CATS_STATE_MOVING,
	CATS_STATE_HALTING,
	CATS_STATE_FAULT,
	CATS_STATE_ESTOP,
	CATS_STATE_COUNT /* 状态个数，states[] 表的大小 */
};

/* 故障码：进 FAULT 时确定，之后不再变 */
enum cats_fault {
	CATS_FAULT_NONE = 0,
	CATS_FAULT_BOOTUP_TIMEOUT, /* PREPARING 里静默期过后仍等不到心跳 */
	CATS_FAULT_HEARTBEAT_LOST, /* 运行期节点心跳超时 */
	CATS_FAULT_CONFIG_TIMEOUT, /* CONFIGURING 里超时仍未配完 */
	CATS_FAULT_CONFIG_FAILED,  /* CONFIGURING 里写参数被 abort 或读回值不符 */
	CATS_FAULT_ENABLE_TIMEOUT, /* ENABLING 里超时还没使能上 */
	CATS_FAULT_ENABLE_FAILED,  /* ENABLING 里驱动器回了 abort 或 402 进故障 */
	CATS_FAULT_MOTION_FAILED,  /* MOVING 里 abort / 超时 / 402 进故障 */
	CATS_FAULT_STOP_FAILED     /* HALTING 里停不下来（超时或 abort） */
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONTROL_STATE_TYPES_H */
