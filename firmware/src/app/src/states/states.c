/*
 * states.c — 状态表与状态名映射（**唯一的注册点**）
 *
 * 新增状态时：写好 state_xxx.c → 在 states.h 加声明 → 在本表加一行。
 * 下标与 enum cats_run_state 一一对应，靠 static_assert 保证不会错位。
 */

#include "states.h"

#include <stdarg.h>
#include <stdio.h>

/* ---- 状态表：下标 = enum cats_run_state，值 = 状态描述符 ---- */

const struct hsm_state *const states[CATS_RUN_STATE_COUNT] = {
	[CATS_RUN_PREPARING] = &st_preparing, [CATS_RUN_RUNNING] = &st_running,
	[CATS_RUN_STOPPING] = &st_stopping,   [CATS_RUN_FAULT] = &st_fault,
	[CATS_RUN_ESTOP] = &st_estop,
};

static const char *const NAMES[CATS_RUN_STATE_COUNT] = {
	[CATS_RUN_PREPARING] = "PREPARING", [CATS_RUN_RUNNING] = "RUNNING",
	[CATS_RUN_STOPPING] = "STOPPING",   [CATS_RUN_FAULT] = "FAULT",
	[CATS_RUN_ESTOP] = "ESTOP",
};

static const char *const FAULT_NAMES[] = {
	[CATS_FAULT_NONE] = "NONE",
	[CATS_FAULT_BOOTUP_TIMEOUT] = "BOOTUP_TIMEOUT",
	[CATS_FAULT_HEARTBEAT_LOST] = "HEARTBEAT_LOST",
};

const char *cats_app_state_name(enum cats_run_state s)
{
	if ((int)s < 0 || s >= CATS_RUN_STATE_COUNT) {
		return "?";
	}
	return NAMES[s];
}

const char *fault_name(enum cats_app_fault f)
{
	if ((int)f < 0 ||
	    f >= (enum cats_app_fault)(sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]))) {
		return "?";
	}
	return FAULT_NAMES[f];
}

void app_log(struct cats_app *app, const char *fmt, ...)
{
	char line[128];
	va_list ap;

	if (app->log == NULL) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	app->log(line);
}

uint32_t now_ms(struct cats_app *app)
{
	return app->master != NULL ? can_io_now_ms(app->master->bus) : 0;
}
