/*
 * control_api.c — 状态机侧：把对外 API 翻译成状态机操作，并把事实发布出去
 *
 * 这里只有"业务状态机"相关的东西：
 *   1. 每拍一次：算心跳事实 → 状态机走一拍 → 发布只读快照；
 *   2. 指令：move_to / stop / estop → 入队（由控制线程消费）；
 *   3. 查询：状态、故障码、abort 码、各轴样本。
 *
 * 装配与三个推进步骤在 controller.c（组合根）。
 */

#include "controller_internal.h"

/*
 * 把"其他线程可以看"的值发布出去。**只在控制线程里调用**
 */
void cats_control_publish(void)
{
	struct cats_control *app = control_ctx();
	int state;
	struct cats_axis_sample samples[CATS_MAX_AXES];
	uint8_t i;

	if (app->stm == NULL) {
		return;
	}
	state = (int)stm_state(app->stm);
	/* 换算在锁外做（axis_sample 只读本线程写的字段，不做 I/O） */
	for (i = 0; i < CATS_MAX_AXES; i++) {
		struct axis_sample s;

		axis_sample(&app->axes[i], &s);
		samples[i].position_mm = s.position_mm;
		samples[i].velocity_mm_s = s.velocity_mm_s;
		samples[i].statusword = s.statusword;
		samples[i].error_code = s.error_code;
		samples[i].valid = s.valid;
	}
	pthread_mutex_lock(&app->lock);
	app->published_state = state;
	app->published_fault = (int)stm_fault(app->stm);
	for (i = 0; i < CATS_MAX_AXES; i++) {
		app->published_axes[i] = samples[i];
	}
	pthread_mutex_unlock(&app->lock);
}

/*
 * 心跳：本函数只把**事实**算好（每拍一次，所有状态读同一个值）。
 *
 * “掉线就进 FAULT”的判定不在这里，而在 st_online（运行期父状态）：
 * 它是 ONLINE 子状态的公共规则，写成层次比写成状态白名单可靠——
 * 新增一个运行期状态时不需要回来改任何名单。
 * FAULT / ESTOP 挂在 root 下，自然也拿不到这条规则。
 */
static uint8_t heartbeat_fact(const struct cats_control *ctl)
{
	uint8_t ok = ctl->axis_count > 0 ? 1u : 0u;
	uint8_t i;

	/* 轴数为 0 说明没装配上：没有可确认的节点，不能报健康 */
	for (i = 0; i < ctl->axis_count; i++) {
		if (master_node_alive(ctl->master, ctl->axes[i].node_id) != 0) {
			ok = 0u;
			break;
		}
	}
	return ok;
}

/* ---- 每拍一次：状态机走一拍 + 发布 ---- */

enum cats_result cats_control_poll(void)
{
	struct cats_control *ctl = control_ctx();
	uint32_t now;

	if (ctl->master == NULL) {
		return CATS_ERR_STATE;
	}
	now = now_ms(ctl);

	/* 1. 收帧在 I/O 线程、402/SDO 推进在服务线程：本拍只读事实 */
	/* 2. 事实交给业务状态机：它自己消费事件、走一拍节拍 */
	stm_poll(ctl->stm, heartbeat_fact(ctl), now);

	cats_control_publish();
	return CATS_OK;
}

/* ---- 指令：入队，由控制线程在下一拍消费 ---- */

void cats_control_estop(void)
{
	struct cats_control *ctl = control_ctx();

	if (ctl->master == NULL) {
		return;
	}
	/* 插到队头：优先于已排队的所有命令 */
	(void)stm_command(ctl->stm, STM_CMD_ESTOP, NULL);
}

enum cats_result cats_control_move_to(uint8_t axis, float mm, float mm_s, float mm_s2)
{
	struct cats_control *ctl = control_ctx();
	struct stm_move mv;

	if (ctl->master == NULL || axis >= ctl->axis_count) {
		return CATS_ERR_PARAM;
	}
	/* NaN 进到整数换算就是未定义行为：在入口就拦住（±inf 由轴层饱和处理） */
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return CATS_ERR_PARAM;
	}

	mv.axis = axis;
	mv.mm = mm;
	mv.mm_s = mm_s;
	mv.mm_s2 = mm_s2;
	/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
	return stm_command(ctl->stm, STM_CMD_MOVE, &mv);
}

void cats_control_stop(void)
{
	struct cats_control *ctl = control_ctx();

	if (ctl->master == NULL) {
		return;
	}
	/* 停止也插队：排在已有序命令前面 */
	(void)stm_command(ctl->stm, STM_CMD_STOP, NULL);
}

/* ---- 查询：只读发布值 / 轴事实 ---- */

enum cats_fault cats_control_fault_code(void)
{
	struct cats_control *ctl = control_ctx();
	enum cats_fault f;

	pthread_mutex_lock(&ctl->lock);
	f = (enum cats_fault)ctl->published_fault;
	pthread_mutex_unlock(&ctl->lock);
	return f;
}

enum cats_state cats_control_state(void)
{
	struct cats_control *ctl = control_ctx();
	enum cats_state s;

	pthread_mutex_lock(&ctl->lock);
	s = (enum cats_state)ctl->published_state;
	pthread_mutex_unlock(&ctl->lock);
	return s;
}

uint32_t cats_control_axis_abort(uint8_t axis)
{
	struct cats_control *ctl = control_ctx();

	if (axis >= ctl->axis_count) {
		return 0U;
	}
	return axis_abort_code(&ctl->axes[axis]);
}

enum cats_result cats_control_axis_sample(uint8_t axis, struct cats_axis_sample *out)
{
	struct cats_control *ctl = control_ctx();

	if (out == NULL || axis >= CATS_MAX_AXES) {
		return CATS_ERR_PARAM;
	}
	pthread_mutex_lock(&ctl->lock);
	*out = ctl->published_axes[axis];
	pthread_mutex_unlock(&ctl->lock);
	return CATS_OK;
}

/* 名字表：状态机侧定义，界面与日志共用同一份 */
const char *cats_control_state_name(enum cats_state s)
{
	return stm_state_name(s);
}

const char *cats_control_fault_name(enum cats_fault f)
{
	return stm_fault_name(f);
}
