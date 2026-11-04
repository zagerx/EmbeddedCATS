/*
 * states.h — 状态描述符的集中声明 + 共享工具
 *
 * 每个状态一个 .c 文件（state_*.c），这里只放它们的描述符声明，
 * 以及"从 hsm 指针找回 cats_control"的 container_of 小工具。
 *
 * 层次（见各文件里状态对象的 .super 字段）：
 *   st_root
 *   ├── st_online      运行期父状态：故障出口 + 心跳 + 上抛其余事件
 *   │   ├── st_preparing / st_enabling / st_holding / st_moving / st_halting
 *   ├── st_fault       （直接挂 root：安全态不继承运行期动作）
 *   └── st_estop
 *
 * 约定：运行期子状态的 TICK 处理完自己的事后返回 HSM_RET_SUPER，
 *       由 st_online 补做公共判定（故障码 / 心跳），由它决定本拍最终归属。
 */

#ifndef CONTROL_STATES_H
#define CONTROL_STATES_H

#include "../control_internal.h"

#include <stddef.h>

/* 从 hsm 成员指针反推 cats_control：各状态函数用它拿到应用上下文 */
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

extern const struct hsm_state st_root;
extern const struct hsm_state st_online;
extern const struct hsm_state st_preparing;
extern const struct hsm_state st_enabling;
extern const struct hsm_state st_holding;
extern const struct hsm_state st_moving;
extern const struct hsm_state st_halting;
extern const struct hsm_state st_fault;
extern const struct hsm_state st_estop;

#endif /* CONTROL_STATES_H */
