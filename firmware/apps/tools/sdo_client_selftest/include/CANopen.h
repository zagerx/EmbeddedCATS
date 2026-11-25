/* 仅用于 sdo_client_selftest 的 CANopenNode 协议接口桩 */
#ifndef CANOPEN_H
#define CANOPEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool bool_t;

typedef enum {
	CO_SDO_RT_transmittBufferFull = 3,
	CO_SDO_RT_waitingResponse = 1,
	CO_SDO_RT_ok_communicationEnd = 0,
	CO_SDO_RT_abort = -1,
	CO_SDO_RT_endedWithClientAbort = -9
} CO_SDO_return_t;

typedef enum {
	CO_SDO_AB_NONE = 0u,
	CO_SDO_AB_TIMEOUT = 0x05040000u
} CO_SDO_abortCode_t;

typedef struct {
	CO_SDO_return_t initiate;
	CO_SDO_return_t next;
	CO_SDO_abortCode_t abort_code;
	uint32_t read_value;
	unsigned closes;
} CO_SDOclient_t;

CO_SDO_return_t CO_SDOclientDownloadInitiate(CO_SDOclient_t *client, uint16_t index,
					     uint8_t subindex, size_t size, uint16_t timeout_ms,
					     bool block);
CO_SDO_return_t CO_SDOclientUploadInitiate(CO_SDOclient_t *client, uint16_t index, uint8_t subindex,
					   uint16_t timeout_ms, bool block);
CO_SDO_return_t CO_SDOclientDownload(CO_SDOclient_t *client, uint32_t dt_us, bool abort, bool block,
				     CO_SDO_abortCode_t *abort_code, size_t *size,
				     uint32_t *timer_next_us);
CO_SDO_return_t CO_SDOclientUpload(CO_SDOclient_t *client, uint32_t dt_us, bool abort,
				   CO_SDO_abortCode_t *abort_code, size_t *size, size_t *read_size,
				   uint32_t *timer_next_us);
size_t CO_SDOclientUploadBufRead(CO_SDOclient_t *client, uint8_t *data, size_t size);
size_t CO_SDOclientDownloadBufWrite(CO_SDOclient_t *client, const uint8_t *data, size_t size);
void CO_SDOclientClose(CO_SDOclient_t *client);

#ifdef __cplusplus
}
#endif

#endif /* CANOPEN_H */
