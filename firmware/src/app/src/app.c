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
static int s_started; /* 标记 hsm 是否已初始化（区分"还没设日志"的首次调用） */

static void ensure_init(void)
{
	if (!s_started) {
		hsm_init(&s_app.hsm, st_idle, state_name, s_app.hsm.log, now_ms(&s_app));
		s_started = 1;
	}
}

/* ---- 对外接口 ---- */

void cats_app_set_log(void (*log_fn)(const char *line))
{
	s_app.hsm.log = log_fn;
	s_app.hsm.state_name = state_name;
	ensure_init();
}

enum cats_app_result cats_app_start(struct co_master *master)
{
	if (master == NULL) {
		return CATS_APP_ERR_PARAM;
	}
	if (s_app.master != NULL && s_app.hsm.state != st_idle) {
		return CATS_APP_ERR_STATE; /* 已经在跑，先 stop */
	}
	ensure_init();
	s_app.master = master;
	return cats_app_event(CATS_EV_START);
}

enum cats_app_result cats_app_poll(uint32_t timeout_ms)
{
	static const struct hsm_event tick = {HSM_SIG_TICK};
	uint32_t now;

	(void)timeout_ms; /* 节拍由壳决定，这里只用 now_ms 算间隔 */

	if (s_app.master == NULL) {
		return CATS_APP_ERR_STATE;
	}
	ensure_init();

	now = now_ms(&s_app);
	/* 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_app.master, now);
	hsm_dispatch(&s_app.hsm, &tick, now);
	return CATS_APP_OK;
}

enum cats_app_result cats_app_event(enum cats_event ev)
{
	static const int SIGS[] = {
		[CATS_EV_START] = HSM_SIG_START,
		[CATS_EV_STOP] = HSM_SIG_STOP,
		[CATS_EV_RESET] = HSM_SIG_RESET,
		[CATS_EV_TICK] = HSM_SIG_TICK,
	};
	struct hsm_event e;

	if (s_app.master == NULL || ev < 0 || ev > CATS_EV_TICK) {
		return CATS_APP_ERR_STATE;
	}
	ensure_init();
	e.sig = SIGS[ev];
	hsm_dispatch(&s_app.hsm, &e, now_ms(&s_app));
	return CATS_APP_OK;
}

void cats_app_stop(void)
{
	(void)cats_app_event(CATS_EV_STOP);
}

enum cats_app_result cats_app_reset(void)
{
	return cats_app_event(CATS_EV_RESET);
}

void cats_app_estop(void)
{
	if (s_app.master == NULL) {
		return;
	}
	ensure_init();

	/* 急停不走事件表：任何状态都强制切过去，ENTRY 里做安全动作 */
	if (s_app.hsm.log != NULL) {
		s_app.hsm.log("ESTOP!");
	}
	hsm_force(&s_app.hsm, st_estop, now_ms(&s_app));
}

enum cats_run_state cats_app_state(void)
{
	for (int i = 0; i < CATS_RUN_STATE_COUNT; i++) {
		if (states[i] == s_app.hsm.state) {
			return (enum cats_run_state)i;
		}
	}
	return CATS_RUN_IDLE;
}
