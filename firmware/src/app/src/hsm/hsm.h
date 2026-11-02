/*
 * hsm.h — 层次状态机内核（QP 风格，**机制层，与业务无关**）
 *
 * 借 QP/QEP 的四样东西，不借它的活动对象内核：
 *   1. 层次：每个状态带 super 链接，不认识的���件自动上抛（HSM_SUPER）；
 *   2. LCA 迁移：EXIT 只到最近公共祖先，父状态只进出一次；
 *   3. RTC：hsm_dispatch() 一次处理一个事件，handler 之间不嵌套。
 *
 * 不做的事：零动态内存、无队列、无定时器、无 defer、无事件池、无引用计数。
 *             日志请在状态的 ENTRY 里自己记：迁移必然经过 ENTRY。
 *
 * 不变量（由 hsm.c 保证）：
 *   - h->state 只在 hsm.c 的 transition() 里改写一处；
 *   - ENTRY/EXIT 的逐级调用由内核负责，handler 返回 TRAN 计 anomaly 后忽略；
 *   - 迁移到自己的祖先时，目标不重新 ENTRY（LCA == 目标）。
 */

#ifndef APP_HSM_H
#define APP_HSM_H

#include <stddef.h>
#include <stdint.h>

struct hsm;
struct hsm_state;

/* ---- 容量：编译期定死，零动态内存 ---- */
#define HSM_MAX_DEPTH 8u /* 父链深度上限，兼作防环 */

/* 事件：信号 + 一个整型参数；不放指针，避免悬挂 */
struct hsm_event {
	int sig;
	int32_t arg;
};

typedef int (*hsm_state_fn)(struct hsm *me, const struct hsm_event *e);

/* 状态描述符：状态的**身份**就是它的地址 */
struct hsm_state {
	hsm_state_fn handler;
	const struct hsm_state *super; /* NULL = 根 */
	const char *name;              /* 日志 / 界面显示用 */
};

/*
 * 状态定义：handler 约定名为 <desc>_h，需在宏之前 static 前向声明。
 *   static int st_running_h(struct hsm *me, const struct hsm_event *e);
 *   HSM_STATE_DEF(st_running, &st_operational, "RUNNING");
 */
#define HSM_STATE_DEF(desc, sup, nm) const struct hsm_state desc = {desc##_h, sup, nm}

enum hsm_ret {
	HSM_RET_HANDLED = 0, /* 已处理，留在本状态 */
	HSM_RET_TRAN,        /* 请求迁移，目标在 me->target */
	HSM_RET_SUPER        /* 不认识 → 交给父状态 */
};

enum hsm_sig {
	HSM_SIG_ENTRY = 0, /* 伪事件：进入状态 */
	HSM_SIG_EXIT,      /* 伪事件：离开状态 */
	HSM_SIG_USER       /* 业务事件从这一号起，由上层定义 */
};

struct hsm {
	const struct hsm_state *state;  /* 当前状态 */
	const struct hsm_state *target; /* HSM_TRAN() 填的迁移目标 */
	uint32_t entered_ms;            /* 进入当前状态的时刻 */
	unsigned anomalies; /* handler 在 ENTRY/EXIT 里返回 TRAN 的非法动作次数 */
};

#define HSM_HANDLED() return HSM_RET_HANDLED
#define HSM_SUPER() return HSM_RET_SUPER
#define HSM_TRAN(t)                                                                                \
	do {                                                                                       \
		me->target = (t);                                                                  \
		return HSM_RET_TRAN;                                                               \
	} while (0)

/* 生命周期：清零计数，设初态并从根一路 ENTRY 到 initial */
void hsm_init(struct hsm *h, const struct hsm_state *initial, uint32_t now);

/*
 * 事件注入：**唯一的执行点**。处理一个事件，返回 1 表示发生了迁移。
 * 不要在 handler 里再调它（会嵌套）——需要"事件套事件"时置个标志，下一拍再处理。
 */
int hsm_dispatch(struct hsm *h, int sig, int32_t arg, uint32_t now);

/*
 * 强制迁移：绕过事件处理，直接 EXIT 链 → 换状态 → ENTRY 链。
 * 只给"急停"这类安全动作用——它必须在任何状态下都立即生效。
 * 只能在 dispatch 之外调。
 */
void hsm_transition(struct hsm *h, const struct hsm_state *target, uint32_t now);

/* 查询 */
const struct hsm_state *hsm_current(const struct hsm *h);
uint32_t hsm_entered_ms(const struct hsm *h);
unsigned hsm_anomalies(const struct hsm *h);

#endif /* APP_HSM_H */
