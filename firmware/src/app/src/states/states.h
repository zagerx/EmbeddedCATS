/*
 * states.h — 状态描述符的集中声明 + 共享工具
 *
 * 每个状态一个 .c 文件（state_*.c），这里只放它们的描述符声明，
 * 以及"从 hsm 指针找回 cats_app"的 container_of 小工具。
 *
 * 层次（见各文件里 HSM_STATE_DEF 的第二个参数）：
 *   st_root
 *   ├── st_operational   运行期父状态（本轮不处理事件，留给公共逻辑）
 *   │   ├── st_preparing / st_running / st_stopping
 *   ├── st_fault
 *   └── st_estop
 */

#ifndef APP_STATES_H
#define APP_STATES_H

#include "../app_internal.h"

#include <stddef.h>

/* 从 hsm 成员指针反推 cats_app：各状态函数用它拿到应用上下文 */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

extern const struct hsm_state st_root;
extern const struct hsm_state st_operational;
extern const struct hsm_state st_preparing;
extern const struct hsm_state st_running;
extern const struct hsm_state st_stopping;
extern const struct hsm_state st_fault;
extern const struct hsm_state st_estop;

#endif /* APP_STATES_H */
