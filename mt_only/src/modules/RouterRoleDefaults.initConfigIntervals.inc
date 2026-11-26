#ifdef USERPREFS_CONFIG_GPS_UPDATE_INTERVAL
    config.position.gps_update_interval = USERPREFS_CONFIG_GPS_UPDATE_INTERVAL;
#else
    config.position.gps_update_interval = default_gps_update_interval;
#endif

#ifdef USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL
    config.position.position_broadcast_secs = USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL;
#else
    config.position.position_broadcast_secs = default_broadcast_interval_secs;
#endif

    config.power.ls_secs = default_ls_secs;
    config.power.min_wake_secs = default_min_wake_secs;
    config.power.sds_secs = default_sds_secs;
    config.power.wait_bluetooth_secs = default_wait_bluetooth_secs;

    config.display.screen_on_secs = default_screen_on_secs;

#if defined(USE_POWERSAVE)
    config.power.is_power_saving = true;
    config.display.screen_on_secs = 30;
    config.power.wait_bluetooth_secs = 30;
#endif
