/*
 * cia402/device.h — 一个 CiA 402 从站的运行控制
 *
 * 对外接口：
 *   cia402_init()         装配
 *   cia402_request()      投递一条请求，立即回答"收不收"
 *   cia402_poll()         推进一拍，同时交出流程状态
 *   cia402_get_feedback() 读取最新原始反馈
 *
 * 本层不做"等待"：流程是异步推进的，"成没成功"从 poll 的
 * struct cia402_status 里读出来。想同步等待的调用方自己轮询 poll。
 *
 * Author: zager
 */

#ifndef CIA402_DEVICE_H
#define CIA402_DEVICE_H

#include <hsm/sm_hsm.h>
#include <transport/sdo_access.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 请求 ---- */

/* 一类请求对应一条流程 */
enum cia402_kind {
	CIA402_REQ_CONFIG = 0, /* 写模式并回读确认 */
	CIA402_REQ_ENABLE,     /* PDS 使能链 */
	CIA402_REQ_DISABLE,    /* 断使能 */
	CIA402_REQ_MOVE,       /* PP 绝对位置（参数在 req.pp 里）*/
	CIA402_REQ_HALT,       /* 抢占：减速停并保持使能 */
	CIA402_REQ_QUICK_STOP  /* 抢占：快速停并断使能 */
};

struct cia402_request {
	enum cia402_kind kind;
	struct {
		int32_t target;   /* 目标位置，pulse */
		int32_t velocity; /* 轮廓速度 */
		int32_t accel;    /* 轮廓加速度 */
		int32_t decel;    /* 轮廓减速度 */
	} pp;                     /* 仅 CIA402_REQ_MOVE 使用 */
};

/* 投递结果：只说收不收，不承诺成功 */
enum cia402_response {
	CIA402_ACCEPTED = 0, /* 已放进邮箱，等待CANopen 工作线程取走 */
	CIA402_BUSY,         /* 有流程在跑或邮箱还有待办：重试可能成功 */
	CIA402_INVALID,      /* 请求本身不合法（kind、未知参数）*/
	CIA402_REFUSED       /* 前置条件不成立：先 ENABLE 再 MOVE，重试无用 */
};

/* ---- poll 的输出 ---- */

/* 当前在做什么 */
enum cia402_phase {
	CIA402_PHASE_IDLE = 0,
	CIA402_PHASE_CONFIG,
	CIA402_PHASE_ENABLE,
	CIA402_PHASE_DISABLE,
	CIA402_PHASE_MOVE,
	CIA402_PHASE_HALT,
	CIA402_PHASE_QUICK_STOP
};

/* 一次操作的结局；保持到下一次请求被受理 */
enum cia402_operation_result {
	CIA402_OP_NONE = 0, /* 还没有结局：流程在跑，或刚装配 */
	CIA402_OP_DONE,
	CIA402_OP_FAILED
};

/* 失败原因：上层据此判断"重试有没有意义" */
enum cia402_operation_fail {
	CIA402_OP_FAIL_NONE = 0,
	CIA402_OP_FAIL_TIMEOUT, /* 步骤超时：驱动器没按期响应 */
	CIA402_OP_FAIL_DRIVER,  /* 6041 报故障，复位也没成功 */
	CIA402_OP_FAIL_SDO,     /* 对象访问被 abort（见 abort_code）*/
	CIA402_OP_FAIL_CHECK    /* 回读值不符（例如模式没设进去）*/
};

/* 驱动器原始反馈，仅作显示与诊断，不用来判断流程成败 */
struct cia402_feedback {
	uint16_t statusword; /* 6041：状态字 */
	int32_t position;    /* 6064：实际位置，pulse */
	int32_t velocity;    /* 606C：实际速度，pulse/s */
	uint16_t error_code; /* 603F：驱动器错误码 */
};

struct cia402_status {
	/* 现在 */
	enum cia402_phase phase; /* 现在在做什么；IDLE = 没有流程在跑 */
	uint8_t configured;      /* 模式已确认 */
	uint8_t enabled;         /* 当前确认处于使能态 */
	uint8_t failed;          /* 有过失败记录，直到下一次 MOVE/CONFIG 受理 */

	/* 最近一次操作的结局 */
	struct {
		enum cia402_operation_result result;
		enum cia402_operation_fail fail; /* result == CIA402_OP_FAILED 时的原因 */
		uint32_t abort_code;             /* 失败对应的 SDO abort 码；0 = 没有 */
	} last;
};

/* ---- 内部数据分组（纯数据，无逻辑；分组只为可读性）---- */

/* 取消原因：谁让当前流程提前结束，决定要不要报失败 */
enum cia402_cancel_reason {
	CIA402_CANCEL_NONE = 0,
	CIA402_CANCEL_REQUEST, /* 被 Halt / Quick Stop 抢占：结论作废 */
	CIA402_CANCEL_TIMEOUT  /* 单笔事务超时：报失败 */
};

/* 当前这笔对象事务的账，仅CANopen 工作线程使用 */
struct cia402_sdo {
	uint8_t pending;          /* 有在途事务 */
	uint8_t rejected;         /* 上一笔发起被拒：下一拍补一个失败事件 */
	struct sdo_result result; /* 最近一笔结果 */
	uint32_t deadline;        /* 单笔事务期限 */
	uint16_t cw_ok;           /* 已确认的控制字 */
	uint16_t cw_next;         /* 在途的控制字 */
	uint8_t cw_next_valid;
};

/* 当前流程：仅CANopen 工作线程使用 */
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

/* 反馈轮转：仅CANopen 工作线程使用 */
struct cia402_monitor {
	uint8_t cursor; /* 下一项要读的反馈，0..3 轮转 */
	uint32_t last_ms;
};

struct cia402_device {
	/* 装配后只读 */
	const struct sdo_access *access;

	/* 请求、状态与反馈只在同一个 CANopen 工作线程内访问 */
	struct {
		struct cia402_request req; /* 待办的便条 */
		uint8_t pending;           /* 有待办 */
		uint8_t cancel;            /* enum cia402_cancel_reason */
	} mailbox;

	struct cia402_status outbox;
	struct cia402_feedback feedback;

	struct sm_hsm hsm;                 /* 状态机：当前状态由 hsm 维护 */
	uint32_t now_ms;                   /* 本拍时刻，用来算期限 */
	struct cia402_sdo sdo;             /* 当前这笔对象事务 */
	struct cia402_operation operation; /* 当前执行的操作及其进度 */
	struct cia402_monitor monitor;     /* 反馈轮转 */
};

/* 对外接口 */

void cia402_init(struct cia402_device *d, const struct sdo_access *access);

/*
 * 投递一条请求。立即返回，不阻塞。
 * HALT / QUICK_STOP 会抢占在途流程；其余请求在忙时返回 CIA402_BUSY。
 */
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);

/*
 * 推进一拍；out 非空时把这一刻的事实拷出去（在事件分发之后填，
 * 因此本拍刚产生的结论立刻可见）。仅 CANopen 工作线程调用。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms, struct cia402_status *out);

/* 工作线程读取流程状态；跨线程查询应由控制层发布快照。NULL 入参安全。 */
void cia402_get_status(const struct cia402_device *d, struct cia402_status *out);

/* 工作线程取走一次操作结论；取走后结果为 NONE，失败诊断保留。 */
enum cia402_operation_result cia402_take_result(struct cia402_device *d);

/* 工作线程读取最新原始反馈；d 或 out 为 NULL 时不执行任何操作。 */
void cia402_get_feedback(const struct cia402_device *d, struct cia402_feedback *out);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_H */
