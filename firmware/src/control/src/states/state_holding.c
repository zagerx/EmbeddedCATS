/*
 * state_holding.c — 保持中：已使能、没有运动在途、可以接受指令
 *
 * 进入：清故障码；丢掉上一个运动的结论（否则会被当成本次运动的结果）。
 * 每拍：取指令槽 ——
 *         位置指令 → 带着参数进 MOVING；
 *         停止指令 → 丢弃（本来就停在原位保持力矩，没有可做的事）。
 *       处理完返回 SUPER，让 ONLINE 补做故障/心跳判定。
 *
 * 名字不叫 READY：READY 在 CiA 402 里是 "Ready to switch on"（另一个枚举里
 * 就有 CIA402_READY），这里是业务上的"保持位置等待指令"。
 */

#include "states.h"

static int st_holding_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_holding = {
	.handler = st_holding_h,
	.super = &st_online,
	.name = "HOLDING",
};

static int st_holding_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY: {
		uint8_t i;

		ctl->fault_code = CATS_FAULT_NONE;
		for (i = 0; i < ctl->axis_count; i++) {
			(void)axis_take_motion(&ctl->axes[i]); /* 丢弃旧结论 */
		}
		break;
	}

	case CATS_SIG_TICK: {
		enum cats_cmd_kind cmd = cats_take_cmd(ctl, &ctl->move);

		if (cmd == CATS_CMD_MOVE) {
			/* 参数已在临界区里复制到 ctl->move，指令槽再变也不影响本次 */
			HSM_TRAN(&st_moving);
		}
		/* CATS_CMD_STOP / NONE：丢弃 */
		return HSM_RET_SUPER; /* 公共判定交 ONLINE */
	}

	case HSM_SIG_EXIT:
		break;

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
