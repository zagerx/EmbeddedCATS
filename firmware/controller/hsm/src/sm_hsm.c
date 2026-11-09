/*
 * sm_hsm.c - 层次状态机内核实现
 *
 * 算法与 Quantum Leaps QP/C 的 qep_hsm.c 等价，逐条对应：
 *
 *   sm_hsm_dispatch()   <-> QHsm_dispatch_
 *   sm_tran_simple()    <-> QHsm_tran_simple_
 *   sm_tran_complex()   <-> QHsm_tran_complex_
 *   sm_enter_target()   <-> QHsm_enter_target_
 *   sm_hsm_init()       <-> QHsm_init_
 *   sm_hsm_is_in()      <-> QHsm_isIn_
 *
 * 一次转移分四步：
 *   1. 事件自最内层状态向上冒泡，第一个返回 SM_RET_TRAN 的状态
 *      就是"转移源"（不是叶子）；
 *   2. 退出源之下、直到叶子的所有状态；
 *   3. 找到源与目标的最近公共祖先 LCA，退出源到 LCA 之间（不含 LCA），
 *      进入 LCA 到目标之间（不含 LCA）；
 *   4. 从目标沿 SM_SIG_INIT 链钻到最内层状态。
 */

#include <hsm/sm_hsm.h>

#include <stdio.h>

/* path[] 长度：最深处是叶子，最浅处是顶层状态 */
#define SM_PATH_LEN (SM_CFG_MAX_NEST_DEPTH + 1)

/*
 * 框架保留信号对应的不可变事件。
 * 下标必须与信号值一致：0=EMPTY, 1=ENTRY, 2=EXIT, 3=INIT。
 */
static const struct sm_event sm_res_evt[4] = {
	{SM_SIG_EMPTY},
	{SM_SIG_ENTRY},
	{SM_SIG_EXIT},
	{SM_SIG_INIT},
};

#define SM_RES_EVT(sig_) (&sm_res_evt[(sig_)])

/*
 * 断言。失败时**不阻塞**（见下面 sm_hsm_assert_failed 的实现）。
 *
 * 定义 SM_CFG_NO_ASSERT 可整体关闭；
 * 自行定义 SM_ASSERT(cond) 可完全接管（例如打印后复位）。
 */
#ifndef SM_ASSERT
#ifdef SM_CFG_NO_ASSERT
/* 关闭断言时仍对条件求值，避免"变量只赋值未使用"的告警 */
#define SM_ASSERT(cond_)                                                                           \
	do {                                                                                       \
		(void)(cond_);                                                                     \
	} while (0)
#else
void sm_hsm_assert_failed(const char *file, unsigned int line);

#define SM_ASSERT(cond_)                                                                           \
	do {                                                                                       \
		if (!(cond_))                                                                      \
			sm_hsm_assert_failed(__FILE__, __LINE__);                                  \
	} while (0)
#endif
#endif

#ifndef SM_CFG_NO_ASSERT
/*
 * 【本工程改造】原版是 for (;;) 死等调试器。
 * 控制层跑在 Linux 的一个线程里，死等会把整条控制链路一起挂住，
 * 所以这里只留证据（stderr）然后返回，由上层 watchdog / 状态发布兜底。
 */
void sm_hsm_assert_failed(const char *file, unsigned int line)
{
	(void)fprintf(stderr, "sm_hsm: assert failed at %s:%u\n", file, line);
}
#endif

/*
 * 求状态 s 的父状态。
 *
 * 协议：向状态处理函数投递 SM_SIG_EMPTY，它必须 return SM_SUPER(&父状态)，
 * 父状态留在 me->temp 中。顶层状态返回 SM_RET_IGNORED 且不改动 temp。
 *
 * 副作用：覆盖 me->temp。
 */
static sm_state_t sm_super_of(struct sm_hsm *const me, sm_state_t const s)
{
	(void)s(me, SM_RES_EVT(SM_SIG_EMPTY));

	return me->temp;
}

sm_ret_t sm_top(struct sm_hsm *const me, const struct sm_event *const e)
{
	(void)me;
	(void)e;

	return SM_RET_IGNORED;
}

/*
 * 进入转移目标。
 *
 * path[0] = 目标，path[1..depth-1] = 目标的祖先（由内向外，不含 LCA）。
 * 先按逆序执行 entry（从 LCA 的下一层一路向下到目标），
 * 再沿 SM_SIG_INIT 链继续向内钻，直到某个状态的 INIT 不再返回
 * SM_RET_TRAN 为止。
 */
static void sm_enter_target(struct sm_hsm *const me, sm_state_t *const path, size_t depth)
{
	sm_state_t t;
	size_t ip;

	for (ip = depth; ip > 0U; ip--)
		(void)path[ip - 1](me, SM_RES_EVT(SM_SIG_ENTRY));

	t = path[0];
	while (t(me, SM_RES_EVT(SM_SIG_INIT)) == SM_RET_TRAN) {
		SM_ASSERT(me->temp != NULL);

		/* 收集"初始目标 -> t"之间的祖先链，不含 t 本身 */
		ip = 0;
		do {
			SM_ASSERT(ip < SM_PATH_LEN);
			path[ip] = me->temp;
			ip++;
			(void)me->temp(me, SM_RES_EVT(SM_SIG_EMPTY));
		} while (me->temp != t);

		/* 由外向内进入 */
		while (ip > 0U) {
			ip--;
			(void)path[ip](me, SM_RES_EVT(SM_SIG_ENTRY));
		}

		t = path[0];
	}
}

/*
 * 处理能直接判定的转移。返回需要进入的层数；
 * 返回 2 表示情况复杂，需交给 sm_tran_complex()。
 *
 * 进入时：path[0] = 目标，path[2] = 转移源。
 * 返回时：me->temp = 源的父状态（供复杂分支使用）。
 */
static size_t sm_tran_simple(struct sm_hsm *const me, sm_state_t *const path)
{
	sm_state_t t = path[0];       /* 目标 */
	sm_state_t const s = path[2]; /* 源 */
	sm_ret_t r;
	size_t ip = 1;

	/* (a) 自转移：退出源再重新进入（外部自转移语义） */
	if (s == t) {
		(void)s(me, SM_RES_EVT(SM_SIG_EXIT));
		return ip;
	}

	/* 求目标的父状态 */
	r = t(me, SM_RES_EVT(SM_SIG_EMPTY));
	SM_ASSERT(r == SM_RET_SUPER);
	t = me->temp;

	/* (b) 源就是目标的父状态：父已活动，只需进入目标 */
	if (s == t)
		return ip;

	(void)sm_super_of(me, s); /* me->temp = 源的父状态 */

	/* (c) 源与目标同父：退出源即可 */
	if (me->temp == t) {
		(void)s(me, SM_RES_EVT(SM_SIG_EXIT));
		return ip;
	}

	/*
	 * (d) 目标就是源的父状态：退出源，且不重新进入目标。
	 *     深度返回 0，因此不执行任何 entry，但目标的 INIT 链仍会执行
	 *     （与 QP 行为一致）。
	 */
	if (me->temp == path[0]) {
		(void)s(me, SM_RES_EVT(SM_SIG_EXIT));
		return 0U;
	}

	/* (e) 其余情况：交给复杂分支 */
	path[1] = t;
	return 2U;
}

/*
 * 处理跨层转移：沿两条祖先链找 LCA，并把目标的进入路径填进 path[]。
 *
 * 进入时：path[0] = 目标，path[1] = 目标的父，path[2] = 源，
 *         me->temp = 源的父，源本身尚未退出。
 * 返回时：需要进入的层数，path[0..n-1] 不含 LCA。
 */
static size_t sm_tran_complex(struct sm_hsm *const me, sm_state_t *const path)
{
	sm_state_t s = path[2];         /* 源 */
	sm_state_t const ss = me->temp; /* 源的父状态 */
	sm_ret_t r;
	size_t ip = 1;
	size_t iq;

	/*
	 * 沿目标的祖先链向上收集进入路径。
	 * 撞到源 -> 源就是 LCA，源保持活动，直接返回。
	 * 到达顶层仍未找到 -> LCA 在源这一侧，转去退出源。
	 */
	me->temp = path[1];
	for (;;) {
		SM_ASSERT(ip < SM_PATH_LEN);
		path[ip] = me->temp;
		ip++;

		r = me->temp(me, SM_RES_EVT(SM_SIG_EMPTY));
		if (me->temp == s)
			return ip;
		if (r != SM_RET_SUPER)
			break;
	}

	/* 退出源，然后沿源的祖先链向上找进入路径上的状态（即 LCA） */
	(void)s(me, SM_RES_EVT(SM_SIG_EXIT));
	s = ss;

	for (;;) {
		iq = ip;
		while (iq > 0U) {
			iq--;
			if (s == path[iq]) {
				ip = iq; /* 不进入 LCA */
				return ip;
			}
		}

		/* s 还不是 LCA：退出它，继续向上 */
		(void)s(me, SM_RES_EVT(SM_SIG_EXIT));
		(void)s(me, SM_RES_EVT(SM_SIG_EMPTY));
		s = me->temp;
	}
}

void sm_hsm_init(struct sm_hsm *const me, sm_state_t const initial)
{
	sm_state_t path[SM_PATH_LEN];
	sm_ret_t r;
	size_t ip = 0;

	SM_ASSERT(me != NULL);
	SM_ASSERT(initial != NULL);

	if (!me || !initial)
		return;

	me->state = sm_top;
	me->busy = 0;
	me->temp = NULL;

	/*
	 * 执行最顶层的初始转移。它与普通转移同构，因此也可以带守卫和
	 * 转移动作；必须取一个转移。
	 */
	r = initial(me, SM_RES_EVT(SM_SIG_INIT));
	SM_ASSERT(r == SM_RET_TRAN);
	SM_ASSERT(me->temp != NULL);

	/* 收集从初始目标到顶层状态的祖先链，不含顶层 */
	do {
		SM_ASSERT(ip < SM_PATH_LEN);
		path[ip] = me->temp;
		ip++;
		(void)me->temp(me, SM_RES_EVT(SM_SIG_EMPTY));
	} while (me->temp != sm_top);

	sm_enter_target(me, path, ip);

	me->state = path[0];
}

void sm_hsm_dispatch(struct sm_hsm *const me, const struct sm_event *const e)
{
	sm_state_t path[SM_PATH_LEN];
	sm_state_t s;
	sm_ret_t r;
	size_t ip = SM_PATH_LEN;
	size_t iq;
	size_t n;

	SM_ASSERT(me != NULL);
	SM_ASSERT(e != NULL);

	if (!me || !e)
		return;

	SM_ASSERT(me->busy == 0U);
	if (me->busy)
		return;

	me->busy = 1U;

	/*
	 * 自最内层状态逐级向上尝试处理事件。
	 * path[] 从高索引往低索引填：索引越大，层级越深。
	 */
	me->temp = me->state;
	do {
		SM_ASSERT(ip != 0U);

		s = me->temp;
		ip--;
		path[ip] = s;

		r = s(me, e);

		if (r == SM_RET_UNHANDLED) {
			/* 守卫不满足：取父状态，继续冒泡 */
			r = s(me, SM_RES_EVT(SM_SIG_EMPTY));
		}
	} while (r == SM_RET_SUPER);

	if (r == SM_RET_IGNORED) {
		/* 一路冒泡到顶层都无人处理：事件被忽略 */
	} else if (r == SM_RET_HANDLED) {
		/* 内部转移：事件已消费，状态不变，无 entry/exit */
	} else if (r == SM_RET_TRAN || r == SM_RET_TRAN_HIST) {
		path[0] = me->temp; /* 转移目标 */
		SM_ASSERT(path[0] != NULL);

		/* 先退出源之下、直到叶子的所有状态 */
		for (iq = SM_PATH_LEN - 1U; iq > ip; iq--)
			(void)path[iq](me, SM_RES_EVT(SM_SIG_EXIT));

		path[2] = s; /* 转移源 = 处理该事件的状态 */

		n = sm_tran_simple(me, path);
		if (n > 1U)
			n = sm_tran_complex(me, path);

		sm_enter_target(me, path, n);

		me->state = path[0];
	} else {
		SM_ASSERT(0); /* 状态处理函数返回了非法值 */
	}

	me->busy = 0;
}

bool sm_hsm_is_in(struct sm_hsm *const me, sm_state_t const state)
{
	sm_state_t s = me->state;
	sm_ret_t r;

	for (;;) {
		if (s == state)
			return true;

		r = s(me, SM_RES_EVT(SM_SIG_EMPTY));
		s = me->temp;

		if (r != SM_RET_SUPER)
			return false;
	}
}

sm_state_t sm_hsm_state(struct sm_hsm const *const me)
{
	return me->state;
}
