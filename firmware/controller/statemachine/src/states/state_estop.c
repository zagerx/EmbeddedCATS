/*
 * state_estop.c — 抢占在途流程并保持急停终态
 *
 * 入口请求所有轴 Quick Stop 后断使能，实际流程由 CiA 402 层异步完成。
 * ESTOP 没有退出事件，只能重启程序，避免自动恢复引入风险。
 *
 * Author: zager
 */

#include "states.h"

/**
 * st_estop - 处理急停入口、运行命令和节拍
 * @me: 状态机内核对象
 * @e: 待处理事件
 *
 * Return: MOVE、STOP 和 TICK 被消费；其他事件上抛 ROOT。
 */
sm_ret_t st_estop(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct controller_sm *ctl = container_of(me, struct controller_sm, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		axis_quick_stop_all(ctl->axes, ctl->axis_count);
		/* 进终态：丢掉还没处理的命令 */
		cats_evtq_clear(&ctl->evtq);
		break;

	case SM_SIG_EXIT:
		break;

	case CATS_SIG_MOVE:
	case CATS_SIG_STOP:
		return SM_HANDLED(); /* 终态：命令直接丢弃 */

	case CATS_SIG_TICK:
		/* 终态吃掉节拍：不能让 ROOT 的“故障码 → FAULT”规则把
		 * ESTOP 又拉回 FAULT（进入急停前可能已经置过故障码）。 */
		return SM_HANDLED();

	default:
		return SM_SUPER(&st_root);
	}

	return SM_HANDLED();
}
