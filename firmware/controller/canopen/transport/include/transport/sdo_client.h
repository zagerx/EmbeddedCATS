/*
 * sdo_client.h — 单事务异步 SDO 客户端接口
 *
 * 读写只负责发起，协议推进由 CANopen 工作循环的 step 完成。
 * ACCEPTED 后必须 poll 到 DONE；BUSY / INVALID 不产生结果。
 * 取消也异步收尾，普通取消状态不是调用方的业务操作失败。
 *
 * Author: zager
 */

#ifndef TRANSPORT_SDO_CLIENT_H
#define TRANSPORT_SDO_CLIENT_H

#include <transport/sdo_types.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sdo_client;

enum sdo_start_result sdo_client_read(struct sdo_client *s, uint16_t index, uint8_t subindex,
				      uint32_t timeout_ms);
enum sdo_start_result sdo_client_write(struct sdo_client *s, uint16_t index, uint8_t subindex,
				       uint32_t value, uint8_t size, uint32_t timeout_ms);
enum sdo_poll sdo_client_poll(struct sdo_client *s, struct sdo_result *result);
void sdo_client_abort(struct sdo_client *s);
void sdo_client_step(struct sdo_client *s, uint32_t dt_us, uint32_t *timer_next_us);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_SDO_CLIENT_H */
