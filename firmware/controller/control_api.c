/*
 * control_api.c — 将控制 API 翻译成状态机操作并发布只读快照
 *
 * 外部指令经事件队列进入控制线程，查询接口读取发布值或轴事实。
 * 装配以及 I/O、服务推进位于 controller.c。
 *
 * Author: zager
 */

#include "controller_internal.h"

/* 汇总所有已装配轴的心跳事实，供每次状态机推进共用 */
static uint8_t heartbeat_fact(const struct cats_control *ctl);

/**
 * cats_control_publish - 发布整机状态、故障和各轴反馈快照
 *
 * 只能由控制线程调用；轴单位换算在发布锁外完成。
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

/**
 * cats_control_poll - 推进一次业务状态机并发布快照
 *
 * 只能由控制线程调用；本函数不执行总线收发或阻塞式对象访问。
 *
 * Return: CATS_OK 推进成功；CATS_ERR_STATE 尚未初始化。
 */
enum cats_result cats_control_poll(void)
{
	struct cats_control *ctl = control_ctx();
	uint32_t now;

	if (ctl->master == NULL) {
		return CATS_ERR_STATE;
	}
	now = now_ms(ctl);

	/* 收帧和 402/SDO 推进由其他入口执行，本拍只向状态机提供事实。 */
	stm_poll(ctl->stm, heartbeat_fact(ctl), now);

	cats_control_publish();
	return CATS_OK;
}

/**
 * cats_control_estop - 请求所有轴急停
 *
 * 任意线程均可调用；事件插入队头，ESTOP 只能重启退出。
 */
void cats_control_estop(void)
{
	struct cats_control *ctl = control_ctx();

	if (ctl->master == NULL) {
		return;
	}
	/* 插到队头：优先于已排队的所有命令 */
	(void)stm_command(ctl->stm, STM_CMD_ESTOP, NULL);
}

/**
 * cats_control_move_to - 提交绝对位置指令
 * @axis: 轴索引
 * @mm: 目标位置，单位 mm
 * @mm_s: 速度，单位 mm/s
 * @mm_s2: 加速度和减速度，单位 mm/s²
 *
 * Return: CATS_OK 已入队；CATS_ERR_BUSY 队列满；CATS_ERR_PARAM 参数或状态无效。
 */
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

/**
 * cats_control_stop - 请求减速停并保持使能
 *
 * 任意线程均可调用；停止事件插入队头，由控制线程消费。
 */
void cats_control_stop(void)
{
	struct cats_control *ctl = control_ctx();

	if (ctl->master == NULL) {
		return;
	}
	/* 停止也插队：排在已有序命令前面 */
	(void)stm_command(ctl->stm, STM_CMD_STOP, NULL);
}

/**
 * cats_control_fault_code - 读取已发布的整机故障码
 *
 * Return: 最近一次发布的故障码。
 */
enum cats_fault cats_control_fault_code(void)
{
	struct cats_control *ctl = control_ctx();
	enum cats_fault f;

	pthread_mutex_lock(&ctl->lock);
	f = (enum cats_fault)ctl->published_fault;
	pthread_mutex_unlock(&ctl->lock);
	return f;
}

/**
 * cats_control_state - 读取已发布的整机业务状态
 *
 * Return: 最近一次发布的业务状态。
 */
enum cats_state cats_control_state(void)
{
	struct cats_control *ctl = control_ctx();
	enum cats_state s;

	pthread_mutex_lock(&ctl->lock);
	s = (enum cats_state)ctl->published_state;
	pthread_mutex_unlock(&ctl->lock);
	return s;
}

/**
 * cats_control_axis_abort - 读取轴最近一次 SDO abort 码
 * @axis: 轴索引
 *
 * Return: abort 码；没有记录或轴索引越界时返回 0。
 */
uint32_t cats_control_axis_abort(uint8_t axis)
{
	struct cats_control *ctl = control_ctx();

	if (axis >= ctl->axis_count) {
		return 0U;
	}
	return axis_abort_code(&ctl->axes[axis]);
}

/**
 * cats_control_axis_sample - 拷贝一轴已发布的反馈样本
 * @axis: 轴索引，范围为 0 至 CATS_MAX_AXES-1
 * @out: 输出样本；NULL 时拒绝
 *
 * Return: CATS_OK 成功；CATS_ERR_PARAM 输出为空或轴索引越界。
 */
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

/**
 * cats_control_state_name - 获取业务状态名称
 * @s: 业务状态枚举
 *
 * Return: 静态名称字符串；未知状态返回 "?"。
 */
const char *cats_control_state_name(enum cats_state s)
{
	return stm_state_name(s);
}

/**
 * cats_control_fault_name - 获取整机故障名称
 * @f: 故障枚举
 *
 * Return: 静态名称字符串；未知故障返回 "?"。
 */
const char *cats_control_fault_name(enum cats_fault f)
{
	return stm_fault_name(f);
}

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
