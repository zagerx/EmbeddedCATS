/*
 * sdo_client.h — CANopenNode SDO 客户端的异步封装
 *
 * 服务线程提交请求并取结果，I/O 线程独占地推进 CANopenNode。
 * 每条通道同时最多一笔事务，取消也生成一笔可取走的结果。
 */

#ifndef TRANSPORT_SDO_CLIENT_H
#define TRANSPORT_SDO_CLIENT_H

#include <transport/sdo_access.h>

#include "CANopen.h"

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sdo_client_state {
	SDO_CLIENT_IDLE = 0,
	SDO_CLIENT_QUEUED,
	SDO_CLIENT_RUNNING,
	SDO_CLIENT_DONE
};

struct sdo_client {
	CO_SDOclient_t *co;
	struct sdo_access access;

	pthread_mutex_t lock;
	uint8_t dir;
	uint8_t sub;
	uint8_t size;
	uint16_t index;
	uint16_t txn_ms;
	uint32_t value;

	uint8_t state;
	uint8_t cancel;
	struct sdo_result result;
	uint32_t last_us;
};

void sdo_client_init(struct sdo_client *s, CO_SDOclient_t *client);
void sdo_client_io_poll(struct sdo_client *s, uint32_t now_ms);
void sdo_client_abort(struct sdo_client *s);
const struct sdo_access *sdo_client_access(const struct sdo_client *s);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_CLIENT_H */
