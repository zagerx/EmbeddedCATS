/*
 * root_internal.h — 组合根私有上下文（**不对外发布**）
 *
 * 组合根的私有对象：
 *   struct cats_control 主站 + 每轴驱动 + 每轴对象访问通道 + 业务状态机 + 发布值
 *   now_ms()            取当前时刻（I/O 与服务线程用）
 *
 * 业务状态机在 statemachine/；通用状态机内核在 hsm/。
 */

#ifndef CONTROL_INTERNAL_H
#define CONTROL_INTERNAL_H

#include <control/control.h>

#include <statemachine/statemachine.h>
#include <axis/axis.h>
#include <drive/drive.h>
#include <drive/od_access.h>
#include <drive/od_sdo.h>

#include <pthread.h>

#include <statemachine/state_types.h>

/* 位置指令的参数（业务单位：mm / mm·s⁻¹ / mm·s⁻²）；定义在 evt_queue.h */

/*
 * 字段顺序刻意分三段：装配期只读 → 控制线程私有 → 跨线程共享（放最后）。
 * 阅读时看到最后一段就知道"要加锁的只有这几个"，不用把整个结构扫一遍。
 */
struct cats_control {
	/* ---- 装配期写入，之后所有线程只读（不需要锁） ---- */
	struct co_master *master; /* 内核层主站，只借用；不归本层拥有 */
	struct cats_axis axes[CATS_MAX_AXES];
	struct od_sdo sdo[CATS_MAX_AXES]; /* 每轴一条 SDO 通道（服务线程 ↔ I/O 线程） */
	struct od_access od[CATS_MAX_AXES]; /* 每轴的 od_access 实体（drive 只存指针） */
	uint8_t axis_count; /* 实际装配了几条轴（= 主站登记数，上限 MAX_AXES） */

	/* ---- 业务状态机（内部对象，只由控制线程调用） ---- */
	struct stm *stm; /* 业务状态机（不透明句柄） */

	/* ---- 跨线程共享 ----
	 * 写者：任意线程（事件入队，evtq 自带锁）
	 * 读者：控制线程（取事件）/ 任意线程（读发布值，用 lock 保护）
	 * 临界区里只有几条赋值，不包含任何 I/O、状态机动作或循环。 */
	pthread_mutex_t lock;
	int published_state; /* enum cats_state，每拍发布一次 */
	int published_fault; /* enum cats_fault，与状态同拍发布 */
	struct cats_axis_sample published_axes[CATS_MAX_AXES]; /* 各轴实时反馈样本 */
};

/* 取当前时刻；未接管主站时返回 0（I/O/服务线程用） */
uint32_t now_ms(struct cats_control *app);

#endif /* CONTROL_INTERNAL_H */
