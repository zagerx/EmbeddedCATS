/*
 * cia402/device_instance.h — CiA 402 实例存储定义
 *
 * 只有需要静态持有实例的对象所有者和 cia402 内部包含本头。
 * 普通调用方只包含 device.h，通过指针和公开函数使用实例。
 * 字段不属于公共接口，所有者不得直接读写。
 *
 * Author: zager
 */

#ifndef CIA402_DEVICE_INSTANCE_H
#define CIA402_DEVICE_INSTANCE_H

#include <cia402/device.h>

#include <hsm/sm_hsm.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 取消原因：谁让当前流程提前结束，决定要不要报失败 */
enum cia402_cancel_reason {
	CIA402_CANCEL_NONE = 0,
	CIA402_CANCEL_REQUEST, /* 被 Halt / Quick Stop 抢占：结论作废 */
	CIA402_CANCEL_TIMEOUT  /* 单笔事务超时：报失败 */
};

/* 当前这笔对象事务的账，仅 CANopen 工作线程使用 */
struct cia402_sdo {
	uint8_t pending;          /* 有在途事务 */
	uint8_t rejected;         /* 上一笔发起被拒：下一拍补一个失败事件 */
	uint8_t armed;            /* 事务已上总线，deadline 已起算 */
	struct sdo_result result; /* 最近一笔结果 */
	uint32_t timeout_ms;      /* 本笔事务的业务期限长度 */
	uint32_t deadline;        /* 事务上总线后才起算的期限 */
	uint16_t cw_ok;           /* 已确认的控制字 */
	uint16_t cw_next;         /* 在途的控制字 */
	uint8_t cw_next_valid;
};

/* 当前流程：仅 CANopen 工作线程使用 */
struct cia402_operation {
	enum cia402_kind kind;
	uint8_t cancel;    /* enum cia402_cancel_reason，交给 st_idle 决定要不要报失败 */
	uint32_t deadline; /* 流程期限 */
	struct {
		int32_t target;
		int32_t velocity;
		int32_t accel;
		int32_t decel;
		int32_t issued;   /* 触发时锁定的目标，判定到位用它 */
		uint8_t seen_low; /* 见过 bit10 = 0 */
	} pp;
};

/* 所有字段仅由同一个 CANopen 工作线程访问 */
struct cia402_device {
	const struct sdo_access *access; /* 装配后只读 */

	/* 单槽命令入口：普通请求忙时拒绝；抢占请求可覆盖待办并取消旧流程 */
	struct {
		struct cia402_request req;
		uint8_t pending;
		uint8_t cancel; /* enum cia402_cancel_reason */
	} mailbox;

	struct cia402_status outbox; /* 主站确认结果 */

	/* 内部执行状态：一个 operation 可依次包含多笔 SDO 事务 */
	struct sm_hsm hsm;
	uint32_t now_ms;
	struct cia402_sdo sdo;
	struct cia402_operation operation;
};

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_INSTANCE_H */
