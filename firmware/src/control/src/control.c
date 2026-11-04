/*
 * control.c — 控制层装配与对外接口（**不含状态逻辑**）
 *
 * 职责：把对外 API 翻译成状态机事件，其余一概不管。
 *   状态逻辑在 states/state_*.c（一状态一文件），状态机机制在 hsm/hsm.c，
 *   状态注册在 states/states.c，内部对象在 control_internal.h。
 */

#include <control/control.h>

#include "control_internal.h"
#include "states/states.h"

static struct cats_control s_ctl = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* 把"其他线程可以看"的值发布出去。**只在控制线程里调用** */
static void publish_readonly(struct cats_control *app)
{
	int state = CATS_STATE_PREPARING;
	int i;

	for (i = 0; i < CATS_STATE_COUNT; i++) {
		if (states[i] == app->hsm.state) {
			state = i;
			break;
		}
	}
	pthread_mutex_lock(&app->lock);
	app->published_state = state;
	app->published_fault = (int)app->fault_code;
	pthread_mutex_unlock(&app->lock);
}

/* ---- 对外接口 ---- */

enum cats_result cats_control_init(struct co_master *master)
{
	uint8_t i;

	if (master == NULL) {
		return CATS_ERR_PARAM;
	}
	if (s_ctl.master != NULL) {
		return CATS_ERR_STATE; /* 只初始化一次 */
	}

	s_ctl.master = master;
	/* 轴 = 主站上已登记的节点，站点号只在装配层声明一次，这里读回来 */
	s_ctl.axis_count = master->node_count < CATS_MAX_AXES ? master->node_count : CATS_MAX_AXES;
	for (i = 0; i < s_ctl.axis_count; i++) {
		axis_init(&s_ctl.axes[i], master, master->node_id[i]);
	}

	/* 初态即准备态：hsm_init 从根一路 ENTRY 到 PREPARING，广播复位从这里发出 */
	hsm_init(&s_ctl.hsm, &st_preparing, now_ms(&s_ctl));

	publish_readonly(&s_ctl);
	return CATS_OK;
}

/*
 * 心跳：本函数只把**事实**算好存进 ctl->hb_ok（每拍一次，所有状态读同一个值）。
 *
 * “掉线就进 FAULT”的判定不在这里，而在 st_online（运行期父状态）：
 * 它是 ONLINE 子状态的公共规则，写成层次比写成状态白名单可靠——
 * 新增一个运行期状态时不需要回来改任何名单。
 * FAULT / ESTOP 挂在 root 下，自然也拿不到这条规则。
 */
static void refresh_heartbeat_fact(struct cats_control *ctl)
{
	uint8_t i;

	/* 轴数为 0 说明没装配上：没有可确认的节点，不能报健康 */
	ctl->hb_ok = ctl->axis_count > 0 ? 1 : 0;
	for (i = 0; i < ctl->axis_count; i++) {
		if (co_master_node_alive(ctl->master, ctl->axes[i].node_id) != 0) {
			ctl->hb_ok = 0;
			break;
		}
	}
}

enum cats_result cats_control_poll(void)
{
	uint32_t now;
	int estop;

	if (s_ctl.master == NULL) {
		return CATS_ERR_STATE;
	}

	/* 急停优先于一切：别的线程只能置请求标志，真正的迁移在这里做 */
	pthread_mutex_lock(&s_ctl.lock);
	estop = s_ctl.estop_req;
	s_ctl.estop_req = 0;
	pthread_mutex_unlock(&s_ctl.lock);
	if (estop) {
		hsm_transition(&s_ctl.hsm, &st_estop, now_ms(&s_ctl));
	}

	now = now_ms(&s_ctl);
	/* 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_ctl.master, now);
	refresh_heartbeat_fact(&s_ctl);

	/* 各层自管推进：轴（含 402/SDO）每拍走一步；状态机只看事实 */
	axis_poll_all(s_ctl.axes, s_ctl.axis_count, now);

	/* 一拍推一次状态机（内部处理一个事件） */
	hsm_dispatch(&s_ctl.hsm, CATS_SIG_TICK, 0, now);

	publish_readonly(&s_ctl);
	return CATS_OK;
}

void cats_control_estop(void)
{
	if (s_ctl.master == NULL) {
		return;
	}

	/* 只置请求，不在这里改状态机：任何线程都可安全调用，
	 * 真正的迁移由控制线程在下一拍（≤ 1 个节拍）执行 */
	pthread_mutex_lock(&s_ctl.lock);
	s_ctl.estop_req = 1;
	pthread_mutex_unlock(&s_ctl.lock);
}

int cats_control_move_to(uint8_t axis, int32_t um, int32_t um_s, int32_t um_s2)
{
	if (s_ctl.master == NULL || axis >= s_ctl.axis_count) {
		return -1;
	}
	if (s_ctl.axes[axis].pulses_per_rev <= 0 || s_ctl.axes[axis].um_per_rev <= 0) {
		return -1; /* 换算参数没配：不接受位置指令 */
	}

	pthread_mutex_lock(&s_ctl.lock);
	s_ctl.cmd.axis = axis;
	s_ctl.cmd.um = um;
	s_ctl.cmd.um_s = um_s;
	s_ctl.cmd.um_s2 = um_s2;
	s_ctl.cmd_kind = CATS_CMD_MOVE;
	pthread_mutex_unlock(&s_ctl.lock);
	return 0;
}

void cats_control_stop(void)
{
	if (s_ctl.master == NULL) {
		return;
	}
	pthread_mutex_lock(&s_ctl.lock);
	s_ctl.cmd_kind = CATS_CMD_STOP;
	pthread_mutex_unlock(&s_ctl.lock);
}

int cats_control_set_axis_scale(uint8_t axis, int32_t pulses_per_rev, int32_t um_per_rev)
{
	if (axis >= CATS_MAX_AXES) {
		return -1;
	}
	axis_set_scale(&s_ctl.axes[axis], pulses_per_rev, um_per_rev);
	return 0;
}

enum cats_fault cats_control_fault_code(void)
{
	enum cats_fault f;

	pthread_mutex_lock(&s_ctl.lock);
	f = (enum cats_fault)s_ctl.published_fault;
	pthread_mutex_unlock(&s_ctl.lock);
	return f;
}

enum cats_state cats_control_state(void)
{
	enum cats_state s;

	pthread_mutex_lock(&s_ctl.lock);
	s = (enum cats_state)s_ctl.published_state;
	pthread_mutex_unlock(&s_ctl.lock);
	return s;
}

uint32_t cats_control_axis_abort(uint8_t axis)
{
	if (axis >= s_ctl.axis_count) {
		return 0U;
	}
	return axis_abort_code(&s_ctl.axes[axis]);
}
