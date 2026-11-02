/*
 * state_running.c — 运行中
 *
 * ENTRY：清掉启动期间的重启标记，之后的 boot-up 才算异常；
 * TICK ：节点重启 → FAULT；节点掉线 → FAULT；
 * EXIT：不管因何离开运行态，都停 SYNC（出口动作的天然位置）；
 * STOP：进 STOPPING。
 *
 * 判定阈值放在应用层，内核对“多久算掉线”一无所知。
 */

#include "states.h"

/* 心跳超时 = 1.5 × 500 ms 生产周期（CiA 301 的超时口径） */
#define HEARTBEAT_TIMEOUT_MS 750u

int st_running(struct hsm *me, const struct hsm_event *e)
{
	struct cats_app *app = container_of(me, struct cats_app, hsm);
	uint32_t now;

	switch (e->sig) {
	case HSM_SIG_ENTRY:
		co_master_clear_rebooted(app->master);
		app->fault_code = CATS_FAULT_NONE;
		HSM_HANDLED();

	case HSM_SIG_TICK:
		now = now_ms(app);
		/* 运行中收到 boot-up = 驱动器掉电重启，状态已与我们的假设不一致 */
		if (co_master_any_rebooted(app->master)) {
			app->fault_code = CATS_FAULT_NODE_REBOOTED;
			HSM_TRAN(st_fault);
		}
		/* 任一个节点掉线就整体 FAULT：安全优先 */
		if (!co_master_all_alive(app->master, now, HEARTBEAT_TIMEOUT_MS)) {
			app->fault_code = CATS_FAULT_HEARTBEAT_LOST;
			HSM_TRAN(st_fault);
		}
		/* TODO(下一拍)：发 SYNC、检查 402 状态字 */
		HSM_HANDLED();

	case HSM_SIG_EXIT:
		/* TODO(下一拍)：停 SYNC */
		HSM_HANDLED();

	case HSM_SIG_STOP:
		HSM_TRAN(st_stopping);

	default:
		HSM_IGNORED();
	}
}
