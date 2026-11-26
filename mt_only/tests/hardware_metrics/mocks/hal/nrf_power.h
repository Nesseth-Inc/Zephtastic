#ifndef MOCK_NRF_POWER_H
#define MOCK_NRF_POWER_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {int identity;} NRF_POWER_Type;
extern NRF_POWER_Type mock_nrf_power;
typedef struct {uint32_t DEVICEID[2],DEVICEADDR[2];} mock_ficr_t;
extern const mock_ficr_t mock_nrf_ficr;
#define NRF_FICR (&mock_nrf_ficr)
#define NRF_POWER (&mock_nrf_power)
bool nrf_power_usbregstatus_vbusdet_get(const NRF_POWER_Type *power);
#ifdef __cplusplus
}
#endif
#endif
