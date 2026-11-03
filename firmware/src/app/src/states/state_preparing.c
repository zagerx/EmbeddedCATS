/*
 * state_preparing.c — 准备中
 *
 * 进入：广播 Reset Communication；
 * 每拍：静默期后心跳正常 → ENABLING；超时 → FAULT；
 * 退出：无动作。
 *
 * 静默期：发完 0x82 后等这么久再判心跳，让驱动器有机会先处理复位并回 boot-up
 * （boot-up 会把消费者槽位置回 UNKNOWN，之后必须收到新心跳才算 ACTIVE）。
 *
 * 注意：本值（100 ms）小于 OD 0x1016 的超时值（750 ms），所以它并不保证
 * "复位前那一帧残留的 ACTIVE 已经过期"。若驱动器回 boot-up 晚于本值，
 * 判定会用上残留的 ACTIVE 而提前进 RUNNING，随后槽位变 UNKNOWN，
 * 可能在 750 ms 内被判成心跳丢失。驱动器响应足够快时不会走到这条路。
 *
 * 心跳的采集不在这里：cats_app_poll() 每拍算好 app->hb_ok，本状态只读。
 * 时间用 me->entered_ms 手算（内核不提供定时器）。
 */

#include "states.h"

#define PREPARE_SETTLE_MS 100u /* 发完 0x82 先静默这么久再判心跳 */
#define PREPARE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还等不到心跳就判失败 */

static int st_preparing_h(struct hsm *me, const struct hsm_event *e);
const struct hsm_state st_preparing = {
	.handler = st_preparing_h,
	.super = &st_operational,
	.name = "PREPARING",
};

static int st_preparing_h(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* 0x82 = Reset Communication，站号 0 = 广播；只复位通信，
		 * 不改动对象字典里的配置 */
		(void)co_master_nmt(app->master, 0, 0x82);
		break;

	case HSM_SIG_EXIT:
		/* 无出口动作 */
		break;

	case CATS_SIG_TICK: {
		uint32_t elapsed = now_ms(app) - me->entered_ms;

		if (elapsed < PREPARE_SETTLE_MS) {
			break; /* 静默期：此时的心跳状态还是复位前的旧数据 */
		}
		if (app->hb_ok) {
			HSM_TRAN(&st_enabling);
		}
		if (elapsed > PREPARE_TIMEOUT_MS) {
			app->fault_code = CATS_FAULT_BOOTUP_TIMEOUT;
			HSM_TRAN(&st_fault);
		}
		break;
	}

	default:
		return HSM_RET_SUPER;
	}

	return HSM_RET_HANDLED;
}
