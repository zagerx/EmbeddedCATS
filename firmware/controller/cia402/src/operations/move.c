/*
 * move.c — PP 绝对位置运动操作
 *
 * 写四个参数 → 6040 bit4 上升沿（再落沿）触发 → 读 6041 等 bit10。
 * 在途改目标只写最后一帧 6040 = 0x103F，然后同样读 6041 等 bit10。
 *
 * 到位只信驱动器的状态字 bit10。写 6040 是收到应答才算成功的，应答之前驱动器
 * 已经处理过该控制字，所以之后读到的 bit10 属于本次运动，不会再拿到上一条的
 * 陈旧值。位置由 TPDO 反馈另行提供，不参与到位判定。
 *
 * 运动中的新目标走在线变位：重写四个参数后写一帧 6040 = 0x103F，驱动器按新
 * 目标立即重规划，不停车也不重复起步，所以不需要 bit4 的边沿。
 *
 * 七个步骤在 operation.stage 里顺序推进；每笔事务分轮执行，工作线程可以在
 * 等待期间处理其它设备。
 *
 * Author: zager
 */

#include "../states/states.h"

/* 一步一笔事务，顺序即流程本身 */
enum move_stage {
	MOVE_WRITE_VELOCITY = 0,
	MOVE_WRITE_ACCEL,
	MOVE_WRITE_DECEL,
	MOVE_WRITE_TARGET,
	MOVE_TRIGGER_HIGH, /* 普通定位写 bit4 = 1；在线变位写 0x103F */
	MOVE_TRIGGER_LOW,  /* 仅普通定位，在线变位跳过这一步 */
	MOVE_WAIT_ARRIVE,
};

static sm_state_t move_step(struct cia402_device *d, const struct sdo_result *result);
static sm_state_t move_arrived(struct cia402_device *d, uint16_t sw);
static void move_advance(struct cia402_device *d);
static sm_state_t move_issue(struct cia402_device *d);
static void arm_arrive(struct cia402_device *d);
static void take_retarget(struct cia402_device *d);

/**
 * st_move - PP 绝对位置运动
 * @me: 状态机内核对象
 * @e: 待处理事件
 */
sm_ret_t st_move(struct sm_hsm *const me, const struct sm_event *const e)
{
	struct cia402_device *d = DEV_OF(me);
	sm_state_t next;

	switch (e->sig) {
	case SM_SIG_ENTRY:
		d->operation.stage = MOVE_WRITE_VELOCITY;
		next = move_step(d, NULL);
		break;

	case CIA402_SIG_SDO_DONE:
		next = move_step(d, DEV_SDO_RESULT(e));
		break;

	default:
		return SM_SUPER(&st_operation_active);
	}
	return (next != NULL) ? SM_TRAN(next) : SM_HANDLED();
}

/**
 * move_step - 判上一步结果并推进
 * @d: 设备对象
 * @result: 上一步的事务结果；NULL 表示刚进入流程
 *
 * Return: 要迁移到的状态；NULL 表示已发起下一步。
 */
static sm_state_t move_step(struct cia402_device *d, const struct sdo_result *result)
{
	if (result != NULL) {
		if (result->status != 0) {
			dev_fail(d, result);
			return &st_idle;
		}
		if (d->operation.stage == MOVE_WAIT_ARRIVE) {
			sm_state_t end = move_arrived(d, (uint16_t)result->value);

			if (end != NULL) {
				return end;
			}
		} else {
			move_advance(d);
		}
	}
	return move_issue(d);
}

/**
 * move_arrived - 判到位
 * @d: 设备对象
 * @sw: 6041 原始值
 *
 * 在途改目标优先于到位：新目标到了就重跑参数链，不能因为旧目标刚好报到位
 * 就把用户后来的指令丢掉。
 *
 * Return: 要迁移到的状态；NULL 表示还没到位（再读一次）或已改好 stage。
 */
static sm_state_t move_arrived(struct cia402_device *d, uint16_t sw)
{
	if (d->operation.retarget_valid != 0u) {
		take_retarget(d);
		return NULL;
	}
	if (cia402_parse(sw) == CIA402_FAULT) {
		dev_finish(d, CIA402_FAIL_DRIVER);
		return &st_idle;
	}
	if ((sw & CIA402_SW_TARGET_REACHED) != 0u) {
		dev_done(d);
		return &st_idle;
	}
	if ((int32_t)(d->now_ms - d->operation.deadline) >= 0) {
		dev_finish(d, CIA402_FAIL_TIMEOUT);
		return &st_idle;
	}
	return NULL; /* 还没到位：再读一次 */
}

/**
 * move_advance - 一笔写成功后进入下一步
 * @d: 设备对象
 *
 * 到位等待由 move_arrived 判定，这里不推进。
 */
static void move_advance(struct cia402_device *d)
{
	switch (d->operation.stage) {
	case MOVE_WRITE_VELOCITY:
		d->operation.stage = MOVE_WRITE_ACCEL;
		break;
	case MOVE_WRITE_ACCEL:
		d->operation.stage = MOVE_WRITE_DECEL;
		break;
	case MOVE_WRITE_DECEL:
		d->operation.stage = MOVE_WRITE_TARGET;
		break;
	case MOVE_WRITE_TARGET:
		d->operation.stage = MOVE_TRIGGER_HIGH;
		break;
	case MOVE_TRIGGER_HIGH:
		if (d->operation.pp.online != 0u) {
			/* 在线变位：单帧指令，写完直接等到位，没有落沿这一步 */
			arm_arrive(d);
			d->operation.stage = MOVE_WAIT_ARRIVE;
		} else {
			d->operation.stage = MOVE_TRIGGER_LOW;
		}
		break;
	case MOVE_TRIGGER_LOW:
		arm_arrive(d);
		d->operation.stage = MOVE_WAIT_ARRIVE;
		break;
	default: /* MOVE_WAIT_ARRIVE */
		break;
	}
}

/**
 * move_issue - 发起当前步骤的那笔事务
 * @d: 设备对象
 */
static sm_state_t move_issue(struct cia402_device *d)
{
	switch (d->operation.stage) {
	case MOVE_WRITE_VELOCITY:
		return dev_start_write(d, CIA402_OD_PROFILE_VELOCITY,
				       (uint32_t)d->operation.pp.velocity, 4u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	case MOVE_WRITE_ACCEL:
		return dev_start_write(d, CIA402_OD_PROFILE_ACCEL, (uint32_t)d->operation.pp.accel,
				       4u, CIA402_DEVICE_SDO_TIMEOUT_MS);
	case MOVE_WRITE_DECEL:
		return dev_start_write(d, CIA402_OD_PROFILE_DECEL, (uint32_t)d->operation.pp.decel,
				       4u, CIA402_DEVICE_SDO_TIMEOUT_MS);
	case MOVE_WRITE_TARGET:
		return dev_start_write(d, CIA402_OD_TARGET_POSITION,
				       (uint32_t)d->operation.pp.target, 4u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	case MOVE_TRIGGER_HIGH:
		if (d->operation.pp.online != 0u) {
			return dev_start_pulse(d, CIA402_CW_ONLINE_SETPOINT,
					       CIA402_DEVICE_SDO_TIMEOUT_MS);
		}
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       d->cw.confirmed | CIA402_CW_NEW_SETPOINT, 2u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	case MOVE_TRIGGER_LOW:
		return dev_start_write(d, CIA402_OD_CONTROLWORD,
				       d->cw.confirmed & (uint16_t)~CIA402_CW_NEW_SETPOINT, 2u,
				       CIA402_DEVICE_SDO_TIMEOUT_MS);
	default: /* MOVE_WAIT_ARRIVE */
		return dev_start_read(d, CIA402_OD_STATUSWORD, CIA402_DEVICE_SDO_TIMEOUT_MS);
	}
}

static void arm_arrive(struct cia402_device *d)
{
	d->operation.deadline = d->now_ms + CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS;
}

/* 新目标生效：整段参数按新值重跑，触发改用在线变位 */
static void take_retarget(struct cia402_device *d)
{
	d->operation.retarget_valid = 0u;
	d->operation.pp.target = d->operation.retarget.target;
	d->operation.pp.velocity = d->operation.retarget.velocity;
	d->operation.pp.accel = d->operation.retarget.accel;
	d->operation.pp.decel = d->operation.retarget.decel;
	d->operation.pp.online = 1u;
	d->operation.stage = MOVE_WRITE_VELOCITY;
}
