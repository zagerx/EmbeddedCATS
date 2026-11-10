/*
 * sm_hsm.c — 实现 QP 风格的层次状态机内核
 *
 * 负责事件冒泡、基于最近公共祖先的退出与进入链，以及初始转移下钻。
 * 算法独立编写，不包含事件队列、动态内存、时间事件或业务依赖。
 *
 * Author: zager
 */

#include <hsm/sm_hsm.h>

#include <stdio.h>

/* path[] 长度：最深处是叶子，最浅处是顶层状态 */
#define SM_PATH_LEN (SM_CFG_MAX_NEST_DEPTH + 1)

static const struct sm_event sm_res_evt[4];

#define SM_RES_EVT(sig_) (&sm_res_evt[(sig_)])

/*
 * 断言失败默认只记录证据并返回，避免挂住 Linux 控制线程。
 * 可定义 SM_CFG_NO_ASSERT 关闭，或自行定义 SM_ASSERT(cond) 接管。
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

/* 通过 SM_SIG_EMPTY 查询父状态，结果保存在 me->temp */
static sm_state_t sm_super_of(struct sm_hsm *const me, sm_state_t const s);

/* 执行目标进入链，并沿初始转移钻到最内层状态 */
static void sm_enter_target(struct sm_hsm *const me, sm_state_t *const path, size_t depth);

/* 处理可直接判定的转移，复杂情况交给 sm_tran_complex */
static size_t sm_tran_simple(struct sm_hsm *const me, sm_state_t *const path);

/* 沿源与目标的祖先链寻找 LCA，并完成跨层退出与进入路径计算 */
static size_t sm_tran_complex(struct sm_hsm *const me, sm_state_t *const path);

#ifndef SM_CFG_NO_ASSERT
/**
 * sm_hsm_assert_failed - 记录状态机断言失败位置
 * @file: 源文件名
 * @line: 源文件行号
 *
 * 默认实现只写 stderr，不阻塞控制线程。
 */
void sm_hsm_assert_failed(const char *file, unsigned int line)
{
	(void)fprintf(stderr, "sm_hsm: assert failed at %s:%u\n", file, line);
}
#endif

/**
 * sm_top - 提供所有用户状态的顶层状态
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: 始终返回 SM_RET_IGNORED，表示事件冒泡结束。
 */
sm_ret_t sm_top(struct sm_hsm *const me, const struct sm_event *const e)
{
	(void)me;
	(void)e;

	return SM_RET_IGNORED;
}

/**
 * sm_hsm_init - 初始化状态机并执行完整初始进入链
 * @me: 状态机内核对象；NULL 安全
 * @initial: 顶层初始转移函数；NULL 安全
 *
 * initial 必须返回 SM_TRAN，目标状态沿 SM_SIG_INIT 链下钻。
 */
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

/**
 * sm_hsm_dispatch - 同步分发一个事件直到处理完成
 * @me: 状态机内核对象；NULL 安全
 * @e: 待处理事件；NULL 安全
 *
 * 事件从当前叶子向父状态冒泡；本函数不可重入。
 */
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

/**
 * sm_hsm_is_in - 检查当前活动配置是否包含指定状态
 * @me: 状态机内核对象
 * @state: 待查询状态函数
 *
 * Return: true 当前叶子或其祖先包含 @state；false 不包含。
 */
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

/**
 * sm_hsm_state - 获取当前活动配置的最内层状态
 * @me: 状态机内核对象
 *
 * Return: 当前叶子状态函数指针。
 */
sm_state_t sm_hsm_state(struct sm_hsm const *const me)
{
	return me->state;
}

static sm_state_t sm_super_of(struct sm_hsm *const me, sm_state_t const s)
{
	(void)s(me, SM_RES_EVT(SM_SIG_EMPTY));

	return me->temp;
}

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

static const struct sm_event sm_res_evt[4] = {
	{SM_SIG_EMPTY},
	{SM_SIG_ENTRY},
	{SM_SIG_EXIT},
	{SM_SIG_INIT},
};
