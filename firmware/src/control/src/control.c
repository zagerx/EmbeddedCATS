/*
 * control.c — 控制层装配与对外接口（**不含状态逻辑**）
 *
 * 职责：把对外 API 翻译成状态机事件，其余一概不管。
 *   状态逻辑在 states/state_*.c（一状态一文件），状态机机制在 hsm/sm_hsm.c，
 *   状态注册在 states/states.c，初始转移也在那里，内部对象在 control_internal.h。
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
		if (states[i] == sm_hsm_state(&app->hsm)) {
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

	/* 初态：initial() 返回 SM_TRAN(&st_preparing)，内核把 sm_top → ROOT → PREPARING
	 * 的 ENTRY 链走一遍，广播复位从 PREPARING 的 ENTRY 里发出 */
	cats_evtq_init(&s_ctl.evtq);
	sm_hsm_init(&s_ctl.hsm, &initial);
	s_ctl.state_since_ms = now_ms(&s_ctl);

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
	static const struct sm_event evt_tick = {CATS_SIG_TICK};
	struct cats_evt evt;
	sm_state_t before;
	uint32_t now;
	unsigned n;

	if (s_ctl.master == NULL) {
		return CATS_ERR_STATE;
	}

	now = now_ms(&s_ctl);

	/*
	 * 1. 先消费外部事件：停止/急停插在队头，所以这一步永远先于本拍 I/O。
	 *    每拍最多 CATS_EVTQ_DRAIN_MAX 条，剩下的留给下一拍（不会丢）。
	 *    dispatch 一律在**锁外**做：内核会间接调 axis_*，不能占着队列锁。
	 */
	for (n = 0u; n < CATS_EVTQ_DRAIN_MAX; n++) {
		if (!cats_evtq_pop(&s_ctl.evtq, &evt)) {
			break;
		}
		before = sm_hsm_state(&s_ctl.hsm);
		sm_hsm_dispatch(&s_ctl.hsm, &evt.super);
		if (sm_hsm_state(&s_ctl.hsm) != before) {
			s_ctl.state_since_ms = now;
		}
	}

	/* 2. 收帧与运行状态无关：心跳要在任何状态下都记着，这才叫“一直检测” */
	(void)co_master_poll(s_ctl.master, now);
	refresh_heartbeat_fact(&s_ctl);

	/* 3. 各层自管推进：轴（含 402/SDO）每拍走一步；状态机只看事实 */
	axis_poll_all(s_ctl.axes, s_ctl.axis_count, now);

	/* 4. 一拍一次节拍事件 */
	before = sm_hsm_state(&s_ctl.hsm);
	sm_hsm_dispatch(&s_ctl.hsm, &evt_tick);
	if (sm_hsm_state(&s_ctl.hsm) != before) {
		s_ctl.state_since_ms = now; /* 状态变了：重新计“进入时刻” */
	}

	publish_readonly(&s_ctl);
	return CATS_OK;
}

void cats_control_estop(void)
{
	struct cats_evt e = {0};

	if (s_ctl.master == NULL) {
		return;
	}

	/* 插到队头：优先于已排队的所有命令；队列满时挤掉最旧的那条，永不失败 */
	e.super.sig = CATS_SIG_ESTOP;
	cats_evtq_push_front(&s_ctl.evtq, &e);
}

enum cats_result cats_control_move_to(uint8_t axis, float mm, float mm_s, float mm_s2)
{
	struct cats_evt e = {0};

	if (s_ctl.master == NULL || axis >= s_ctl.axis_count) {
		return CATS_ERR_PARAM;
	}
	if (s_ctl.axes[axis].pulses_per_rev <= 0 || s_ctl.axes[axis].mm_per_rev <= 0.0f) {
		return CATS_ERR_PARAM; /* 换算参数没配：不接受位置指令 */
	}
	/* NaN 进到整数换算就是未定义行为：在入口就拦住（±inf 由轴层饱和处理） */
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return CATS_ERR_PARAM;
	}

	e.super.sig = CATS_SIG_MOVE;
	e.u.move.axis = axis;
	e.u.move.mm = mm;
	e.u.move.mm_s = mm_s;
	e.u.move.mm_s2 = mm_s2;

	/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
	return cats_evtq_push(&s_ctl.evtq, &e) ? CATS_OK : CATS_ERR_BUSY;
}

void cats_control_stop(void)
{
	struct cats_evt e = {0};

	if (s_ctl.master == NULL) {
		return;
	}

	/* 停止也插队：排在已有序命令前面，永不失败 */
	e.super.sig = CATS_SIG_STOP;
	cats_evtq_push_front(&s_ctl.evtq, &e);
}

int cats_control_set_axis_scale(uint8_t axis, int32_t pulses_per_rev, float mm_per_rev)
{
	if (axis >= CATS_MAX_AXES) {
		return -1;
	}
	axis_set_scale(&s_ctl.axes[axis], pulses_per_rev, mm_per_rev);
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
