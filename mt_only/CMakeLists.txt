cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_SOURCE_DIR}/native-driver.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/ble-native-store.cmake)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
include(${CMAKE_CURRENT_SOURCE_DIR}/gnss-native.cmake)
project(mt_zephyr_baseline LANGUAGES C CXX)
include(${CMAKE_CURRENT_SOURCE_DIR}/M119PsaPremasterOverlay.cmake)
set(MT_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore/adapters/bench/vendor)
set(MONO ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore/lib/monocypher)
target_include_directories(app PRIVATE src ${MT_VENDOR} ${MONO} ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore/helpers)
target_sources(app PRIVATE src/main.cpp src/BleTransport.cpp src/UsbBootloader.cpp src/ZephyrHardware.cpp
  src/security/ConsoleSecurity.cpp
  src/notifications/ZephyrNotification.cpp ../zephcore/helpers/led_gate.c
  src/network/WifiRuntime.cpp src/network/TcpTransport.cpp
  ../zephcore/helpers/battery_curve.c ${MT_VENDOR}/MtCodec.cpp ${MT_VENDOR}/MtSignature.cpp
  ${MONO}/monocypher.c ${MONO}/monocypher-ed25519.c)
# M128 code-size experiment: use Monocypher's existing fixed 12-round loop
# for BLAKE2b in this source file only; algorithm and public interfaces stay unchanged.
set_property(SOURCE "${MONO}/monocypher.c" TARGET_DIRECTORY app
  APPEND PROPERTY COMPILE_DEFINITIONS BLAKE2_NO_UNROLLING)
if(CONFIG_MT_PAGED_SATELLITES)
  set(mt_lfs_adapter "${ZEPHYR_BASE}/subsys/fs/littlefs_fs.c")
  file(SHA256 "${mt_lfs_adapter}" mt_lfs_sha)
  if(NOT mt_lfs_sha STREQUAL "871bcca21b08c4802c2e1a37c608115fa72123113254034b21fba43a4bc1cc3a")
    message(FATAL_ERROR "Check the LittleFS recovery wrappers against the new Zephyr adapter")
  endif()
  set_property(SOURCE "${mt_lfs_adapter}" DIRECTORY "${ZEPHYR_BASE}/subsys/fs"
    APPEND PROPERTY COMPILE_OPTIONS "-include${CMAKE_CURRENT_SOURCE_DIR}/src/companion/PrivateLittlefsRecovery.h")
endif()
target_compile_features(app PRIVATE cxx_std_17)
# The composed Companion dispatch in this TU exceeds the Xtensa separate
# literal-pool range. Keep its literals beside the code on ESP32-S3 only.
if(CONFIG_SOC_ESP32S3)
  zephyr_check_compiler_flag(CXX "-mtext-section-literals" mt_xtensa_text_literals_supported)
  if(NOT mt_xtensa_text_literals_supported)
    message(FATAL_ERROR "ESP32-S3 Companion dispatch requires text-section literals")
  endif()
  set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" TARGET_DIRECTORY app
    APPEND PROPERTY COMPILE_OPTIONS -mtext-section-literals)
endif()
if(CONFIG_MT_SIGNED_BLE_OTA)
  target_sources(app PRIVATE src/update/SignedBleOta.cpp)
endif()
if(CONFIG_MT_GNSS_T096 OR CONFIG_MT_GNSS_XIAO_L76K)
  target_sources(app PRIVATE src/gnss/ZephyrGnss.cpp)
endif()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore/adapters/sensors)
target_sources_ifdef(CONFIG_MT_MAX30102_OPTICAL app PRIVATE src/stock/telemetry/max30102/ZephyrOptical.cpp src/stock/telemetry/max30102/MaximAlgorithm.cpp)
target_sources_ifdef(CONFIG_MT_ONE_WIRE_TELEMETRY app PRIVATE src/stock/telemetry/ZephyrOneWire.cpp)
target_sources_ifdef(CONFIG_MT_ACCESSORY_SENSORS app PRIVATE src/stock/telemetry/accessory/ZephyrAccessories.cpp)
target_sources_ifdef(CONFIG_MT_MLX90632 app PRIVATE src/stock/telemetry/mlx90632/ZephyrMlx90632.cpp)
target_sources_ifdef(CONFIG_MT_BME680_IAQ app PRIVATE src/stock/telemetry/bme680/ZephyrBme680.cpp
  src/stock/telemetry/bme680/BME680IaqEstimator.cpp)
target_sources_ifdef(CONFIG_MT_SCD4X_ADMIN app PRIVATE src/stock/telemetry/ZephyrScd4xAdmin.cpp)
target_sources_ifdef(CONFIG_MT_ENVIRONMENT_SENSORS app PRIVATE src/environment/ZephyrEnvironment.cpp
  ../zephcore/adapters/sensors/ZephyrEnvSensors.cpp)
target_sources_ifdef(CONFIG_MT_LOCAL_CLOCK app PRIVATE src/clock/LocalTime.cpp)
target_sources_ifdef(CONFIG_MT_DISPLAY app PRIVATE src/ui/Display.cpp)
target_sources_ifdef(CONFIG_MT_BASEUI_GAMES app PRIVATE
  src/stock/ui/games/Snake.cpp src/stock/ui/games/Tetris.cpp
  src/stock/ui/games/ChirpyRunner.cpp src/stock/ui/games/Breakout.cpp)
target_sources_ifdef(CONFIG_MT_SERIAL_MODULE app PRIVATE
  src/serial/SerialEngine.cpp src/serial/ZephyrSerial.cpp
  src/stock/peripherals/NMEAWPL.cpp src/serial/ProtoEngine.cpp src/serial/PositionOutput.cpp src/serial/Ws85Engine.cpp)
if(CONFIG_MT_WIFI_STATION)
  target_include_directories(app PRIVATE ../zephcore/adapters/wifi)
  target_sources(app PRIVATE ../zephcore/adapters/wifi/ZephyrWiFiStation.c)
endif()
if(CONFIG_MT_MQTT_GATEWAY)
  target_sources(app PRIVATE src/network/MqttRuntime.cpp
    src/network/ZephyrMqttPublisherDriver.cpp)
  if(CONFIG_MT_M119_TLS_TEST)
    target_sources(app PRIVATE src/network/M119TlsTestCa.cpp)
  elseif(CONFIG_MQTT_LIB_TLS)
    set(MT_MQTT_CA_DER "" CACHE FILEPATH "One deployment MQTT public self-signed root in DER format")
    set(_mt_ca "${MT_MQTT_CA_DER}")
    set(_mt_ca_sha)
    if(NOT _mt_ca)
      set(_mt_ca "${CMAKE_CURRENT_SOURCE_DIR}/certs/isrg-root-x1.der")
      set(_mt_ca_sha "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6")
    endif()
    set(_mt_ca_header "${CMAKE_CURRENT_BINARY_DIR}/mqtt-trust/mqtt_root_der.generated.h")
    add_custom_command(OUTPUT "${_mt_ca_header}"
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-mqtt-ca.py"
              "${_mt_ca}" "${_mt_ca_header}" ${_mt_ca_sha}
      DEPENDS "${_mt_ca}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-mqtt-ca.py" VERBATIM)
    target_sources(app PRIVATE src/network/MqttRootCa.cpp "${_mt_ca_header}")
    target_include_directories(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/mqtt-trust")
  endif()
endif()

set(MT_PROTO ${CMAKE_CURRENT_SOURCE_DIR}/../zephcore/bench/upstream-differential/source)
file(GLOB MT_PROTO_HEADERS ${MT_PROTO}/*.pb.h)
foreach(HEADER ${MT_PROTO_HEADERS})
  get_filename_component(NAME ${HEADER} NAME)
  configure_file(${HEADER} ${CMAKE_CURRENT_BINARY_DIR}/protobuf/meshtastic/${NAME} COPYONLY)
endforeach()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/protobuf ${MT_PROTO}/nanopb)
target_sources(app PRIVATE src/stock/platform/ZephyrWatchdog.cpp)
target_sources(app PRIVATE src/stock/modules/StatusMessageModule.cpp src/stock/position/PositionPrecision.cpp src/Protobuf.cpp ${MT_PROTO}/nanopb/pb_common.c ${MT_PROTO}/nanopb/pb_encode.c ${MT_PROTO}/nanopb/pb_decode.c)

include(${CMAKE_CURRENT_SOURCE_DIR}/ble-diagnostics.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/pki-groundwork.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/controller-lto-profile.cmake)

# BEGIN M104 native Settings admission. Narrow undefined Settings entries.
if(CONFIG_MT_BLE_NATIVE_STORE)
  target_sources(app PRIVATE src/BleSettingsAdmission.cpp)
  zephyr_ld_options(-Wl,--wrap=settings_save_one -Wl,--wrap=settings_delete)
endif()
# END M104 native Settings admission.

target_sources_ifdef(CONFIG_MT_STORE_FORWARD_PSRAM app PRIVATE
  src/modules/StoreForwardWire.cpp src/modules/StoreForwardMemory.cpp)
target_sources_ifdef(CONFIG_MT_GPIO_MODULES app PRIVATE src/modules/ZephyrGpioModule.cpp)

# M176 implementation batch. Optional hardware remains DT/capability gated.
target_sources(app PRIVATE src/modules/BeaconExcursion.cpp src/modules/BeaconTargetRuntime.cpp src/modules/TrafficNodeInfoResponder.cpp src/modules/RangeTestLog.cpp src/input/CannedGpioInput.cpp src/telemetry/CompletionTelemetry.cpp src/power/PowerPolicy.cpp
  src/modules/peripherals/ZephyrPeripherals.cpp src/modules/audio/AudioModule.cpp
  src/modules/audio/Codec2FrameCodec.cpp src/modules/audio/ZephyrI2sAudio.cpp
  src/notifications/ExtendedNotificationController.cpp src/notifications/ZephyrExtendedNotification.cpp)
target_sources_ifdef(CONFIG_MT_INA260 app PRIVATE src/power/ZephyrIna260.c)
target_sources_ifdef(CONFIG_MT_MAX17048 app PRIVATE src/power/ZephyrMax17048.cpp)
target_sources(app PRIVATE src/power/ZephyrPowerBackend.cpp src/power/InaBatterySelector.cpp src/update/ZephyrUpdateBackend.cpp
  src/pki/verification/KeyVerification.cpp src/pki/verification/KeyVerificationService.cpp)
if(CONFIG_ZEPHCORE_AUDIO_CODEC2)
  include(${CMAKE_CURRENT_SOURCE_DIR}/src/modules/audio/Codec2Dependency.cmake)
  target_link_libraries(app PRIVATE codec2)
endif()

if(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY)
  set_property(SOURCE "${MONO}/monocypher.c" "${MONO}/monocypher-ed25519.c" TARGET_DIRECTORY app
    APPEND PROPERTY COMPILE_DEFINITIONS MT_SMALL_CRYPTO_ROUNDS)
  # All app nanopb translation units use memory-backed streams and consume the
  # bool result. Keep the stream ABI identical in the codec and its callers.
  target_compile_definitions(app PRIVATE PB_BUFFER_ONLY PB_NO_ERRMSG)
  if(CONFIG_MT_BASEUI_EMOTES)
    set(emote_source ${CMAKE_CURRENT_SOURCE_DIR}/src/stock/ui/EmoteTable.inc)
    set(emote_packer ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pack-emotes.py)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${emote_source} ${emote_packer})
    execute_process(COMMAND ${Python3_EXECUTABLE} ${emote_packer} ${emote_source}
      ${CMAKE_CURRENT_BINARY_DIR}/mt_emote_table.h COMMAND_ERROR_IS_FATAL ANY)
    target_compile_definitions(app PRIVATE MT_PACKED_EMOTES)
  endif()
  set(notice_source ${CMAKE_CURRENT_SOURCE_DIR}/src/Companion.h)
  set(notice_packer ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pack-notices.py)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${notice_source} ${notice_packer})
  execute_process(COMMAND ${Python3_EXECUTABLE} ${notice_packer} ${notice_source}
    ${CMAKE_CURRENT_BINARY_DIR}/mt_notice_table.h COMMAND_ERROR_IS_FATAL ANY)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(CONFIG_MT_SIGNED_WIFI_OTA)
  if(NOT MT_OTA_PUBLIC_KEY OR NOT EXISTS "${MT_OTA_PUBLIC_KEY}")
    message(FATAL_ERROR "Signed XIAO OTA requires the MCUboot loader public PEM")
  endif()
  set(_ota_header "${CMAKE_CURRENT_BINARY_DIR}/MtReleaseKey.h")
  add_custom_command(OUTPUT "${_ota_header}"
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-ota-public.py "${MT_OTA_PUBLIC_KEY}" "${_ota_header}"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed-ota-public.py "${MT_OTA_PUBLIC_KEY}" VERBATIM)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_sources(app PRIVATE src/update/SignedOta.cpp src/update/SignedWifiOta.cpp "${_ota_header}")
endif()
