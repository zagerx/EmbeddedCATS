/* 保持中：已使能且无运动在途；MOVE 转 MOVING，STOP 直接忽略。 */

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
