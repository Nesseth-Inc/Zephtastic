#define ONE_DAY 24 * 60 * 60
#define MAX_INTERVAL INT32_MAX // FIXME: INT32_MAX to avoid overflow issues with Apple clients but should be UINT32_MAX
#define default_gps_update_interval IF_ROUTER(ONE_DAY, 2 * 60)
#define default_telemetry_broadcast_interval_secs IF_ROUTER(ONE_DAY / 2, 60 * 60)
#define default_broadcast_interval_secs IF_ROUTER(ONE_DAY / 2, 60 * 60)
#define default_wait_bluetooth_secs IF_ROUTER(1, 60)
#define default_sds_secs IF_ROUTER(ONE_DAY, UINT32_MAX) // Default to forever super deep sleep
#define default_ls_secs IF_ROUTER(ONE_DAY, 5 * 60)
#define default_min_wake_secs 10
#define default_screen_on_secs IF_ROUTER(1, 60 * 10)
#define IF_ROUTER(routerVal, normalVal)                                                                                          \
    ((config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||                                                        \
      config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE)                                                     \
         ? (routerVal)                                                                                                           \
         : (normalVal))

