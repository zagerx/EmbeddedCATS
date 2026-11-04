/*
 * state_enabling.c — 使能中
 *
 * 进入：NMT 启动（0x01，手册 6.1.4 步骤 2）+ 请求各轴使能（并发，互不阻塞）；
 * 每拍：只看两个事实——都使能上了 → RUNNING；有轴失败 / 超时 → FAULT；
 * 退出：无动作（失败时轴自己已经停在失败步骤，不再发帧）。
 *
 * 本状态不推进任何步骤：轴的推进在 axis_group_poll() 里（cats_app_poll 每拍
 * 调一次），这里只读 axis_group_all_enabled() / any_failed() 的结论。
 *
 * 心跳也约束本状态：使能到一半掉线，不能继续往下走。这个判断不在本文件，
 * 在 app.c 的 handle_heartbeat()（RUNNING / STOPPING / ENABLING 一视同仁）。
 */

#include "states.h"

#define ENABLE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还没全部使能上就判失败 */

static int st_enabling_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_enabling = {
	.handler = st_enabling_h,
	.super = &st_operational,
	.name = "ENABLING",
};

static int st_enabling_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* 0x01 = Start remote node，站号 0 = 全部。手册例子先 NMT 启动再发 SDO */
		(void)co_master_nmt(app->master, 0, 0x01);
		(void)axis_group_enable(&app->group);
		break;

	case HSM_SIG_EXIT:
		/* 无出口动作：失败的那一步已经停下，正常完成时也不再发帧 */
		break;

	case CATS_SIG_TICK: {
		uint32_t elapsed = now_ms(app) - me->entered_ms;

		if (axis_group_any_failed(&app->group)) {
			app->fault_code = CATS_FAULT_ENABLE_FAILED;
			HSM_TRAN(&st_fault);
		}
		if (axis_group_all_enabled(&app->group)) {
			HSM_TRAN(&st_ready);
		}
		if (elapsed > ENABLE_TIMEOUT_MS) {
			app->fault_code = CATS_FAULT_ENABLE_TIMEOUT;
			HSM_TRAN(&st_fault);
		}
		break;
	}

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
