/*
 * state_estop.c — 急停态
 *
 * ENTRY：各轴 quick stop → 断使能（两段式，由 402 层异步走完）。
 *        急停是安全动作，**抢占**任何在途过程（运动、停车、使能都不排队）。
 * 之后：停住不动。没有出口事件，只能重启程序——自动恢复会把风险带回来。
 *
 * 进本状态不走"允许/拒绝"判断：CATS_SIG_ESTOP 由 st_root 统一接管，
 * 而 cats_control_poll() 里急停请求优先于一切。
 */

#include "states.h"

sm_ret_t st_estop(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

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
