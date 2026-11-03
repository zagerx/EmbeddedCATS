/*
 * app.c — 应用层装配与对外接口（**不含状态逻辑**）
 *
 * 职责：把对外 API 翻译成状态机事件，其余一概不管。
 *   状态逻辑在 states/state_*.c（一状态一文件），状态机机制在 hsm/hsm.c，
 *   状态注册在 states/states.c，内部对象在 app_internal.h。
 */

#include <app/app.h>

#include "app_internal.h"
#include "states/states.h"

static struct cats_app s_app;

/* 迁移日志：内核不再提供 trace 回调，这里用迁移前后的状态对比补上 */
static void log_transition(const struct hsm_state *before)
{
	const struct hsm_state *after = hsm_current(&s_app.hsm);

	if (after != before) {
		app_log(&s_app, "%s -> %s", before->name, after->name);
	}
}

/* ---- 对外接口 ---- */

void cats_app_set_log(void (*log_fn)(const char *line))
{
	/* 纯赋值：init 之前/之后调都可以。NULL 表示静默 */
	s_app.log = log_fn;
}

enum cats_app_result cats_app_init(struct co_master *master)
{
	uint8_t i;

	if (master == NULL) {
		return CATS_APP_ERR_PARAM;
	}
	if (s_app.master != NULL) {
		return CATS_APP_ERR_STATE; /* 只初始化一次 */
	}

	s_app.master = master;
	/* 轴 = 主站上已登记的节点，站点号只在装配层声明一次，这里读回来 */
	s_app.axis_count =
		master->node_count < CATS_APP_MAX_AXES ? master->node_count : CATS_APP_MAX_AXES;
	for (i = 0; i < s_app.axis_count; i++) {
		s_app.axes[i].node_id = master->node_id[i];
	}

	/* 初态即准备态：hsm_init 从根一路 ENTRY 到 PREPARING，广播复位从这里发出 */
	hsm_init(&s_app.hsm, &st_preparing, now_ms(&s_app));
	return CATS_APP_OK;
}

int all_axes_alive(const struct cats_app *app)
{
	uint8_t i;

	if (app == NULL || app->master == NULL) {
		return 0;
	}
	for (i = 0; i < app->axis_count; i++) {
		if (co_master_node_alive(app->master, app->axes[i].node_id) != 0) {
			return 0;
		}
	}
	return 1;
}

enum cats_app_result cats_app_poll(void)
{
	const struct hsm_state *before;
	uint32_t now;

	if (s_app.master == NULL) {
		return CATS_APP_ERR_STATE;
	}

	now = now_ms(&s_app);
	/* 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_app.master, now);

	/* 一拍推一次状态机（内部处理一个事件） */
	before = hsm_current(&s_app.hsm);
	hsm_dispatch(&s_app.hsm, CATS_SIG_TICK, 0, now);
	log_transition(before);
	return CATS_APP_OK;
}

void cats_app_estop(void)
{
	const struct hsm_state *before;

	if (s_app.master == NULL) {
		return;
	}

	app_log(&s_app, "ESTOP 触发");

	/* 急停不走队列：任何状态下都立即切过去，ENTRY 里做安全动作 */
	before = hsm_current(&s_app.hsm);
	hsm_transition(&s_app.hsm, &st_estop, now_ms(&s_app));
	log_transition(before);
}

enum cats_run_state cats_app_state(void)
{
	for (int i = 0; i < CATS_RUN_STATE_COUNT; i++) {
		if (states[i] == s_app.hsm.state) {
			return (enum cats_run_state)i;
		}
	}
	return CATS_RUN_PREPARING;
}
