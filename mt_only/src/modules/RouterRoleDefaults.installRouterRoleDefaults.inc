    if (role == meshtastic_Config_DeviceConfig_Role_ROUTER) {
        initConfigIntervals();
        initModuleConfigIntervals();
        moduleConfig.telemetry.device_update_interval = default_telemetry_broadcast_interval_secs;
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY;
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
    }
