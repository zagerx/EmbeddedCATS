/*
 * sdo_access — SDO 类对象访问契约。
 *
 * 语义就是 SDO 的语义：**请求—响应、按 index/sub 寻址、可阻塞**，
 * 失败原因是 abort 码。实现可以是 CANopen SDO，也可以是别的总线上
 * 同语义的服务（如 CoE SDO）。
 *
 * 周期性映射（PDO 一类）不属于本接口：它没有 index/sub 寻址、
 * 没有请求—响应、也没有 abort 码，需要另立接口。
 *
 * 读写是阻塞的，只能由服务线程调用。
 */

#ifndef TRANSPORT_SDO_ACCESS_H
#define TRANSPORT_SDO_ACCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_dir {
	SDO_DIR_READ = 0,
	SDO_DIR_WRITE = 1,
};

struct sdo_access {
	/*
	 * 一次 SDO 事务（阻塞）：
	 *   dir = SDO_DIR_READ ：结果写进 out_value/out_len
	 *   dir = SDO_DIR_WRITE：把 in_value（size 字节，小端）写下去
	 * 返回 0 成功；-1 失败（原因读 last_error）。
	 */
	int (*xfer)(void *ctx, uint8_t dir, uint16_t index, uint8_t sub, uint32_t in_value,
		    uint8_t size, uint32_t *out_value, uint8_t *out_len, uint32_t timeout_ms);
	/* 立即唤醒等待方并作废当前事务。 */
	void (*abort)(void *ctx);
	uint32_t (*now_ms)(void *ctx);
	/* 最近一次 abort 码；超时/总线错返回 0。 */
	uint32_t (*last_error)(void *ctx);
	void *ctx;
};

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_ACCESS_H */
