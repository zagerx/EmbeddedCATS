/*
 * control_api.c — 将控制 API 翻译成状态机操作并提供只读信息
 *
 * 外部指令经带锁队列进入工作线程，成功入队后用 eventfd 唤醒。
 * 查询只读 controller_info，不接触主站、CANopenNode 或设备内部对象。
 *
 * Author: zager
 */

#include "controller_internal.h"

/**
 * controller_set_fault - 锁存业务故障
 * @ctl: 控制上下文；NULL 安全
 * @fault: 待锁存故障码
 *
 * 已经存在非零故障时保留最初原因，避免后续故障覆盖根因。
 */
void controller_set_fault(struct controller *ctl, enum controller_fault fault)
{
	if (ctl == NULL) {
		return;
	}
	pthread_mutex_lock(&ctl->info_lock);
	if (ctl->info.fault_code == CONTROLLER_FAULT_NONE) {
		ctl->info.fault_code = fault;
	}
	pthread_mutex_unlock(&ctl->info_lock);
}

/**
 * controller_info_update - 更新全部上报信息
 * @ctl: 控制上下文；NULL 安全
 *
 * 仅 CANopen 工作线程调用；所有字段在同一个临界区内更新。
 */
void controller_info_update(struct controller *ctl)
{
	uint8_t i;

	if (ctl == NULL) {
		return;
	}
	pthread_mutex_lock(&ctl->info_lock);
	for (i = 0u; i < CONTROLLER_STATE_COUNT; i++) {
		if (states[i] == sm_hsm_state(&ctl->hsm)) {
			ctl->info.state = (enum controller_state)i;
			break;
		}
	}
	for (i = 0u; i < ctl->axis_count; i++) {
		ctl->info.abort[i] = axis_abort_code(&ctl->axes[i]);
	}
	pthread_mutex_unlock(&ctl->info_lock);
}

/**
 * controller_estop - 请求所有轴急停
 *
 * 任意线程均可调用；事件插入队头并立即唤醒工作线程。
 */
void controller_estop(void)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_ESTOP}};

	if (ctl->master == NULL) {
		return;
	}
	controller_evtq_push_front(&ctl->evtq, &evt);
	controller_wakeup();
}

/**
 * controller_clear_estop - 请求解除急停
 *
 * 任意线程均可调用；事件插入队头并立即唤醒工作线程。
 * 只有 ESTOP 接受：驱动器断使能，控制器回 IDLE，故障码保持锁存。
 */
void controller_clear_estop(void)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_ESTOP_CLEAR}};

	if (ctl->master == NULL) {
		return;
	}
	controller_evtq_push_front(&ctl->evtq, &evt);
	controller_wakeup();
}

/**
 * controller_enable - 请求各轴使能
 *
 * 任意线程均可调用；只有 IDLE 会接受，其余状态由状态机丢弃。
 *
 * Return: OK 已入队并唤醒；BUSY 队列满；STATE 控制层未初始化。
 */
enum controller_result controller_enable(void)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_ENABLE}};

	if (ctl->master == NULL) {
		return CONTROLLER_ERR_STATE;
	}
	if (!controller_evtq_push(&ctl->evtq, &evt)) {
		return CONTROLLER_ERR_BUSY;
	}
	controller_wakeup();
	return CONTROLLER_OK;
}

/**
 * controller_move - 同时提交一批绝对位置指令
 * @reqs: 指令数组，每条含轴号与目标位置/速度/加速度
 * @count: 条数（1..CONTROLLER_AXIS_COUNT）
 *
 * 一批轴一起启动，全部到位才回 HOLDING。
 *
 * Return: OK 已入队并唤醒；BUSY 队列满；PARAM 参数无效或未初始化。
 */
enum controller_result controller_move(const struct controller_move_req *reqs, uint8_t count)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_MOVE}};
	enum controller_result result;
	uint8_t i;

	if (ctl->master == NULL || reqs == NULL || count == 0u || count > CONTROLLER_AXIS_COUNT) {
		return CONTROLLER_ERR_PARAM;
	}
	evt.u.move.count = count;
	for (i = 0u; i < count; i++) {
		const struct controller_move_req *m = &reqs[i];

		if (m->axis >= ctl->axis_count) {
			return CONTROLLER_ERR_PARAM;
		}
		if (m->mm != m->mm || m->mm_s != m->mm_s || m->mm_s2 != m->mm_s2) {
			return CONTROLLER_ERR_PARAM;
		}
		evt.u.move.axis[i] = reqs[i];
	}
	result = controller_evtq_push(&ctl->evtq, &evt) ? CONTROLLER_OK : CONTROLLER_ERR_BUSY;
	if (result == CONTROLLER_OK) {
		controller_wakeup();
	}
	return result;
}

/**
 * controller_halt - 请求暂停并保持使能
 *
 * 任意线程均可调用；暂停事件插入队头并立即唤醒工作线程。
 */
void controller_halt(void)
{
	struct controller *ctl = control_ctx();
	struct controller_evt evt = {.super = {.sig = CONTROLLER_SIG_HALT}};

	if (ctl->master == NULL) {
		return;
	}
	controller_evtq_push_front(&ctl->evtq, &evt);
	controller_wakeup();
}

/**
 * controller_info_get - 读取完整控制器信息
 * @out: 输出信息；NULL 时返回 PARAM
 *
 * 在同一次加锁里复制全部字段，保证状态、故障、反馈和 abort 属于同一时刻。
 *
 * Return: OK 已填充；PARAM 参数为空；STATE 控制层未初始化。
 */
enum controller_result controller_info_get(struct controller_info *out)
{
	struct controller *ctl = control_ctx();

	if (out == NULL) {
		return CONTROLLER_ERR_PARAM;
	}
	if (ctl->master == NULL) {
		return CONTROLLER_ERR_STATE;
	}
	pthread_mutex_lock(&ctl->info_lock);
	*out = ctl->info;
	pthread_mutex_unlock(&ctl->info_lock);
	return CONTROLLER_OK;
}
