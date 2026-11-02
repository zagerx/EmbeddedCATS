/*
 * states.h — 状态函数的集中声明 + 共享工具
 *
 * 每个状态一个 .c 文件（state_*.c），这里只放它们的声明，
 * 以及"从 hsm 指针找回 cats_app"的 container_of 小工具。
 */

#ifndef APP_STATES_H
#define APP_STATES_H

#include "../app_internal.h"

#include <stddef.h>

/* 从 hsm 成员指针反推 cats_app：各状态函数用它拿到应用上下文 */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

int st_preparing(struct hsm *me, const struct hsm_event *e);
int st_running(struct hsm *me, const struct hsm_event *e);
int st_stopping(struct hsm *me, const struct hsm_event *e);
int st_fault(struct hsm *me, const struct hsm_event *e);
int st_estop(struct hsm *me, const struct hsm_event *e);

/* 格式化后走 app->log 输出（未注入日志时静默）。格式化只有这一处 */
void app_log(struct cats_app *app, const char *fmt, ...);

#endif /* APP_STATES_H */
