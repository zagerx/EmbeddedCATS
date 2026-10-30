/*
 * app.c — 应用层骨架（**还没有真正收发**）
 *
 * 分工（方案 A，**不用 typedef**）：
 *   struct cats_app    = 应用层对象：主站指针 + 轴集合 + **运行状态**。
 *   struct co_master   = 内核层主站：总线 + 节点 + NMT 状态（协议身份归它）。
 *   enum cats_run_state = IDLE / RUNNING / FAULT，描述“整个程序”的运行状态。
 *
 * 现在只做三件事：记住主站指针、维护运行状态、把非法调用挡回去。
 * 下一步往 cats_app_poll() 里填“每拍发什么、收什么”，其余函数基本不用改。
 */

#include <app/app.h>

#include <stddef.h>

#define CATS_APP_MAX_AXES 2

/* 业务轴（轴层）。现在只是占位，等 axis/ 有实现再填 */
struct cats_axis {
	uint8_t node_id; /* 轴 0 = 节点 1，轴 1 = 节点 2 */
};

struct cats_app {
	struct co_master *master; /* 内核层主站，只借用；不归本层拥有 */

	struct cats_axis axes[CATS_APP_MAX_AXES];
	uint8_t axis_count;

	enum cats_run_state state; /* IDLE / RUNNING / FAULT */
};

static struct cats_app s_app = {NULL, {{0}, {0}}, 0, CATS_RUN_IDLE};

enum cats_app_result cats_app_start(struct co_master *master)
{
	if (master == NULL) {
		return CATS_APP_ERR_PARAM;
	}
	if (s_app.state == CATS_RUN_RUNNING) {
		return CATS_APP_ERR_STATE;
	}
	s_app.master = master;
	s_app.state = CATS_RUN_RUNNING;
	return CATS_APP_OK;
}

enum cats_app_result cats_app_poll(uint32_t timeout_ms)
{
	if (s_app.state != CATS_RUN_RUNNING) {
		return CATS_APP_ERR_STATE;
	}
	(void)timeout_ms;
	/* TODO(下一拍)：NMT 启动节点 → SDO 探测 0x1000 → 状态上抛 */
	return CATS_APP_OK;
}

void cats_app_stop(void)
{
	s_app.master = NULL; /* 主站和总线都由壳持有，本层不释放 */
	s_app.state = CATS_RUN_IDLE;
}

enum cats_run_state cats_app_state(void)
{
	return s_app.state;
}
