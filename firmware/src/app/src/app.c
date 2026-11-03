/*
 * app.c — 应用层装配与对外接口（**不含状态逻辑**）
 *
 * 职责：把对外 API 翻译成状态机事件，其余一概不管。
 *   状态逻辑在 states/state_*.c（一状态一文件），状态机机制在 hsm/hsm.c，
 *   状态注册在 states/states.c，内部对象在 app_internal.h。
 */

#include <app/app.h>

#include "app_internal.h"
#include "states/states.h"

static struct cats_app s_app;

/* ---- 对外接口 ---- */

enum cats_app_result cats_app_init(struct co_master *master)
{
	uint8_t i;

	if (master == NULL) {
		return CATS_APP_ERR_PARAM;
	}
	if (s_app.master != NULL) {
		return CATS_APP_ERR_STATE; /* 只初始化一次 */
	}

	s_app.master = master;
	/* 轴 = 主站上已登记的节点，站点号只在装配层声明一次，这里读回来 */
	s_app.axis_count =
		master->node_count < CATS_APP_MAX_AXES ? master->node_count : CATS_APP_MAX_AXES;
	for (i = 0; i < s_app.axis_count; i++) {
		s_app.axes[i].node_id = master->node_id[i];
	}

	/* 初态即准备态：hsm_init 从根一路 ENTRY 到 PREPARING，广播复位从这里发出 */
	hsm_init(&s_app.hsm, &st_preparing, now_ms(&s_app));
	return CATS_APP_OK;
}

/*
 * 心跳的唯一入口：先把事实算好存进 app->hb_ok（各状态都读同一个值），
 * 再在“受心跳约束的状态”里执行故障迁移。
 *
 * 受约束的只有已接管驱动器的状态（RUNNING / STOPPING）：
 *   PREPARING 有自己的静默期与超时（故障码也不同）；
 *   ESTOP 已在最安全处且没有出口，再迁一次只会把原因覆盖掉；
 *   FAULT 本身已经是故障。
 */
static void handle_heartbeat(struct cats_app *app, uint32_t now)
{
	uint8_t i;

	/* 轴数为 0 说明没装配上：没有可确认的节点，不能报健康 */
	app->hb_ok = app->axis_count > 0 ? 1 : 0;
	for (i = 0; i < app->axis_count; i++) {
		if (co_master_node_alive(app->master, app->axes[i].node_id) != 0) {
			app->hb_ok = 0;
			break;
		}
	}

	if (app->hb_ok) {
		return;
	}
	if (app->hsm.state != &st_running && app->hsm.state != &st_stopping) {
		return;
	}

	app->fault_code = CATS_FAULT_HEARTBEAT_LOST;
	hsm_transition(&app->hsm, &st_fault, now);
}

enum cats_app_result cats_app_poll(void)
{
	uint32_t now;

	if (s_app.master == NULL) {
		return CATS_APP_ERR_STATE;
	}

	now = now_ms(&s_app);
	/* 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_app.master, now);
	handle_heartbeat(&s_app, now);

	/* 一拍推一次状态机（内部处理一个事件） */
	hsm_dispatch(&s_app.hsm, CATS_SIG_TICK, 0, now);
	return CATS_APP_OK;
}

void cats_app_estop(void)
{
	if (s_app.master == NULL) {
		return;
	}

	/* 急停不走队列：任何状态下都立即切过去，ENTRY 里做安全动作 */
	hsm_transition(&s_app.hsm, &st_estop, now_ms(&s_app));
}

enum cats_app_fault cats_app_fault_code(void)
{
	return s_app.fault_code;
}

enum cats_run_state cats_app_state(void)
{
	for (int i = 0; i < CATS_RUN_STATE_COUNT; i++) {
		if (states[i] == s_app.hsm.state) {
			return (enum cats_run_state)i;
		}
	}
	return CATS_RUN_PREPARING;
}
