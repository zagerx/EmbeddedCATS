/*
 * state_preparing.c — 准备中
 *
 * 进入：广播 Reset Communication；
 * 每拍：静默期后所有节点心跳正常 → RUNNING；超时 → FAULT；
 * 退出：无动作。
 *
 * 静默期的原因：0x82 之前刚收到的那一帧心跳会让消费者的槽位还停留在 ACTIVE，
 * 那一刻去查“都活着吗”会立刻成立，等于没等。该帧的尾巴在 1.5 × 500 ms 后
 * 才过期，所以静默期必须大于 750 ms，这里取 1000 ms 留余量。
 *
 * 时间用 me->entered_ms 手算（内核不提供定时器）。
 */

#include "states.h"

#define PREPARE_SETTLE_MS 1000u /* 发完 0x82 先静默这么久再判心跳 */
#define PREPARE_TIMEOUT_MS 3000u /* 从进入本状态算起，这么久还等不到心跳就判失败 */

static int st_preparing_h(struct hsm *me, const struct hsm_event *e);
HSM_STATE_DEF(st_preparing, &st_operational, "PREPARING");

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
		if (all_axes_alive(app)) {
			HSM_TRAN(&st_running);
		}
		if (elapsed > PREPARE_TIMEOUT_MS) {
			app->fault_code = CATS_FAULT_BOOTUP_TIMEOUT;
			HSM_TRAN(&st_fault);
		}
		break;
	}

	default:
		HSM_SUPER();
	}

	return HSM_RET_HANDLED;
}
