/*
 * app_internal.h — 应用层内部对象与共享声明（**不对外发布**）
 *
 * 供 app.c 与各 states/state_*.c 共享：
 *   struct cats_app     应用层对象（状态机 + 主站指针 + 轴 + 故障码）
 *   states[]           状态函数表（states/states.c 定义，app.c 用于初始化与日志）
 *   state_name()       函数指针 → 状态名
 *   now_ms()           取当前时刻的小工具
 */

#ifndef APP_APP_INTERNAL_H
#define APP_APP_INTERNAL_H

#include <app/app.h>

#include "hsm/hsm.h"

#define CATS_APP_MAX_AXES 2

/* 业务轴（轴层）。现在只是占位，等 axis/ 有实现再填 */
struct cats_axis {
	uint8_t node_id; /* 轴 0 = 节点 1，轴 1 = 节点 2 */
};

struct cats_app {
	struct co_master *master; /* 内核层主站，只借用；不归本层拥有 */

	struct cats_axis axes[CATS_APP_MAX_AXES];
	uint8_t axis_count;

	struct hsm hsm;      /* 状态机（含当前状态函数指针、entered_ms、日志） */
	uint32_t fault_code; /* 最后一次出错原因，供界面显示 */
};

/* states.c 定义的状态表：下标 = enum cats_run_state，值 = 状态函数 */
extern const hsm_state_fn states[CATS_RUN_STATE_COUNT];

/* 日志用：把状态函数指针翻成名字 */
const char *state_name(hsm_state_fn fn);

/* 取当前时刻；未接管主站时返回 0 */
uint32_t now_ms(struct cats_app *app);

#endif /* APP_APP_INTERNAL_H */
