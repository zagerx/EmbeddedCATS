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

/* 迁移发生时内核回调到这里：迁移日志的唯一出处 */
static void on_transition(void *ctx, const struct hsm_state *from, const struct hsm_state *to)
{
	struct cats_app *app = ctx;

	app_log(app, "%s -> %s", from != NULL ? from->name : "?", to->name);
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
	/* 初态即准备态：hsm_init 从根一路 ENTRY 到 PREPARING，广播复位从这里发出 */
	hsm_init(&s_app.hsm, &st_preparing, now_ms(&s_app));
	hsm_set_trace(&s_app.hsm, on_transition, &s_app);
	return CATS_APP_OK;
}

enum cats_app_result cats_app_poll(void)
{
	uint32_t now;

	if (s_app.master == NULL) {
		return CATS_APP_ERR_STATE;
	}

	now = now_ms(&s_app);
	/* 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_app.master, now);

	/* 一拍一个 TICK，再让内核排空队列（到期定时器也在这里入队） */
	hsm_post(&s_app.hsm, CATS_SIG_TICK, 0);
	hsm_poll(&s_app.hsm, now);
	return CATS_APP_OK;
}

void cats_app_estop(void)
{
	if (s_app.master == NULL) {
		return;
	}

	app_log(&s_app, "ESTOP 触发");

	/* 急停不走队列：任何状态下都立即切过去，ENTRY 里做安全动作 */
	hsm_force(&s_app.hsm, &st_estop, now_ms(&s_app));
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
