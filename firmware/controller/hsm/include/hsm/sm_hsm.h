/*
 * sm_hsm.h - 层次状态机（HSM）内核
 *
 * 语义等价于 Quantum Leaps QP/C 的 QEP-QHsm（源码 qep_hsm.c），
 * 本实现为独立编写，未包含 QP 源码，不涉及 QP 的 GPLv3 授权。
 *
 * 内核只做三件事：
 *   1. 沿状态层次分发事件（自最内层状态向上冒泡）
 *   2. 计算一次转移的退出链与进入链（LCA 算法）
 *   3. 沿初始转移链钻入目标状态的最内层子状态
 *
 * 不含：事件队列、时间事件、活动对象、正交区域、状态表、日志、
 *       动态内存。不依赖 RTOS 或任何库。
 *
 * 典型用法（每个状态一个 .c 文件）：
 *
 *   [states.h] 声明所有状态，供各 .c 文件互相引用：
 *
 *   sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e);
 *   sm_ret_t idle(struct sm_hsm *const me, const struct sm_event *const e);
 *
 *   [states.c] 最顶层初始转移：
 *
 *   sm_ret_t initial(struct sm_hsm *const me, const struct sm_event *const e)
 *   {
 *           return SM_TRAN(&idle);
 *   }
 *
 *   [state_idle.c] 一个状态一个文件：
 *
 *   sm_ret_t idle(struct sm_hsm *const me, const struct sm_event *const e)
 *   {
 *           switch (e->sig) {
 *           case SM_SIG_ENTRY:
 *                   ...;
 *                   return SM_HANDLED();
 *           }
 *           return SM_SUPER(&sm_top);
 *   }
 *
 *   struct sm_hsm me;
 *   sm_hsm_init(&me, &initial);
 *   sm_hsm_dispatch(&me, &evt);
 */

#ifndef _SM_HSM_H
#define _SM_HSM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * 状态嵌套最大深度（不含顶层状态）。
 * dispatch() 在栈上开 SM_CFG_MAX_NEST_DEPTH + 1 个指针的 path[] 数组，
 * 32 位平台上默认 9 * 4 = 36 字节。
 */
#ifndef SM_CFG_MAX_NEST_DEPTH
#define SM_CFG_MAX_NEST_DEPTH 8
#endif

/* 事件信号 */
typedef uint16_t sm_signal_t;

/* 状态处理函数的返回值 */
typedef uint8_t sm_ret_t;

struct sm_hsm;
struct sm_event;

/* 状态处理函数：返回处理结果，转移目标/父状态经 me->temp 传回 */
typedef sm_ret_t (*sm_state_t)(struct sm_hsm *const me, const struct sm_event *const e);

/*
 * 事件基类。所有用户定义的事件结构体必须以 struct sm_event 作为
 * 第一个成员，内核只认识 sig 字段。
 */
struct sm_event {
	sm_signal_t sig;
};

/*
 * 事件参数：内核只认识 sig（与 QP 的 QEvt 一致——基类只承载框架需要
 * 的信息），具体数据一律由"派生事件结构体"自己携带。
 *
 * 规则：派生结构体必须把 struct sm_event 放在第一个成员。
 * C 标准保证"指向结构体的指针"与"指向其第一个成员的指针"指向同一
 * 地址（C11 6.7.2.1p15），因此下面的强转是合法的。
 *
 *   enum {
 *           SIG_CMD_RUN = SM_SIG_USER,
 *           SIG_EVT_FAULT,
 *   };
 *
 *   struct cmd_run_evt {
 *           struct sm_event super;      (必须是第一个成员)
 *           int32_t target_rpm;
 *   };
 *
 *   在状态处理函数里还原：
 *
 *   const struct cmd_run_evt *ce = SM_EVT_CAST(struct cmd_run_evt, e);
 *   ... ce->target_rpm ...
 *
 * 常量事件可以直接静态定义，反复复用，零构造开销：
 *
 *   static const struct cmd_run_evt evt_run = { { SIG_CMD_RUN }, 3000 };
 *
 * 注意：dispatch() 是同步的（run-to-completion），事件指针只在调用期间
 * 有效，内核从不保存它，因此不需要 QP 那样的引用计数/内存池字段。
 */
#define SM_EVT_CAST(type_, e_) ((const type_ *)(e_))

/*
 * 状态机实例。字段全部由内核维护，用户不得直接读写。
 */
struct sm_hsm {
	sm_state_t state; /* 当前活动配置的最内层状态 */
	sm_state_t temp;  /* 转移目标 / 父状态暂存 */
	uint8_t busy;     /* dispatch() 重入保护 */
};

/*
 * 框架保留信号。用户自定义信号从 SM_SIG_USER 开始。
 *
 * SM_SIG_EMPTY 是内部协议：向状态处理函数投递该信号，它必须
 * return SM_SUPER(&父状态)，以此来反查父状态（不建状态表）。
 */
#define SM_SIG_EMPTY ((sm_signal_t)0)
#define SM_SIG_ENTRY ((sm_signal_t)1)
#define SM_SIG_EXIT ((sm_signal_t)2)
#define SM_SIG_INIT ((sm_signal_t)3)
#define SM_SIG_USER ((sm_signal_t)4)

/*
 * 状态处理函数返回值。数值顺序与 QP 完全一致，算法依赖其比较关系。
 *
 * SUPER     已把父状态放入 temp
 * UNHANDLED 守卫不满足，事件继续向父状态冒泡
 * HANDLED   内部转移：事件被消费，状态不变，不执行任何 entry/exit
 * TRAN      已把转移目标放入 temp
 * TRAN_HIST 已把历史状态放入 temp（内核不自动维护历史变量）
 * IGNORED   顶层状态专用：无人处理该事件
 */
#define SM_RET_SUPER ((sm_ret_t)0)
#define SM_RET_UNHANDLED ((sm_ret_t)1)
#define SM_RET_HANDLED ((sm_ret_t)2)
#define SM_RET_TRAN ((sm_ret_t)3)
#define SM_RET_TRAN_HIST ((sm_ret_t)4)
#define SM_RET_IGNORED ((sm_ret_t)5)

/*
 * 状态处理函数内部使用的宏，全部依赖参数名 me。
 */
#define SM_SUPER(super_) (((me)->temp = (super_)), SM_RET_SUPER)
#define SM_TRAN(target_) (((me)->temp = (target_)), SM_RET_TRAN)
#define SM_TRAN_HIST(hist_) (((me)->temp = (hist_)), SM_RET_TRAN_HIST)

#define SM_HANDLED() SM_RET_HANDLED
#define SM_UNHANDLED() SM_RET_UNHANDLED

/*
 * 状态处理函数一律手写完整签名，不做宏包装：
 *
 *   sm_ret_t 状态名(struct sm_hsm *const me, const struct sm_event *const e)
 *
 * 两条纪律：
 *   1. 函数必须有外部链接，并在公共头文件里声明，否则其他 .c 引不到；
 *   2. 不同文件不能定义同名状态，否则链接期报重复定义；
 *      建议状态名与文件名一致（state_idle.c -> idle）。
 *
 * 参数名必须叫 me：SM_SUPER / SM_TRAN / SM_TRAN_HIST 都依赖它。
 */

/*
 * 初始化：把状态机置于"顶层状态"，执行最顶层的初始转移，
 * 再沿 SM_SIG_INIT 链钻到最内层状态。
 *
 * initial 必须 return SM_TRAN(&某个状态)。
 * 该函数被调用时收到的是信号为 SM_SIG_INIT 的保留事件。
 */
void sm_hsm_init(struct sm_hsm *const me, sm_state_t const initial);

/*
 * 分发一个事件。同步执行到 run-to-completion，返回时状态已稳定。
 * 不可重入（重入会被断言捕获）。
 */
void sm_hsm_dispatch(struct sm_hsm *const me, const struct sm_event *const e);

/* 当前活动配置的最内层状态 */
sm_state_t sm_hsm_state(struct sm_hsm const *const me);

/*
 * 顶层状态，所有状态层次的根。它对一切事件返回 SM_RET_IGNORED，
 * 与 SM_RET_UNHANDLED（守卫不满足）语义不同。
 * 无父状态的用户状态应当 return SM_SUPER(&sm_top)。
 */
sm_ret_t sm_top(struct sm_hsm *const me, const struct sm_event *const e);

#endif /* _SM_HSM_H */
