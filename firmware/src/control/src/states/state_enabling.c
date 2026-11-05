/*
 * state_enabling.c — 使能中
 *
 * 进入：NMT 启动（0x01，手册 6.1.4 步骤 2）+ 请求各轴使能（并发，互不阻塞）。
 * 每拍：只看两个事实——都使能上了 → HOLDING；有轴失败 / 超时 → 置故障码，
 *       由 ROOT 统一迁 FAULT。
 *
 * 本状态不推进任何步骤：轴的推进在 axis_poll_all() 里（control.c 每拍调一次），
 * 这里只读 axis_all_enabled() / axis_any_failed() 的结论。
 */

#include "states.h"

#define ENABLE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还没全部使能上就判失败 */

sm_ret_t st_enabling(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cats_control *ctl = container_of(me, struct cats_control, hsm);

	switch (e->sig) {
	case SM_SIG_ENTRY:
		/* 0x01 = Start remote node，站号 0 = 全部。手册例子先 NMT 启动再发 SDO。
		 * 返回值不判：NMT 是广播、没有应答，发不出去或从站没收到，都由后面
		 * “心跳上来 + 使能成功”来判定，判不出来也会落 ENABLE_TIMEOUT。 */
		(void)co_master_nmt(ctl->master, 0, 0x01);
		(void)axis_enable_all(ctl->axes, ctl->axis_count);
		break;

	case SM_SIG_EXIT:
		break; /* 失败的那一步已经停下，正常完成时也不再发帧 */

	case CATS_SIG_TICK: {
		uint32_t elapsed = now_ms(ctl) - ctl->state_since_ms;

		if (axis_any_failed(ctl->axes, ctl->axis_count)) {
			ctl->fault_code = CATS_FAULT_ENABLE_FAILED;
			return SM_SUPER(&st_online); /* 交 ROOT 迁 FAULT */
		}
		if (axis_all_enabled(ctl->axes, ctl->axis_count)) {
			return SM_TRAN(&st_holding);
		}
		if (elapsed > ENABLE_TIMEOUT_MS) {
			ctl->fault_code = CATS_FAULT_ENABLE_TIMEOUT;
		}
		return SM_SUPER(&st_online);
	}

	default:
		return SM_SUPER(&st_online);
	}

	return SM_HANDLED();
}
