/*
 * cia402/device.h — 一个 CiA 402 从站的非阻塞控制上下文
 *
 * cia402_request() 只受理命令，cia402_poll() 逐拍推进流程。
 * 对外只交操作结局与失败诊断；驱动器状态由控制器读取。
 *
 * 一个实例同时最多一个操作流程、一笔对象事务。邮箱只有一个待办槽，
 * 不是请求队列。想同步等待的调用方自己轮询 poll 与操作结论。
 *
 * Author: zager
 */

#ifndef CIA402_DEVICE_H
#define CIA402_DEVICE_H

#include <sdo/sdo_client.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 请求 ---- */

/* 一类请求对应一条流程 */
enum cia402_kind {
	CIA402_REQ_CONFIG = 0,  /* 写模式并回读确认 */
	CIA402_REQ_ENABLE,      /* PDS 使能链 */
	CIA402_REQ_DISABLE,     /* 断使能 */
	CIA402_REQ_MOVE,        /* PP 绝对位置（参数在 req.pp 里）*/
	CIA402_REQ_HALT,        /* 抢占：减速停并保持使能 */
	CIA402_REQ_QUICK_STOP,  /* 抢占：只写一次快速停，写完不再发帧 */
	CIA402_REQ_ESTOP_CLEAR, /* 解除急停：0x0F 让驱动器回可操作 */
	CIA402_REQ_HOME         /* 回原点：HM 模式跑驱动器自己的回零流程 */
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

/* 一次操作的结局；取消不交付结局，只记日志 */
enum cia402_outcome {
	CIA402_OUTCOME_NONE = 0, /* 正在执行，或当前没有可交付的结果 */
	CIA402_OUTCOME_DONE,
	CIA402_OUTCOME_FAILED
};

/* 仅用于解释已经发生的执行失败，不用于拒绝或取消 */
enum cia402_fail {
	CIA402_FAIL_TIMEOUT = 0,
	CIA402_FAIL_DRIVER,
	CIA402_FAIL_HOMING,
	CIA402_FAIL_SDO,
	CIA402_FAIL_CHECK
};

/* 实例存储定义见 device_instance.h；普通调用方只使用不透明指针 */
struct cia402_device;

/* 对外接口 */

void cia402_init(struct cia402_device *d, struct sdo_client *client);

/*
 * 投递一条请求。立即返回，不阻塞。
 * 只回答收不收：HALT / QUICK_STOP 会抢占在途流程，其余请求在忙时返回 CIA402_BUSY。
 * 流程顺序由调用方负责。
 */
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);

/* 推进一拍；只推进，不交付结果。仅 CANopen 工作线程调用。 */
void cia402_poll(struct cia402_device *d, uint32_t now_ms);

/* 是否还有已受理的流程（含未出邮箱）需要执行或取消清理 */
int cia402_is_running(const struct cia402_device *d);

/* 失败时单独读取归类；只在 FAILED 后有意义，NULL 入参不写。 */
void cia402_failure_of(const struct cia402_device *d, enum cia402_fail *out);

/* 取走一次操作结局；取消不会生成结局。 */
enum cia402_outcome cia402_take_result(struct cia402_device *d);

#ifdef __cplusplus
}
#endif

#endif /* CIA402_DEVICE_H */
