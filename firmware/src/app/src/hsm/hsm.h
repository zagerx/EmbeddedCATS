/*
 * hsm.h — 通用层次状态机内核（QP 风格，**机制层，与业务无关**）
 *
 * 三条规则：
 *   1. 状态 = 函数指针；每个状态一个函数，自己处理事件、自己决定迁移；
 *   2. 迁移不是直接改 state，而是返回 HSM_RET_TRAN 并把目标填进 me->target，
 *      由 hsm_dispatch() 统一完成"旧状态 EXIT → 换指针 → 新状态 ENTRY"；
 *   3. ENTRY / EXIT 也是事件：进入/离开状态时的动作有固定位置可写。
 *
 * 它不认识 CAN、不认识轴、不认识 CATS —— 任何项目都能复用。
 */

#ifndef APP_HSM_H
#define APP_HSM_H

#include <stdint.h>

struct hsm;

struct hsm_event {
	int sig;
};

/* 状态 handler：吃一个事件，返回处理结果 */
typedef int (*hsm_state_fn)(struct hsm *me, const struct hsm_event *e);

enum hsm_ret {
	HSM_RET_HANDLED = 0, /* 已处理，留在本状态 */
	HSM_RET_TRAN,        /* 请求迁移，目标在 me->target */
	HSM_RET_IGNORED      /* 不认识这个事件 */
};

enum hsm_sig {
	HSM_SIG_ENTRY = 0, /* 伪事件：进入状态时被 dispatch 调用 */
	HSM_SIG_EXIT,      /* 伪事件：离开状态时被调用 */
	HSM_SIG_TICK,      /* 以下才是应用事件 */
	HSM_SIG_START,
	HSM_SIG_STOP,
	HSM_SIG_RESET
};

struct hsm {
	hsm_state_fn state;  /* 当前状态 */
	hsm_state_fn target; /* HSM_TRAN() 填的迁移目标 */
	uint32_t entered_ms; /* 进入本状态的时刻，用于算"等了多久" */
	void (*log)(const char *line);
	const char *(*state_name)(hsm_state_fn fn); /* 日志用：函数指针 → 名字 */
};

#define HSM_HANDLED() return HSM_RET_HANDLED
#define HSM_IGNORED() return HSM_RET_IGNORED
#define HSM_TRAN(t)                                                                                \
	do {                                                                                       \
		me->target = (t);                                                                  \
		return HSM_RET_TRAN;                                                               \
	} while (0)

/* 初始化：设初态并触发一次 ENTRY。不触发迁移 */
void hsm_init(struct hsm *h, hsm_state_fn initial, const char *(*name)(hsm_state_fn),
	      void (*log)(const char *), uint32_t now);

/* 注入事件：驱动一次状态处理；若发生迁移，自动完成 EXIT → ENTRY */
void hsm_dispatch(struct hsm *h, const struct hsm_event *e, uint32_t now);

/*
 * 强制迁移：绕过事件处理，直接 EXIT → 换状态 → ENTRY。
 * 只给"急停"这类安全动作用——它必须在任何状态下都立即生效，
 * 不能等当前状态 handler 返回。其余场景一律走 hsm_dispatch()。
 */
void hsm_force(struct hsm *h, hsm_state_fn target, uint32_t now);

#endif /* APP_HSM_H */
