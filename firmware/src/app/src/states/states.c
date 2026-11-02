/*
 * states.c — 状态表与状态名映射（**唯一的注册点**）
 *
 * 新增状态时：写好 state_xxx.c → 在 states.h 加声明 → 在本表加一行。
 * 下标与 enum cats_run_state 一一对应，靠 static_assert 保证不会错位。
 */

#include "states.h"

const hsm_state_fn states[CATS_RUN_STATE_COUNT] = {
	[CATS_RUN_IDLE] = st_idle,       [CATS_RUN_STARTING] = st_starting,
	[CATS_RUN_RUNNING] = st_running, [CATS_RUN_STOPPING] = st_stopping,
	[CATS_RUN_FAULT] = st_fault,     [CATS_RUN_ESTOP] = st_estop,
};

static const char *const NAMES[CATS_RUN_STATE_COUNT] = {
	[CATS_RUN_IDLE] = "IDLE",       [CATS_RUN_STARTING] = "STARTING",
	[CATS_RUN_RUNNING] = "RUNNING", [CATS_RUN_STOPPING] = "STOPPING",
	[CATS_RUN_FAULT] = "FAULT",     [CATS_RUN_ESTOP] = "ESTOP",
};

const char *state_name(hsm_state_fn fn)
{
	for (int i = 0; i < CATS_RUN_STATE_COUNT; i++) {
		if (states[i] == fn) {
			return NAMES[i];
		}
	}
	return "?";
}

uint32_t now_ms(struct cats_app *app)
{
	return app->master != NULL ? can_io_now_ms(app->master->bus) : 0;
}
