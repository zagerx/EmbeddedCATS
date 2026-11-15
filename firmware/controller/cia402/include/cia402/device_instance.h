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

/* 6040 控制字镜像；生命周期是一次操作，跨多笔事务，仅工作线程使用 */
struct cia402_controlword {
	uint16_t confirmed;     /* 最近一次写成功的控制字 */
	uint16_t pending;       /* 在途的控制字 */
	uint8_t pending_valid;  /* pending 有待确认的写入 */
};

/* 当前流程：仅 CANopen 工作线程使用 */
struct cia402_operation {
	enum cia402_kind kind;
	uint8_t running;   /* 流程在跑；自己收尾时清零，被打断退出时仍为 1 */
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

	struct cia402_operation_result outbox; /* 最近一次操作的回执 */

	/* 内部执行状态：一个 operation 可依次包含多笔 SDO 事务，事务状态由通道持有 */
	struct sm_hsm hsm;
	uint32_t now_ms;
	struct cia402_controlword cw;
	struct cia402_operation operation;
};

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_INSTANCE_H */
