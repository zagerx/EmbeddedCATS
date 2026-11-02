/*
 * states.h — 状态声明与业务信号（演示工程对外的唯一注册面）
 *
 * 新增一个状态：写 state_xxx.c → 在本文件加 extern → 在 states.c 的表里加一行。
 *
 * 层次（各文件的 HSM_STATE_DEF 第二个参数就是 super）：
 *
 *   ROOT
 *   ├── OPERATIONAL      运行期父状态：IDLE / RUNNING / STOPPING 的共同祖先
 *   │   ├── IDLE
 *   │   ├── RUNNING
 *   │   └── STOPPING
 *   ├── FAULT
 *   └── ESTOP
 */

#ifndef DEMO_STATES_H
#define DEMO_STATES_H

#include "hsm.h"

/* 业务事件：内核只认 ENTRY/EXIT，业务信号从 HSM_SIG_USER 起编号 */
enum demo_sig {
	DEMO_SIG_TICK = HSM_SIG_USER, /* 周期节拍，每拍投一个 */
	DEMO_SIG_START,               /* 请求启动 */
	DEMO_SIG_STOP,                /* 请求停止 */
	DEMO_SIG_READY,               /* 设备就绪 */
	DEMO_SIG_FAULT,               /* 出错了 */
	DEMO_SIG_RESET                /* 复位 / 清除故障 */
};

/*
 * handler 写法约定（本骨架里每个状态都长这样）：
 *
 *   int ret = HSM_RET_SUPER;        // 默认：不认识的事件交给父状态
 *   switch (e->sig) {
 *   case HSM_SIG_ENTRY:  <进入动作>; ret = HSM_RET_HANDLED; break;
 *   case HSM_SIG_EXIT:   <退出动作>; ret = HSM_RET_HANDLED; break;
 *   case <业务事件>:     me->target = &<目标>; ret = HSM_RET_TRAN; break;
 *   default:             break;     // ret 仍是 SUPER
 *   }
 *   return ret;                     // 整个函数只有一个出口
 *
 * 内核自带的 HSM_HANDLED() / HSM_SUPER() / HSM_TRAN() 宏是同一套含义，
 * 只是它们直接在宏里 return（省掉 break）。两种都能用，本骨架统一用上面这种。
 */

extern const struct hsm_state st_root;
extern const struct hsm_state st_operational;
extern const struct hsm_state st_idle;
extern const struct hsm_state st_running;
extern const struct hsm_state st_stopping;
extern const struct hsm_state st_fault;
extern const struct hsm_state st_estop;

/* states.c 定义：按名字找状态（日志/调试用），找不到返回 NULL */
const struct hsm_state *demo_find_state(const char *name);

/* states.c 定义：信号名（日志用），未知返回 "?" */
const char *demo_sig_name(int sig);

#endif /* DEMO_STATES_H */
