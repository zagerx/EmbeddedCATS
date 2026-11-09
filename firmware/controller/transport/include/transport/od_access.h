/*
 * 对象字典访问抽象。读写采用阻塞语义，只能由服务线程调用。
 * 实现可以是 CANopen SDO，也可以是其他支持对象字典的传输方式。
 */

#ifndef TRANSPORT_OD_ACCESS_H
#define TRANSPORT_OD_ACCESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_access {
	/* 0 = 成功；-1 = 超时、abort 或总线错误。 */
	int (*read)(void *ctx, uint16_t index, uint8_t sub, uint32_t *value, uint8_t *len,
		    uint32_t timeout_ms);
	int (*write)(void *ctx, uint16_t index, uint8_t sub, uint32_t value, uint8_t size,
		     uint32_t timeout_ms);
	/* 立即唤醒等待方并作废当前事务。 */
	void (*abort)(void *ctx);
	uint32_t (*now_ms)(void *ctx);
	/* 最近一次 SDO abort；其他错误返回 0。 */
	uint32_t (*last_error)(void *ctx);
	void *ctx;
};

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_OD_ACCESS_H */
