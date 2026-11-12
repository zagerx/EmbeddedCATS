/*
 * control_api.c — 将控制 API 翻译成状态机操作并发布只读快照
 *
 * 外部指令经事件队列进入控制线程，查询接口读取发布值或轴事实。
 * 装配以及 I/O、控制、服务推进位于 controller.c。
 *
 * Author: zager
 */

#include "controller_internal.h"

/**
 * controller_publish - 发布整机状态、故障和各轴反馈快照
 *
 * 只能由控制线程调用；轴单位换算在发布锁外完成。
 */
void controller_publish(void)
{
	struct controller *app = control_ctx();
	int state;
	struct controller_axis_sample samples[CONTROLLER_MAX_AXES];
	uint8_t i;

	if (app->master == NULL) {
		return;
	}

	/* 将当前 HSM 叶子映射为对外状态；未注册时沿用 PREPARING。 */
	state = CONTROLLER_STATE_PREPARING;
	for (i = 0u; i < CONTROLLER_STATE_COUNT; i++) {
		if (states[i] == sm_hsm_state(&app->hsm)) {
			state = (int)i;
			break;
		}
	}

	/* 换算在锁外做（axis_sample 只读本线程写的字段，不做 I/O） */
	for (i = 0; i < CONTROLLER_MAX_AXES; i++) {
		struct axis_sample s;

		axis_sample(&app->axes[i], &s);
		samples[i].position_mm = s.position_mm;
		samples[i].velocity_mm_s = s.velocity_mm_s;
		samples[i].statusword = s.statusword;
		samples[i].error_code = s.error_code;
	}
	pthread_mutex_lock(&app->lock);
	app->published_state = state;
	app->published_fault = (int)app->fault_code;
	for (i = 0; i < CONTROLLER_MAX_AXES; i++) {
		app->published_axes[i] = samples[i];
	}
	pthread_mutex_unlock(&app->lock);
}

/**
 * controller_estop - 请求所有轴急停
 *
 * 任意线程均可调用；事件插入队头，ESTOP 只能重启退出。
 */
void controller_estop(void)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_ESTOP}};

	if (ctl->master == NULL) {
		return;
	}
	/* 插到队头：优先于已排队的所有命令 */
	controller_evtq_push_front(&ctl->evtq, &evt);
}

/**
 * controller_move - 同时提交一批绝对位置指令
 * @reqs: 指令数组，每条含轴号与目标位置/速度/加速度
 * @count: 条数（1..CONTROLLER_MAX_AXES）
 *
 * 一批轴一起启动，全部到位才回 HOLDING。
 *
 * Return: CONTROLLER_OK 已入队；CONTROLLER_ERR_BUSY 队列满；CONTROLLER_ERR_PARAM 参数或状态无效。
 */
enum controller_result controller_move(const struct controller_move_req *reqs, uint8_t count)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_MOVE}};
	uint8_t i;

	if (ctl->master == NULL || reqs == NULL || count == 0u || count > CONTROLLER_MAX_AXES) {
		return CONTROLLER_ERR_PARAM;
	}
	evt.u.move.count = count;
	for (i = 0u; i < count; i++) {
		const struct controller_move_req *m = &reqs[i];

		if (m->axis >= ctl->axis_count) {
			return CONTROLLER_ERR_PARAM;
		}
		/* NaN 进到整数换算就是未定义行为：入口就拦住（±inf 由轴层饱和） */
		if (m->mm != m->mm || m->mm_s != m->mm_s || m->mm_s2 != m->mm_s2) {
			return CONTROLLER_ERR_PARAM;
		}
		evt.u.move.axis[i] = reqs[i];
	}
	/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
	return controller_evtq_push(&ctl->evtq, &evt) ? CONTROLLER_OK : CONTROLLER_ERR_BUSY;
}

/**
 * controller_stop - 请求减速停并保持使能
 *
 * 任意线程均可调用；停止事件插入队头，由控制线程消费。
 */
void controller_stop(void)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_STOP}};

	if (ctl->master == NULL) {
		return;
	}
	/* 停止也插队：排在已有序命令前面 */
	controller_evtq_push_front(&ctl->evtq, &evt);
}

/**
 * controller_fault_code - 读取已发布的整机故障码
 *
 * Return: 最近一次发布的故障码。
 */
enum controller_fault controller_fault_code(void)
{
	struct controller *ctl = control_ctx();
	enum controller_fault f;

	pthread_mutex_lock(&ctl->lock);
	f = (enum controller_fault)ctl->published_fault;
	pthread_mutex_unlock(&ctl->lock);
	return f;
}

/**
 * controller_state - 读取已发布的整机业务状态
 *
 * Return: 最近一次发布的业务状态。
 */
enum controller_state controller_state(void)
{
	struct controller *ctl = control_ctx();
	enum controller_state s;

	pthread_mutex_lock(&ctl->lock);
	s = (enum controller_state)ctl->published_state;
	pthread_mutex_unlock(&ctl->lock);
	return s;
}

/**
 * controller_axis_abort - 读取轴最近一次 SDO abort 码
 * @axis: 轴索引
 *
 * Return: abort 码；没有记录或轴索引越界时返回 0。
 */
uint32_t controller_axis_abort(uint8_t axis)
{
	struct controller *ctl = control_ctx();

	if (axis >= ctl->axis_count) {
		return 0U;
	}
	return axis_abort_code(&ctl->axes[axis]);
}

/**
 * controller_axis_sample - 拷贝一轴已发布的反馈样本
 * @axis: 轴索引，范围为 0 至 CONTROLLER_MAX_AXES-1
 * @out: 输出样本；NULL 时拒绝
 *
 * Return: CONTROLLER_OK 成功；CONTROLLER_ERR_PARAM 输出为空或轴索引越界。
 */
enum controller_result controller_axis_sample(uint8_t axis, struct controller_axis_sample *out)
{
	struct controller *ctl = control_ctx();

	if (out == NULL || axis >= CONTROLLER_MAX_AXES) {
		return CONTROLLER_ERR_PARAM;
	}
	pthread_mutex_lock(&ctl->lock);
	*out = ctl->published_axes[axis];
	pthread_mutex_unlock(&ctl->lock);
	return CONTROLLER_OK;
}
