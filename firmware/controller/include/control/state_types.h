/*
 * state_types.h — 业务层的公共枚举（不依赖任何协议层）
 *
 * 从 control.h 搬出来：状态与故障码属于"业务语义"，控制层只是转发布。
 */

#ifndef CONTROL_STATE_TYPES_H
#define CONTROL_STATE_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 轴索引：0..N-1，与 CANopen 节点号差 1（节点 1 → 索引 0）。
 * 轴与节点的对应关系固定在 config/axis_map.h，不随登记顺序变化。
 */
enum controller_axis {
	CONTROLLER_AXIS_1 = 0,
	CONTROLLER_AXIS_2,
	CONTROLLER_AXIS_COUNT /* 轴个数：数组长度与循环上界都用它 */
};

enum controller_result {
	CONTROLLER_OK = 0,
	CONTROLLER_ERR_STATE, /* 未初始化或重复初始化 */
	CONTROLLER_ERR_PARAM, /* 参数不合法：通道名为空 / 轴号越界 */
	CONTROLLER_ERR_BUSY   /* 事件队列已满：这条指令没进队，请稍后重试 */
};

/*
 * 业务运行状态（与 NMT 状态、402 状态三者不混）。
 * PREPARING → CONFIGURING → IDLE ──使能指令──> ENABLING → HOLDING ⇄ MOVING；
 * MOVING 可经 HALTING 回 HOLDING；HOLDING ──回零指令──> HOMING → HOLDING；
 * FAULT / ESTOP 为终态。HOMING 放在末尾：新增状态不挪动已有值。
 */
enum controller_state {
	CONTROLLER_STATE_PREPARING = 0,
	CONTROLLER_STATE_CONFIGURING,
	CONTROLLER_STATE_IDLE,
	CONTROLLER_STATE_ENABLING,
	CONTROLLER_STATE_HOLDING,
	CONTROLLER_STATE_MOVING,
	CONTROLLER_STATE_HALTING,
	CONTROLLER_STATE_FAULT,
	CONTROLLER_STATE_ESTOP,
	CONTROLLER_STATE_HOMING, /* 回原点：驱动器在跑 HM 流程，主站等结论 */
	CONTROLLER_STATE_COUNT   /* 状态个数，states[] 表的大小 */
};

/*
 * 故障原因：说明“为什么失败”。数值对外上报，禁止重排。
 * BIZ_TIMEOUT 与 DEV_TIMEOUT 是不同层的超时，不能合并：
 * 前者是状态机自己的计时，后者是单笔对象事务或步骤的期限。
 */
enum controller_fault_reason {
	CONTROLLER_FAULT_REASON_NONE = 0,
	CONTROLLER_FAULT_REASON_BIZ_TIMEOUT = 1, /* 状态机计时超时 */
	CONTROLLER_FAULT_REASON_DEV_TIMEOUT = 2, /* 事务或步骤超时 */
	CONTROLLER_FAULT_REASON_DRIVER = 3,      /* 驱动器进入故障态 */
	CONTROLLER_FAULT_REASON_SDO = 4,         /* 对象访问被 abort */
	CONTROLLER_FAULT_REASON_CHECK = 5,       /* 回读值不符 */
	CONTROLLER_FAULT_REASON_HOMING = 8,      /* 驱动器判回零错误（6041 bit13） */
	CONTROLLER_FAULT_REASON_COUNT
};

/* first_axis 取此值表示未定位到具体轴 */
#define CONTROLLER_FAULT_NO_AXIS UINT8_MAX

/*
 * 首个故障的快照：进入 FAULT 时确定，之后只有 axis_mask 继续累积。
 * abort_code 只在 reason == SDO 时是 abort 码，其余原因下无意义。
 */
struct controller_fault {
	enum controller_state stage;         /* 失败时所处状态 */
	enum controller_fault_reason reason; /* 失败原因 */
	uint8_t first_axis;                  /* 首个失败轴；NO_AXIS = 未定位 */
	uint8_t axis_mask;                   /* 全部失败轴位图 */
	uint32_t abort_code;                 /* SDO abort 码；0 = 没有 */
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CONTROL_STATE_TYPES_H */
