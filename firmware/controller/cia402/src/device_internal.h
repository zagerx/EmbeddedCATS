/* device.c、device_seq.c 与 device_pp.c 之间的私有状态机接口。 */

#ifndef CIA402_DEVICE_INTERNAL_H
#define CIA402_DEVICE_INTERNAL_H

#include <cia402/device.h>
#include <cia402/od.h>
#include <cia402/pds.h>

#define CIA402_DEVICE_SDO_TIMEOUT_MS 200u
#define CIA402_DEVICE_MON_TIMEOUT_MS 100u
#define CIA402_DEVICE_MON_INTERVAL_MS 50u
#define CIA402_DEVICE_STEP_TIMEOUT_MS 300u
#define CIA402_DEVICE_HALT_TIMEOUT_MS 3000u
#define CIA402_DEVICE_QUICK_STOP_TIMEOUT_MS 5000u
#define CIA402_DEVICE_PP_ARRIVE_TIMEOUT_MS 5000u

enum cia402_cancel_reason {
	CIA402_CANCEL_NONE = 0,
	CIA402_CANCEL_REQUEST,
	CIA402_CANCEL_TIMEOUT,
	CIA402_CANCEL_MANUAL
};

enum cia402_state {
	CIA402_ST_IDLE = 0,
	CIA402_ST_CONFIG_WRITE_MODE,
	CIA402_ST_CONFIG_READ_MODE,
	CIA402_ST_ENABLE_READ_PDS,
	CIA402_ST_ENABLE_RESET_LOW,
	CIA402_ST_ENABLE_RESET_HIGH,
	CIA402_ST_ENABLE_READ_RESET,
	CIA402_ST_ENABLE_WRITE_SHUTDOWN,
	CIA402_ST_ENABLE_READ_READY,
	CIA402_ST_ENABLE_WRITE_SWITCH_ON,
	CIA402_ST_ENABLE_READ_SWITCHED_ON,
	CIA402_ST_ENABLE_WRITE_ENABLE_OP,
	CIA402_ST_ENABLE_READ_ENABLED,
	CIA402_ST_DISABLE_WRITE,
	CIA402_ST_DISABLE_READ,
	CIA402_ST_PP_WRITE_VELOCITY,
	CIA402_ST_PP_WRITE_ACCEL,
	CIA402_ST_PP_WRITE_DECEL,
	CIA402_ST_PP_WRITE_TARGET,
	CIA402_ST_PP_TRIGGER_HIGH,
	CIA402_ST_PP_TRIGGER_LOW,
	CIA402_ST_PP_READ_STATUS,
	CIA402_ST_PP_READ_POSITION,
	CIA402_ST_HALT_WRITE,
	CIA402_ST_HALT_READ,
	CIA402_ST_HALT_CLEAR,
	CIA402_ST_QUICK_STOP_WRITE,
	CIA402_ST_QUICK_STOP_READ,
	CIA402_ST_QUICK_STOP_DISABLE,
	CIA402_ST_MONITOR_READ
};

void cia402_device_start_read(struct cia402_device *d, enum cia402_state state, uint16_t index,
			      uint32_t timeout_ms);
void cia402_device_start_write(struct cia402_device *d, enum cia402_state state, uint16_t index,
			       uint32_t value, uint8_t size, uint32_t timeout_ms);
void cia402_device_finish(struct cia402_device *d, int ok, uint32_t abort_code);

void cia402_device_seq_begin(struct cia402_device *d, enum cia402_device_req req);
void cia402_device_seq_complete(struct cia402_device *d, const struct sdo_result *result);
int cia402_device_seq_on_failure(struct cia402_device *d);

void cia402_device_pp_begin(struct cia402_device *d);
void cia402_device_pp_complete(struct cia402_device *d, const struct sdo_result *result);

#endif /* CIA402_DEVICE_INTERNAL_H */
