/*
 * state_starting.c — 启动中
 *
 * ENTRY：发一次 NMT，让所有节点进 Pre-op（只发一次，不重复刷总线）；
 * TICK ：分段等节点上线，齐了进 RUNNING，超时进 FAULT；
 * STOP ：随时可取消。
 */

#include "states.h"

#define STARTUP_TIMEOUT_MS 2000u /* 启动超时：等不到节点上线就报 FAULT */

int st_starting(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);
	uint32_t now;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		/* TODO(下一拍)：co_master_nmt(app->master, 0, 0x80) 进 Pre-op */
		HSM_HANDLED();

	case HSM_SIG_TICK:
		now = now_ms(app);
		if (now - me->entered_ms > STARTUP_TIMEOUT_MS) {
			app->fault_code = 1; /* TODO: 换成正式故障码枚举 */
			HSM_TRAN(st_fault);
		}
		/* TODO(下一拍)：收各节点 boot-up/心跳 → NMT start → 402 Ready，
		 * 全部完成后 HSM_TRAN(st_running) */
		HSM_HANDLED();

	case HSM_SIG_STOP:
		HSM_TRAN(st_stopping);

	default:
		HSM_IGNORED();
	}
}
