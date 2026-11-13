/*
 * sdo_client.h — CANopenNode SDO 客户端的异步封装
 *
 * 请求、推进与结果收取均属于同一个 CANopen 工作线程，不使用互斥锁。
 * 每条通道同时最多一笔事务，取消也生成一笔可取走的结果。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_CLIENT_H
#define TRANSPORT_SDO_CLIENT_H

#include <transport/sdo_access.h>

#include "CANopen.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_client_state {
	SDO_CLIENT_IDLE = 0,
	SDO_CLIENT_RUNNING,
	SDO_CLIENT_DONE
};

struct sdo_client {
	CO_SDOclient_t *co;
	struct sdo_access access;
	uint8_t dir;
	uint8_t state;
	uint8_t cancel;
	struct sdo_result result;
};

void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client);
void sdo_client_step(struct sdo_client *s, uint32_t dt_us, uint32_t *timer_next_us);
void sdo_client_abort(struct sdo_client *s);
const struct sdo_access *sdo_client_access(const struct sdo_client *s);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_CLIENT_H */
