    // Zero out telemetry intervals so that they coalesce to defaults in Default.h
#ifdef USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL
    moduleConfig.telemetry.device_update_interval = USERPREFS_CONFIG_DEVICE_TELEM_UPDATE_INTERVAL;
#else
    moduleConfig.telemetry.device_update_interval = MAX_INTERVAL;
#endif

#ifdef USERPREFS_CONFIG_ENVIRONMENT_MEASUREMENT_ENABLED
    moduleConfig.telemetry.environment_measurement_enabled = USERPREFS_CONFIG_ENVIRONMENT_MEASUREMENT_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_ENV_TELEM_UPDATE_INTERVAL
    moduleConfig.telemetry.environment_update_interval = USERPREFS_CONFIG_ENV_TELEM_UPDATE_INTERVAL;
#else
    moduleConfig.telemetry.environment_update_interval = 0;
#endif

#ifdef USERPREFS_CONFIG_ENV_SCREEN_SCREEN_ENABLED
    moduleConfig.telemetry.environment_screen_enabled = USERPREFS_CONFIG_ENV_SCREEN_SCREEN_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_AQ_TELEM_UPDATE_INTERVAL
    moduleConfig.telemetry.air_quality_interval = USERPREFS_CONFIG_AQ_TELEM_UPDATE_INTERVAL;
#else
    moduleConfig.telemetry.air_quality_interval = 0;
#endif

#ifdef USERPREFS_CONFIG_AQ_MEASUREMENT_ENABLED
    moduleConfig.telemetry.air_quality_enabled = USERPREFS_CONFIG_AQ_MEASUREMENT_ENABLED;
#endif

#ifdef USERPREFS_CONFIG_AQ_SCREEN_ENABLED
    moduleConfig.telemetry.air_quality_screen_enabled = USERPREFS_CONFIG_AQ_SCREEN_ENABLED;
#endif

    moduleConfig.telemetry.power_update_interval = 0;
    moduleConfig.telemetry.health_update_interval = 0;
    moduleConfig.neighbor_info.update_interval = 0;
    moduleConfig.paxcounter.paxcounter_update_interval = 0;
