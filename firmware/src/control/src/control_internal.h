/*
 * control_internal.h — 控制层内部对象与共享声明（**不对外发布**）
 *
 * 供 control.c 与各 states/state_*.c 共享：
 *   struct cats_control 整机控制主体（状态机 + 主站指针 + 轴 + 事件队列）
 *   enum cats_sig       业务事件（内核只认 EMPTY/ENTRY/EXIT/INIT，业务信号从 SM_SIG_USER 起编号）
 *   states[]           状态函数表（states/states.c 定义）
 *   now_ms()           取当前时刻的小工具
 */

#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <control/control.h>

#include <axis/axis.h>

#include <pthread.h>

#include "hsm/sm_hsm.h"
#include "evt_queue/evt_queue.h"

#define CATS_MAX_AXES 2

/* 业务事件：内核只认 EMPTY/ENTRY/EXIT/INIT，业务信号从 SM_SIG_USER 起编号 */
enum cats_sig {
	CATS_SIG_TICK = SM_SIG_USER, /* 周期节拍，cats_control_poll() 每拍自己投一个 */
	CATS_SIG_MOVE,               /* 位置指令（载荷见 cats_evt.u.move） */
	CATS_SIG_STOP,               /* 停止：减速停（6040 bit8） */
	CATS_SIG_ESTOP               /* 急停：quick stop → 断使能 */
};

/* 位置指令的参数（业务单位：mm / mm·s⁻¹ / mm·s⁻²）；定义在 evt_queue.h */

/*
 * 字段顺序刻意分三段：装配期只读 → 控制线程私有 → 跨线程共享（放最后）。
 * 阅读时看到最后一段就知道"要加锁的只有这几个"，不用把整个结构扫一遍。
 */
struct cats_control {
	/* ---- 装配期写入，之后所有线程只读（不需要锁） ---- */
	struct co_master *master; /* 内核层主站，只借用；不归本层拥有 */
	struct cats_axis axes[CATS_MAX_AXES];
	uint8_t axis_count; /* 实际装配了几条轴（= 主站登记数，上限 MAX_AXES） */

	/* ---- 控制线程私有（只有 cats_control_poll 这条链路会读写） ---- */
	uint8_t hb_ok; /* 本拍所有轴心跳都正常；每拍刷新，状态只读不算 */
	struct cats_move_cmd move; /* 从指令槽取走后的参数副本 */
	struct sm_hsm hsm; /* 纯状态机机制（当前状态、暂存字段、重入保护） */
	uint32_t state_since_ms; /* 进入当前状态的时刻；cats_control_poll 每拍刷新 */
	enum cats_fault fault_code; /* 出错原因；非 NONE 时由 ROOT 统一迁 FAULT */

	/* ---- 跨线程共享 ----
	 * 写者：任意线程（事件入队，evtq 自带锁）
	 * 读者：控制线程（取事件）/ 任意线程（读发布值，用 lock 保护）
	 * 临界区里只有几条赋值，不包含任何 I/O、状态机动作或循环。 */
	pthread_mutex_t lock;
	struct cats_evtq evtq; /* 输入事件队列（自带锁，多生产者 / 单消费者） */
	int published_state;   /* enum cats_state，每拍发布一次 */
	int published_fault;   /* enum cats_fault，与状态同拍发布 */
};

/* states.c 定义的状态表：下标 = enum cats_state，值 = 状态函数 */
extern const sm_state_t states[CATS_STATE_COUNT];

/* 取当前时刻；未接管主站时返回 0 */
uint32_t now_ms(struct cats_control *app);

#endif /* CONTROL_INTERNAL_H */
