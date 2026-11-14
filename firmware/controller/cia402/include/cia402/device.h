/*
 * cia402/device.h — 一个 CiA 402 从站的非阻塞控制上下文
 *
 * cia402_request() 只受理命令，cia402_poll() 逐拍推进流程。
 * status 是主站根据通信流程作出的确认与结论，不是驱动器实时状态的完整镜像。
 * 外部反馈读取由控制器直接发起，本模块不缓存或轮询反馈。
 *
 * 一个实例同时最多一个操作流程、一笔对象事务。邮箱只有一个待办槽，
 * 不是请求队列。想同步等待的调用方自己轮询 poll 与操作结论。
 *
 * Author: zager
 */

#ifndef CIA402_DEVICE_H
#define CIA402_DEVICE_H

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

/* 主站确认结果：依据已完成的通信流程得出，不是实时设备状态 */
struct cia402_status {
	enum cia402_phase phase;  /* 主站当前执行的操作；IDLE = 没有流程在跑 */
	uint8_t mode_confirmed;   /* 已写 6060 并回读 6061 确认 PP；不保证此后模式未变 */
	uint8_t enable_confirmed; /* 使能流程曾确认成功，且此后没有确认断使能 */
	uint8_t operation_failed; /* 主站操作失败记录；下一次 MOVE/CONFIG 受理时清零 */

	/* 最近一次主站操作的回执，不等同于驱动器故障状态 */
	struct {
		enum cia402_operation_result result;
		enum cia402_operation_fail fail; /* result == CIA402_OP_FAILED 时的主站归类 */
		uint32_t abort_code;             /* 对应 SDO abort 码；0 = 没有 */
	} last;
};

/* 实例存储定义见 device_instance.h；普通调用方只使用不透明指针 */
struct cia402_device;

/* 对外接口 */

void cia402_init(struct cia402_device *d, const struct sdo_access *access);

/*
 * 投递一条请求。立即返回，不阻塞。
 * HALT / QUICK_STOP 会抢占在途流程；其余请求在忙时返回 CIA402_BUSY。
 */
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);

/*
 * 推进一拍；out 非空时在事件分发后拷贝主站确认结果，本拍结论立刻可见。
 * 仅 CANopen 工作线程调用。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms, struct cia402_status *out);

/* 读取主站确认结果；跨线程查询应由控制层发布快照。NULL 入参安全。 */
void cia402_get_status(const struct cia402_device *d, struct cia402_status *out);

/* 取走一次操作回执；取走后 result 为 NONE，失败诊断保留。 */
enum cia402_operation_result cia402_take_result(struct cia402_device *d);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_H */
