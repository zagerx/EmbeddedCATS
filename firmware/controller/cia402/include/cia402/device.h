/*
 * cia402/device.h — 一个 CiA 402 从站的非阻塞控制上下文
 *
 * cia402_request() 只受理命令，cia402_poll() 逐拍推进流程。
 * 对外只交回执（struct cia402_operation_result）；驱动器状态由控制器读取。
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
	CIA402_REQ_QUICK_STOP, /* 抢占：只写一次快速停，写完不再发帧 */
	CIA402_REQ_ESTOP_CLEAR /* 解除急停：0x0F 让驱动器回可操作 */
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
	CIA402_INVALID       /* 请求本身不合法（kind、未知参数）*/
};

/* ---- poll 的输出 ---- */

/* 一次操作的结局；保持到下一次请求被受理 */
enum cia402_outcome {
	CIA402_OUTCOME_NONE = 0, /* 还没有结局：流程在跑，或刚装配 */
	CIA402_OUTCOME_DONE,
	CIA402_OUTCOME_FAILED
};

/* 失败原因：上层据此判断"重试有没有意义" */
enum cia402_fail {
	CIA402_FAIL_NONE = 0,
	CIA402_FAIL_TIMEOUT, /* 步骤超时：驱动器没按期响应 */
	CIA402_FAIL_DRIVER,  /* 6041 报故障，复位也没成功 */
	CIA402_FAIL_SDO,     /* 对象访问被 abort（见 abort_code）*/
	CIA402_FAIL_CHECK    /* 回读值不符（例如模式没设进去）*/
};

/*
 * 一次操作的回执。
 *
 * 当前模式、PDS 状态、位置速度等事实由上层对象读取获得；这里只有主站
 * 自己的操作结果，不等同于驱动器的 Fault 状态。
 */
struct cia402_operation_result {
	enum cia402_outcome outcome;
	enum cia402_fail fail; /* outcome == CIA402_OUTCOME_FAILED 时的主站归类 */
	uint32_t abort_code;   /* 对应 SDO abort 码；0 = 没有 */
};

/* 实例存储定义见 device_instance.h；普通调用方只使用不透明指针 */
struct cia402_device;

/* 对外接口 */

void cia402_init(struct cia402_device *d, const struct sdo_access *access);

/*
 * 投递一条请求。立即返回，不阻塞。
 * 只回答收不收：HALT / QUICK_STOP 会抢占在途流程，其余请求在忙时返回 CIA402_BUSY。
 * 流程顺序由调用方负责。
 */
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);

/*
 * 推进一拍；out 非空时在事件分发后拷贝回执，本拍结论立刻可见。
 * 仅 CANopen 工作线程调用。
 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms, struct cia402_operation_result *out);

/* 读取最近一次操作的回执（不消费）；跨线程查询应由控制层发布快照。NULL 入参安全。 */
void cia402_get_result(const struct cia402_device *d, struct cia402_operation_result *out);

/* 取走一次操作回执；取走后 outcome 为 NONE，失败诊断保留。 */
enum cia402_outcome cia402_take_result(struct cia402_device *d);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_H */
