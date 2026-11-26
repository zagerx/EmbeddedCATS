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
	uint16_t confirmed;    /* 最近一次写成功的控制字 */
	uint16_t pending;      /* 在途的控制字 */
	uint8_t pending_valid; /* pending 有待确认的写入 */
};

/* 当前流程：仅 CANopen 工作线程使用 */
struct cia402_operation {
	enum cia402_kind kind;
	uint8_t running;   /* 流程在跑；自己收尾时清零，被打断退出时仍为 1 */
	uint8_t cancel;    /* enum cia402_cancel_reason，交给 st_idle 决定要不要报失败 */
	uint8_t stage;     /* 当前步骤号；枚举由各 operations 文件自己定义 */
	uint32_t deadline; /* 当前步骤的期限，由各操作按自己的阶段解释 */
	uint32_t flow_deadline; /* 整条流程的总期限，公共父状态看门狗用 */
	struct {
		int32_t target;
		int32_t velocity;
		int32_t accel;
		int32_t decel;
		uint8_t online; /* 非 0：本段是在途改目标，用 0x103F 触发 */
	} pp;
	/* 在途改目标：运动中的新指令先存这里，等读到状态字那一步才生效 */
	struct {
		int32_t target;
		int32_t velocity;
		int32_t accel;
		int32_t decel;
	} retarget;
	uint8_t retarget_valid; /* retarget 里有待生效的目标 */
	struct {
		/*
		 * 回零的结局要等收尾链（落 bit4、切回 PP）走完才交付，
		 * 所以先记在这里，由收尾链最后一步发布。
		 * pending 取 enum cia402_outcome：NONE 表示被抢占，不交付结局。
		 */
		uint8_t pending;
		uint8_t fail_kind; /* enum cia402_fail，仅 pending == FAILED 时有效 */
	} home;
};

/* 所有字段仅由同一个 CANopen 工作线程访问 */
struct cia402_device {
	struct sdo_client *client; /* 装配后只读 */

	/* 单槽命令入口：普通请求忙时拒绝；抢占请求可覆盖待办并取消旧流程 */
	struct {
		struct cia402_request req;
		uint8_t pending;
		uint8_t cancel; /* enum cia402_cancel_reason */
	} mailbox;

	enum cia402_outcome outbox; /* 待取走的成功或失败结局 */
	enum cia402_fail failure;   /* 最近一次真实执行失败的归类 */
	/* 最近一笔发起的事务：失败时把 abort 码和对象对上号，只在本层打印用 */
	struct {
		uint16_t index;
		uint8_t subindex;
		uint8_t dir; /* enum sdo_dir */
	} txn;
	uint8_t sdo_pending; /* 本设备有一笔已受理且未取走的事务 */
	/* 上一步事务未受理：流程已判失败，下一拍还要退出实际状态 */
	uint8_t sdo_unstarted;

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
