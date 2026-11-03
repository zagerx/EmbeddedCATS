/*
 * app_internal.h — 应用层内部对象与共享声明（**不对外发布**）
 *
 * 供 app.c 与各 states/state_*.c 共享：
 *   struct cats_app     应用层对象（状态机 + 主站指针 + 轴 + 故障码）
 *   enum cats_app_sig   业务事件（内核只认 ENTRY/EXIT，业务信号从这里起编号）
 *   states[]           状态描述符表（states/states.c 定义）
 *   now_ms()           取当前时刻的小工具
 */

#ifndef APP_APP_INTERNAL_H
#define APP_APP_INTERNAL_H

#include <app/app.h>

#include <axis/axis.h>

#include "hsm/hsm.h"

#define CATS_APP_MAX_AXES 2

/* 业务事件：内核只认 ENTRY/EXIT，业务信号从 HSM_SIG_USER 起编号 */
enum cats_app_sig {
	CATS_SIG_TICK = HSM_SIG_USER, /* 周期节拍，cats_app_poll() 每拍投一个 */
	CATS_SIG_STOP                 /* 请求停止 */
};

/* 业务轴（轴层）：一个轴 = 一个节点 + 该节点的 402 推进器，见 axis/axis.h */
struct cats_app {
	struct co_master *master; /* 内核层主站，只借用；不归本层拥有 */

	struct cats_axis axes[CATS_APP_MAX_AXES];
	struct axis_group group; /* 轴集合：谁在用 SDO 由它排（依次使能） */
	uint8_t axis_count;
	/* 本拍所有轴心跳都正常；每拍由 cats_app_poll() 刷新，状态只读不算 */
	uint8_t hb_ok;

	struct hsm hsm; /* 纯状态机机制（当前状态、迁移目标、进入时刻） */
	enum cats_app_fault fault_code; /* 最后一次出错原因 */

	/* ---- 跨线程只读的发布值（见 app.h「线程约定」）----
	 * 只有控制线程写，其他线程只用 __atomic_* 读。
	 * 刻意不用 mutex：写者不能被界面阻塞，否则节拍会掉。 */
	int published_state; /* enum cats_run_state，每拍发布一次 */
	int published_fault; /* enum cats_app_fault，与状态同拍发布 */
	int estop_req;       /* 非 0 = 有线程请求急停，控制线程下一拍执行 */
	uint32_t tick_count; /* 已推进的拍数（诊断用） */
};

/* states.c 定义的状态表：下标 = enum cats_run_state，值 = 状态描述符 */
extern const struct hsm_state *const states[CATS_RUN_STATE_COUNT];

/* 取当前时刻；未接管主站时返回 0 */
uint32_t now_ms(struct cats_app *app);

#endif /* APP_APP_INTERNAL_H */
