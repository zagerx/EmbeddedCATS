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

/* 推进一次状态机；发生迁移时记一行日志（迁移日志的唯一出处） */
static void dispatch(const struct hsm_event *e, uint32_t now)
{
	hsm_state_fn from = s_app.hsm.state;

	hsm_dispatch(&s_app.hsm, e, now);
	if (s_app.hsm.state != from) {
		app_log(&s_app, "%s -> %s", state_name(from), state_name(s_app.hsm.state));
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
	if (master == NULL) {
		return CATS_APP_ERR_PARAM;
	}
	if (s_app.master != NULL) {
		return CATS_APP_ERR_STATE; /* 只初始化一次 */
	}

	s_app.master = master;
	/* 初态即准备态：hsm_init 会触发 PREPARING 的 ENTRY，广播复位从这里发出 */
	hsm_init(&s_app.hsm, st_preparing, now_ms(&s_app));
	return CATS_APP_OK;
}

enum cats_app_result cats_app_poll(void)
{
	static const struct hsm_event tick = {HSM_SIG_TICK};
	uint32_t now;

	if (s_app.master == NULL) {
		return CATS_APP_ERR_STATE;
	}

	now = now_ms(&s_app);
	/* 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_app.master, now);
	dispatch(&tick, now);
	return CATS_APP_OK;
}

void cats_app_estop(void)
{
	hsm_state_fn from;

	if (s_app.master == NULL) {
		return;
	}

	app_log(&s_app, "ESTOP 触发");

	/* 急停不走事件表：任何状态都强制切过去，ENTRY 里做安全动作 */
	from = s_app.hsm.state;
	hsm_force(&s_app.hsm, st_estop, now_ms(&s_app));
	if (s_app.hsm.state != from) {
		app_log(&s_app, "%s -> %s", state_name(from), state_name(st_estop));
	}
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
