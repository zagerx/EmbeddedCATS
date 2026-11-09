/*
 * cob.c — COB-ID 算式（纯函数）
 *
 * 三段预定义连接集（DS 301）：NMT 0x000、SDO 0x580/0x600、心跳 0x700。
 * 算式和判定都只依赖站号，与"登记了哪几个站"无关。
 */

#include <canopen/cob.h>

#include <stddef.h>

#define COB_SDO_REQ_BASE 0x600u
#define COB_SDO_RSP_BASE 0x580u
#define COB_HB_BASE 0x700u
#define COB_SEG_SIZE 0x80u

uint16_t co_cob_sdo_req(uint8_t node_id)
{
	return (uint16_t)(COB_SDO_REQ_BASE + node_id);
}

uint16_t co_cob_sdo_rsp(uint8_t node_id)
{
	return (uint16_t)(COB_SDO_RSP_BASE + node_id);
}

uint16_t co_cob_hb(uint8_t node_id)
{
	return (uint16_t)(COB_HB_BASE + node_id);
}

int co_cob_is_sdo_rsp(uint16_t cob_id, uint8_t *node_id)
{
	uint8_t id = (uint8_t)(cob_id & 0x7Fu);

	if (cob_id < COB_SDO_RSP_BASE || cob_id >= COB_SDO_RSP_BASE + COB_SEG_SIZE) {
		return 0;
	}
	if (node_id != NULL) {
		*node_id = id;
	}
	return 1;
}

int co_cob_is_hb(uint16_t cob_id, uint8_t *node_id)
{
	uint8_t id = (uint8_t)(cob_id & 0x7Fu);

	if (cob_id < COB_HB_BASE || cob_id >= COB_HB_BASE + COB_SEG_SIZE) {
		return 0;
	}
	if (id == 0u) {
		return 0; /* 0x700 是 NMT 错误控制保留位，不是某个站的心跳 */
	}
	if (node_id != NULL) {
		*node_id = id;
	}
	return 1;
}
