/*
 * sdo_client.c — SDO 加速读：发一帧请求，等一帧应答
 *
 * 整段逻辑只有四步：
 *   1. 拼请求帧（cs=0x40 表示"发起上传"，即读）
 *   2. 记下开始时刻
 *   3. 循环收帧：不是这个节点的应答就丢掉，是 abort 就上抛，是上传响应就解析
 *   4. 每次收帧前先算剩余时间，用完就 TIMEOUT
 */

#include <canopen/sdo_client.h>

#define SDO_CS_UPLOAD 0x40u  /* 请求字节：发起上传（读） */
#define SDO_SCS_UPLOAD 0x40u /* 响应字节高 3 位：上传响应 */
#define SDO_SCS_ABORT 0x80u  /* 响应字节：abort */
#define SDO_E_BIT 0x02u /* 加速传输标志：0 表示后面还有分段，本阶段不支持 */

void co_sdo_init(co_sdo_client_t *c, can_io_t *io, uint32_t timeout_ms) {
  c->io = io;
  c->timeout_ms = timeout_ms;
  c->last_abort = 0;
}

co_sdo_result_t co_sdo_read(co_sdo_client_t *c, uint8_t node_id, uint16_t index,
                            uint8_t sub, co_sdo_value_t *out) {
  can_io_frame_t req;
  can_io_frame_t rsp;
  const uint32_t cob_req = 0x600u + node_id; /* 我们 → 从站 */
  const uint32_t cob_rsp = 0x580u + node_id; /* 从站 → 我们 */
  uint32_t start;
  uint32_t used;
  uint8_t i;

  c->last_abort = 0;

  /* 1. 请求帧：索引/子索引都是小端（低字节在前） */
  req.id = cob_req;
  req.dlc = 8;
  req.data[0] = SDO_CS_UPLOAD;
  req.data[1] = (uint8_t)(index & 0xFFu);
  req.data[2] = (uint8_t)(index >> 8);
  req.data[3] = sub;
  req.data[4] = 0;
  req.data[5] = 0;
  req.data[6] = 0;
  req.data[7] = 0;

  if (can_io_send(c->io, &req) != CAN_IO_OK) {
    return CO_SDO_ERR_IO;
  }

  /* 2. 记开始时刻；后面用"无符号相减"算已用时间，天然处理 32 位回绕 */
  start = can_io_now_ms(c->io);

  for (;;) {
    used = can_io_now_ms(c->io) - start;
    if (used >= c->timeout_ms) {
      return CO_SDO_TIMEOUT;
    }

    switch (can_io_recv(c->io, &rsp, c->timeout_ms - used)) {
    case CAN_IO_OK:
      break;
    case CAN_IO_TIMEOUT:
      return CO_SDO_TIMEOUT;
    default:
      return CO_SDO_ERR_IO;
    }

    /* 总线上会混进别人的帧（另一个节点、SYNC、回显），不是给我们的就继续等 */
    if (rsp.id != cob_rsp) {
      continue;
    }

    /* 3a. abort：4 字节小端错误码在 data[4..7] */
    if ((rsp.data[0] & 0xE0u) == SDO_SCS_ABORT) {
      c->last_abort = (uint32_t)rsp.data[4] | ((uint32_t)rsp.data[5] << 8) |
                      ((uint32_t)rsp.data[6] << 16) |
                      ((uint32_t)rsp.data[7] << 24);
      return CO_SDO_ABORT;
    }

    /* 3b. 必须是"上传响应" */
    if ((rsp.data[0] & 0xE0u) != SDO_SCS_UPLOAD) {
      return CO_SDO_ERR_PROTOCOL;
    }
    /* 索引/子索引要对得上，排除上一条请求的迟到应答 */
    if (rsp.data[1] != (uint8_t)(index & 0xFFu) ||
        rsp.data[2] != (uint8_t)(index >> 8) || rsp.data[3] != sub) {
      return CO_SDO_ERR_PROTOCOL;
    }
    /* 3c. e=0 说明是分段传输，本阶段不支持 */
    if ((rsp.data[0] & SDO_E_BIT) == 0) {
      return CO_SDO_ERR_PROTOCOL;
    }

    /* 3d. 解析：n 位给出"有几个字节无效"，长度 = 4 - n */
    out->size = (uint8_t)(4u - ((rsp.data[0] >> 2) & 0x03u));
    out->value = 0;
    for (i = 0; i < out->size; i++) {
      out->value |= (uint32_t)rsp.data[4 + i] << (8 * i);
    }
    return CO_SDO_OK;
  }
}
