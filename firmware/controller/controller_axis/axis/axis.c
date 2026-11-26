/*
 * axis.c — 换算下行命令单位并转发单轴动作请求
 *
 * 一个轴对应一个 CiA 402 设备实例，业务状态由状态机管理。
 * 机械换算参数来自 cats_mech.h，编译期固定。
 * 集合级动作与汇总判定在同模块的 controller_axis.c，本文件只管一个轴。
 *
 * Author: zager
 */

#include <controller_axis/axis.h>

#include <cats_mech.h>

#include <stddef.h>

static int axis_submit(struct axis *a, const struct cia402_request *req);

/* mm 到 pulse：double 计算、远离零取整并饱和到 int32，机械参数见 cats_mech.h */
static int32_t scale_mm(float mm);

/**
 * axis_poll - 推进一次设备状态机
 * @a: 轴对象；NULL 安全
 * @now_ms: 当前单调毫秒时刻
 *
 * 不在轴层复制状态；调用方通过设备接口读取事实或消费结论。
 */
void axis_poll(struct axis *a, uint32_t now_ms)
{
	if (a == NULL) {
		return;
	}
	cia402_poll(&a->dev, now_ms);
}

/**
 * axis_move_to - 提交绝对位置运动
 * @a: 轴对象；NULL 时拒绝
 * @mm: 目标位置
 * @mm_s: 轮廓速度
 * @mm_s2: 轮廓加速度和减速度
 * Return: 0 已受理；1 设备忙；-1 轴为空、参数含 NaN 或设备拒绝。
 */
int axis_move_to(struct axis *a, float mm, float mm_s, float mm_s2)
{
	if (a == NULL) {
		return -1;
	}
	if (mm != mm || mm_s != mm_s || mm_s2 != mm_s2) {
		return -1; /* NaN：进到整数换算就是未定义行为，这里就拦住 */
	}
	{
		struct cia402_request req;

		req.kind = CIA402_REQ_MOVE;
		req.pp.target = scale_mm(mm);
		req.pp.velocity = scale_mm(mm_s);
		req.pp.accel = scale_mm(mm_s2);
		req.pp.decel = scale_mm(mm_s2);
		return axis_submit(a, &req);
	}
}

/**
 * axis_halt - 请求减速停车并保持使能
 * @a: 轴对象；NULL 安全
 *
 * Return: 设备请求结果；@a 为 NULL 时返回 -1。
 */
int axis_halt(struct axis *a)
{
	if (a == NULL) {
		return -1;
	}
	{
		const struct cia402_request req = {.kind = CIA402_REQ_HALT};

		return axis_submit(a, &req);
	}
}

/**
 * axis_home - 请求回原点（原点开关法）
 * @a: 轴对象；NULL 时拒绝
 *
 * 方法号、两档速度与加减速（6098h / 6099h / 609Ah）是驱动器侧的持久配置，
 * 由 apps/tools/drive_setup 负责写入；业务层只发"回零"这一个动作。
 *
 * Return: 设备请求结果；@a 为 NULL 时返回 -1。
 */
int axis_home(struct axis *a)
{
	const struct cia402_request req = {.kind = CIA402_REQ_HOME};

	if (a == NULL) {
		return -1;
	}
	return axis_submit(a, &req);
}

/**
 * axis_is_running - 查询轴是否还有待执行或取消清理的流程
 * @a: 轴对象；NULL 安全
 *
 * Return: 非 0 表示仍在执行；取消结束不产生结局。
 */
int axis_is_running(const struct axis *a)
{
	return a != NULL && cia402_is_running(&a->dev);
}

/**
 * axis_take_motion - 取走轴最近一次动作的结论
 * @a: 轴对象；NULL 安全
 * @failure: 输出失败归类；可为 NULL
 *
 * 取走后结局回 NONE；失败诊断仍保留在设备中，失败后再读取。
 *
 * Return: DONE、FAILED 或 NONE；@a 为 NULL 时返回 NONE。
 */
enum axis_motion axis_take_motion(struct axis *a, struct axis_failure *failure)
{
	enum cia402_outcome result;

	if (a == NULL) {
		return AXIS_MOTION_NONE;
	}

	result = cia402_take_result(&a->dev);
	if (result == CIA402_OUTCOME_FAILED && failure != NULL) {
		cia402_failure_of(&a->dev, &failure->fail);
	}
	if (result == CIA402_OUTCOME_DONE) {
		return AXIS_MOTION_DONE;
	}
	if (result == CIA402_OUTCOME_FAILED) {
		return AXIS_MOTION_FAILED;
	}
	return AXIS_MOTION_NONE;
}

/**
 * axis_config_movemode - 给本轴写运动模式
 * @a: 轴对象；NULL 时拒绝
 *
 * 已配过的轴会被设备层拒绝，不算错。
 *
 * Return: 0 已受理；1 设备忙；-1 轴为空。
 */
int axis_config_movemode(struct axis *a)
{
	const struct cia402_request req = {.kind = CIA402_REQ_CONFIG};

	if (a == NULL) {
		return -1;
	}
	return axis_submit(a, &req);
}

/**
 * axis_enable - 请求本轴使能
 * @a: 轴对象；NULL 时拒绝
 *
 * Return: 0 已受理；1 设备忙；-1 轴为空。已使能时会被内部拒绝，不算错。
 */
int axis_enable(struct axis *a)
{
	const struct cia402_request req = {.kind = CIA402_REQ_ENABLE};

	if (a == NULL) {
		return -1;
	}
	return axis_submit(a, &req);
}

/**
 * axis_quick_stop - 请求本轴快速停止
 * @a: 轴对象；NULL 时拒绝
 *
 * Return: 0 已受理；1 设备忙；-1 轴为空。
 */
int axis_quick_stop(struct axis *a)
{
	const struct cia402_request req = {.kind = CIA402_REQ_QUICK_STOP};

	if (a == NULL) {
		return -1;
	}
	return axis_submit(a, &req);
}

/**
 * axis_clear_estop - 解除本轴急停
 * @a: 轴对象；NULL 时拒绝
 *
 * 只负责下发：写 6040 = 0x0F 让驱动器退出 quick stop 状态。
 *
 * Return: 0 已受理；1 设备忙；-1 轴为空。
 */
int axis_clear_estop(struct axis *a)
{
	const struct cia402_request req = {.kind = CIA402_REQ_ESTOP_CLEAR};

	if (a == NULL) {
		return -1;
	}
	return axis_submit(a, &req);
}

static int32_t scale_mm(float mm)
{
	double p = ((double)mm * (double)CATS_AXIS_PULSES_PER_REV) / (double)CATS_AXIS_MM_PER_REV;

	p += (p >= 0.0) ? 0.5 : -0.5;
	if (p >= 2147483647.0) {
		return INT32_MAX;
	}
	if (p <= -2147483647.0) {
		return -INT32_MAX;
	}
	return (int32_t)p;
}

static int axis_submit(struct axis *a, const struct cia402_request *req)
{
	if (cia402_request(&a->dev, req) != CIA402_ACCEPTED) {
		return 1;
	}
	return 0;
}
