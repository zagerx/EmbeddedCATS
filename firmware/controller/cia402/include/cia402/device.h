/*
 * cia402/device.h — 一个 CiA 402 从站的运行控制
 *
 * 对外只有三个动词：
 *   cia402_init()     装配
 *   cia402_request()  投递一条请求，立即回答"收不收"
 *   cia402_poll()     推进一拍，同时交出这一刻的事实
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

#include <pthread.h>
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
enum cia402_ret {
	CIA402_ACCEPTED = 0, /* 已受理，成为当前流程 */
	CIA402_BUSY,         /* 上一条还没结束 */
	CIA402_INVALID,      /* 请求本身不合法（kind、参数）*/
	CIA402_REFUSED       /* 合法但当前不允许（未配置/未使能）*/
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

/* 最近一条流程的结论；保持到下一次请求被受理 */
enum cia402_result {
	CIA402_RESULT_NONE = 0,
	CIA402_RESULT_DONE,
	CIA402_RESULT_FAILED
};

/* 失败原因：上层据此判断"重试有没有意义" */
enum cia402_fail {
	CIA402_FAIL_NONE = 0,
	CIA402_FAIL_TIMEOUT, /* 步骤超时：驱动器没按期响应 */
	CIA402_FAIL_DRIVER,  /* 6041 报故障，复位也没成功 */
	CIA402_FAIL_SDO,     /* 对象访问被 abort（见 abort_code）*/
	CIA402_FAIL_CHECK    /* 回读值不符（例如模式没设进去）*/
};

struct cia402_status {
	enum cia402_phase phase;   /* 现在在做什么 */
	enum cia402_result result; /* 最近一条流程的结论 */
	enum cia402_fail fail;     /* result == FAILED 时的原因 */
	uint32_t abort_code;       /* 失败对应的 SDO abort 码；0 = 没有 */
	uint8_t configured;        /* 模式已确认 */
	uint8_t enabled;           /* 当前确认处于使能态 */
	uint8_t failed;            /* 有过失败记录，直到下一次 MOVE/CONFIG 受理 */
	/* 以下四项只作显示与诊断，不用来判断流程成败 */
	uint16_t statusword;
	int32_t position;
	int32_t velocity;
	uint16_t error_code;
};

/* ---- 内部数据分组（纯数据，无逻辑；分组只为可读性）---- */

/* 取消原因：谁让当前流程提前结束，决定要不要报失败 */
enum cia402_cancel_reason {
	CIA402_CANCEL_NONE = 0,
	CIA402_CANCEL_REQUEST, /* 被 Halt / Quick Stop 抢占：结论作废 */
	CIA402_CANCEL_TIMEOUT, /* 单笔事务超时：报失败 */
	CIA402_CANCEL_MANUAL   /* 外部取消：结论作废 */
};

/* 请求槽：跨线程，lock 保护 */
struct cia402_slot {
	pthread_mutex_t lock;
	struct cia402_request req; /* 待处理 */
	uint8_t has_req;
	struct cia402_request running; /* 在执行 */
	uint8_t busy;                  /* running 有效 */
	uint8_t cancel;                /* enum cia402_cancel_reason */
};

/* 事实：poll 整块拷出去 */
struct cia402_facts {
	enum cia402_phase phase;
	enum cia402_result result;
	enum cia402_fail fail;
	uint32_t abort_code;
	uint8_t configured;
	uint8_t enabled;
	uint8_t failed;
	uint16_t statusword;
	int32_t position;
	int32_t velocity;
	uint16_t error_code;
};

/* 传输上下文：一笔事务的账，仅服务线程使用 */
struct cia402_xfer {
	uint8_t pending;          /* 有在途事务 */
	uint8_t rejected;         /* 上一笔发起被拒：下一拍补一个失败事件 */
	struct sdo_result result; /* 最近一笔结果 */
	uint32_t deadline;        /* 单笔事务期限 */
	uint16_t cw_ok;           /* 已确认的控制字 */
	uint16_t cw_next;         /* 在途的控制字 */
	uint8_t cw_next_valid;
};

/* 当前流程：仅服务线程使用 */
struct cia402_job {
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

/* 反馈轮转：仅服务线程使用 */
struct cia402_mon {
	uint8_t slot;
	uint32_t last_ms;
};

struct cia402_device {
	/* 装配后只读 */
	const struct sdo_access *access;
	uint8_t node_id;

	/* 状态机：当前状态由 hsm 维护 */
	struct sm_hsm hsm;

	/* 服务线程每拍写入，用于算期限 */
	uint32_t now_ms;

	struct cia402_slot slot;   /* 跨线程：请求槽 */
	struct cia402_facts facts; /* 跨线程：事实快照（slot.lock 保护写）*/
	struct cia402_xfer xfer;   /* 仅服务线程 */
	struct cia402_job job;     /* 仅服务线程 */
	struct cia402_mon mon;     /* 仅服务线程 */
};

/* ---- 三个动词 ---- */

void cia402_init(struct cia402_device *d, const struct sdo_access *access, uint8_t node_id);

/*
 * 投递一条请求。立即返回，不阻塞。
 * HALT / QUICK_STOP 会抢占在途流程；其余请求在忙时返回 CIA402_BUSY。
 */
enum cia402_ret cia402_request(struct cia402_device *d, const struct cia402_request *req);

/*
 * 推进一拍；out 非空时把这一刻的事实拷出去（在事件分发之后填，
 * 因此本拍刚产生的结论立刻可见）。仅服务线程调用。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms, struct cia402_status *out);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_H */
