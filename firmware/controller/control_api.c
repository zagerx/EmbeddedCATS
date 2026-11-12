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
	struct cats_axis_sample samples[CATS_MAX_AXES];
	uint8_t i;

	if (app->master.bus == NULL) {
		return;
	}
	state = (int)stm_state(&app->stm);
	/* 换算在锁外做（axis_sample 只读本线程写的字段，不做 I/O） */
	for (i = 0; i < CATS_MAX_AXES; i++) {
		struct axis_sample s;

		axis_sample(&app->axes[i], &s);
		samples[i].position_mm = s.position_mm;
		samples[i].velocity_mm_s = s.velocity_mm_s;
		samples[i].statusword = s.statusword;
		samples[i].error_code = s.error_code;
	}
	pthread_mutex_lock(&app->lock);
	app->published_state = state;
	app->published_fault = (int)stm_fault(&app->stm);
	for (i = 0; i < CATS_MAX_AXES; i++) {
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
	struct cats_evt evt = {.super = {.sig = CATS_SIG_ESTOP}};

	if (ctl->master.bus == NULL) {
		return;
	}
	/* 插到队头：优先于已排队的所有命令 */
	cats_evtq_push_front(&ctl->stm.evtq, &evt);
}

/**
 * controller_move - 同时提交一批绝对位置指令
 * @reqs: 指令数组，每条含轴号与目标位置/速度/加速度
 * @count: 条数（1..CATS_MAX_AXES）
 *
 * 一批轴一起启动，全部到位才回 HOLDING。
 *
 * Return: CATS_OK 已入队；CATS_ERR_BUSY 队列满；CATS_ERR_PARAM 参数或状态无效。
 */
enum cats_result controller_move(const struct stm_move *reqs, uint8_t count)
{
	struct controller *ctl = control_ctx();
	struct cats_evt evt = {.super = {.sig = CATS_SIG_MOVE}};
	uint8_t i;

	if (ctl->master.bus == NULL || reqs == NULL || count == 0u || count > CATS_MAX_AXES) {
		return CATS_ERR_PARAM;
	}
	evt.u.move.count = count;
	for (i = 0u; i < count; i++) {
		const struct stm_move *m = &reqs[i];

		if (m->axis >= ctl->axis_count) {
			return CATS_ERR_PARAM;
		}
		/* NaN 进到整数换算就是未定义行为：入口就拦住（±inf 由轴层饱和） */
		if (m->mm != m->mm || m->mm_s != m->mm_s || m->mm_s2 != m->mm_s2) {
			return CATS_ERR_PARAM;
		}
		evt.u.move.axis[i] = reqs[i];
	}
	/* 排到队尾：多条指令按顺序保留；队列满则本条被丢弃 */
	return cats_evtq_push(&ctl->stm.evtq, &evt) ? CATS_OK : CATS_ERR_BUSY;
}

/**
 * controller_move_to - 提交单轴绝对位置指令
 * @axis: 轴索引
 * @mm: 目标位置，单位 mm
 * @mm_s: 速度，单位 mm/s
 * @mm_s2: 加速度和减速度，单位 mm/s²
 *
 * Return: CATS_OK 已入队；CATS_ERR_BUSY 队列满；CATS_ERR_PARAM 参数或状态无效。
 */
enum cats_result controller_move_to(uint8_t axis, float mm, float mm_s, float mm_s2)
{
	struct stm_move mv;

	mv.axis = axis;
	mv.mm = mm;
	mv.mm_s = mm_s;
	mv.mm_s2 = mm_s2;
	return controller_move(&mv, 1u);
}

/**
 * controller_stop - 请求减速停并保持使能
 *
 * 任意线程均可调用；停止事件插入队头，由控制线程消费。
 */
void controller_stop(void)
{
	struct controller *ctl = control_ctx();
	struct cats_evt evt = {.super = {.sig = CATS_SIG_STOP}};

	if (ctl->master.bus == NULL) {
		return;
	}
	/* 停止也插队：排在已有序命令前面 */
	cats_evtq_push_front(&ctl->stm.evtq, &evt);
}

/**
 * controller_fault_code - 读取已发布的整机故障码
 *
 * Return: 最近一次发布的故障码。
 */
enum cats_fault controller_fault_code(void)
{
	struct controller *ctl = control_ctx();
	enum cats_fault f;

	pthread_mutex_lock(&ctl->lock);
	f = (enum cats_fault)ctl->published_fault;
	pthread_mutex_unlock(&ctl->lock);
	return f;
}

/**
 * controller_state - 读取已发布的整机业务状态
 *
 * Return: 最近一次发布的业务状态。
 */
enum cats_state controller_state(void)
{
	struct controller *ctl = control_ctx();
	enum cats_state s;

	pthread_mutex_lock(&ctl->lock);
	s = (enum cats_state)ctl->published_state;
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
 * @axis: 轴索引，范围为 0 至 CATS_MAX_AXES-1
 * @out: 输出样本；NULL 时拒绝
 *
 * Return: CATS_OK 成功；CATS_ERR_PARAM 输出为空或轴索引越界。
 */
enum cats_result controller_axis_sample(uint8_t axis, struct cats_axis_sample *out)
{
	struct controller *ctl = control_ctx();

	if (out == NULL || axis >= CATS_MAX_AXES) {
		return CATS_ERR_PARAM;
	}
	pthread_mutex_lock(&ctl->lock);
	*out = ctl->published_axes[axis];
	pthread_mutex_unlock(&ctl->lock);
	return CATS_OK;
}

/**
 * controller_state_name - 获取业务状态名称
 * @s: 业务状态枚举
 *
 * Return: 静态名称字符串；未知状态返回 "?"。
 */
const char *controller_state_name(enum cats_state s)
{
	return stm_state_name(s);
}

/**
 * controller_fault_name - 获取整机故障名称
 * @f: 故障枚举
 *
 * Return: 静态名称字符串；未知故障返回 "?"。
 */
const char *controller_fault_name(enum cats_fault f)
{
	return stm_fault_name(f);
}
