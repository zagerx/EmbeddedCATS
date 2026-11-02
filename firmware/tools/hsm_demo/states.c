/*
 * states.c — 状态注册表与信号名（**唯一注册点**）
 *
 * 新增状态时只改两处：states.h 加 extern，这里加一行表项。
 */

#include "states.h"

#include <stddef.h>

/* 全部状态。顺序不重要，这个名字查表只为日志/调试方便 */
static const struct hsm_state *const ALL[] = {
	&st_root, &st_operational, &st_idle, &st_running, &st_stopping, &st_fault, &st_estop,
};

const struct hsm_state *demo_find_state(const char *name)
{
	const unsigned n = sizeof(ALL) / sizeof(ALL[0]);

	if (name == NULL) {
		return NULL;
	}
	for (unsigned i = 0; i < n; i++) {
		const char *a = ALL[i]->name;
		const char *b = name;

		while (*a != '\0' && *b != '\0' && *a == *b) {
			a++;
			b++;
		}
		if (*a == *b) {
			return ALL[i];
		}
	}
	return NULL;
}

const char *demo_sig_name(int sig)
{
	switch (sig) {
	case HSM_SIG_ENTRY:
		return "ENTRY";
	case HSM_SIG_EXIT:
		return "EXIT";
	case DEMO_SIG_TICK:
		return "TICK";
	case DEMO_SIG_START:
		return "START";
	case DEMO_SIG_STOP:
		return "STOP";
	case DEMO_SIG_READY:
		return "READY";
	case DEMO_SIG_FAULT:
		return "FAULT";
	case DEMO_SIG_RESET:
		return "RESET";
	default:
		return "?";
	}
}
