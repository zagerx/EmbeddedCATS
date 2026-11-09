/*
 * state_holding.c — 保持中：已使能、没有运动在途、可以接受指令
 *
 * 进入：清故障码；丢掉上一个运动的结论（否则会被当成本次运动的结果）。
 * 每拍：取指令槽 ——
 *         位置指令 → 带着参数进 MOVING；
 *         停止指令 → 丢弃（本来就停在原位保持力矩，没有可做的事）。
 *       处理完返回 SUPER，让 ONLINE 补做心跳判定、ROOT 补做故障码判定。
 *
 * 名字不叫 READY：READY 在 CiA 402 里是 "Ready to switch on"（另一个枚举里
 * 就有 CIA402_READY），这里是业务上的"保持位置等待指令"。
 */

#include "states.h"

sm_ret_t st_holding(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct stm *ctl = container_of(me, struct stm, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY: {
		uint8_t i;

		ctl->fault_code = CATS_FAULT_NONE;
		for (i = 0; i < ctl->axis_count; i++) {
			(void)axis_take_motion(&ctl->axes[i]); /* 丢弃旧结论 */
		}
		break;
	}

	case CATS_SIG_MOVE:
		/* 事件自带参数：拷贝到 ctl->move，供 MOVING 的 ENTRY 下发 */
		ctl->move = SM_EVT_CAST(struct cats_evt, e)->u.move;
		return SM_TRAN(&st_moving);

	case CATS_SIG_STOP:
		return SM_HANDLED(); /* 已经在保持位置，没有可做的事 */

	case CATS_SIG_TICK:
		return SM_SUPER(&st_online); /* 没有命令：让 ONLINE 补做心跳判定 */

	case SM_SIG_EXIT:
		break;

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
