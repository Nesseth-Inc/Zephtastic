# Target compile/link evidence: injected runtime has no default provider or fixture keys.
if(CONFIG_MT_LTO_RUNTIME)
  if(NOT CONFIG_BOARD_HELTEC_T096 OR NOT CONFIG_LTO OR NOT CONFIG_ISR_TABLES_LOCAL_DECLARATION OR
     NOT CONFIG_MT_PKI_STOCK_TRIAL OR NOT CONFIG_MT_BLE_NATIVE_STORE)
    message(FATAL_ERROR "Partial LTO requires the exact T096 stock-PKI native-store profile")
  endif()
endif()
if(CONFIG_MT_PKI_GROUNDWORK)
  if((CONFIG_LTO AND NOT CONFIG_MT_LTO_RUNTIME) OR CONFIG_MBEDTLS_PSA_CRYPTO_STORAGE_C)
    message(FATAL_ERROR "PKI groundwork refuses unqualified LTO and PSA persistent key storage")
  endif()
  target_include_directories(app PRIVATE src/pki experimental/pki)
  target_sources(app PRIVATE src/pki/PsaPkiBackend.cpp src/pki/PkiSession.cpp src/pki/PkiIdentity.cpp
    src/pki/PkiLinkProbe.cpp src/pki/PkiRuntimeLinkProbe.cpp src/pki/PkiRuntime.cpp
    src/pki/PkiLedger.cpp src/pki/PkiSigner.cpp experimental/pki/PkiCrypto.cpp experimental/pki/PkiAck.cpp)
  # Load-only qualification images retain every backend/session/proof method.
  # The stock runtime references its actual providers directly; forcing the
  # uncalled qualification tables there also keeps unused probe-only code.
  if(NOT CONFIG_MT_PKI_STOCK_TRIAL)
    target_link_options(app PUBLIC -Wl,--undefined=mt_pki_linkage -Wl,--undefined=mt_pki_runtime_linkage)
  endif()
  # Keep the public type-size receipt for both qualification and runtime audits.
  target_link_options(app PUBLIC -Wl,--undefined=mt_pki_runtime_sizes)
  # Preserve per-TU options already supplied by the application composition,
  # including main.cpp's ESP32-S3 literal placement requirement.
  set_property(SOURCE src/pki/PsaPkiBackend.cpp src/pki/PkiSession.cpp src/pki/PkiIdentity.cpp
    src/pki/PkiRuntime.cpp src/pki/PkiLedger.cpp src/pki/PkiSigner.cpp src/main.cpp
    experimental/pki/PkiCrypto.cpp experimental/pki/PkiAck.cpp
    APPEND PROPERTY COMPILE_OPTIONS "-fstack-usage")
endif()

if(CONFIG_MT_SHARED_MBEDTLS_THREADING)
  if(NOT CONFIG_MULTITHREADING OR NOT CONFIG_MBEDTLS_INIT OR CONFIG_KERNEL_INIT_PRIORITY_DEFAULT LESS_EQUAL 39)
    message(FATAL_ERROR "Shared Mbed TLS threading requires multithreading and an installer before Zephyr TLS/PSA init")
  endif()
  if(NOT CONFIG_MBEDTLS_USER_CONFIG_ENABLE OR
     NOT CONFIG_MBEDTLS_USER_CONFIG_FILE STREQUAL "mt_pki_psa_threading_config.h" OR
     NOT CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE STREQUAL "mt_pki_psa_threading_config.h")
    message(FATAL_ERROR "Zephyr Mbed TLS and TF-PSA must consume the same shared threading config")
  endif()
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/pki/psa-threading)
  target_sources(app PRIVATE src/pki/psa-threading/ZephyrPsaThreading.c)
endif()

if(CONFIG_MT_PKI_TARGET_RUNTIME)
  if(NOT CONFIG_MT_SHARED_MBEDTLS_THREADING)
    message(FATAL_ERROR "Optional private runtime requires the shared Mbed TLS/TF-PSA threading capability")
  endif()
  if(CONFIG_ENTROPY_PSA_CRYPTO_RNG)
    message(FATAL_ERROR "Unsupported PSA RNG configuration for the optional private runtime")
  endif()
  if(NOT CONFIG_MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG OR CONFIG_MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG_ALLOW_NON_CSPRNG)
    message(FATAL_ERROR "Private startup requires the reviewed external PSA RNG configuration; no internal seed/fallback path")
  endif()
  target_sources(app PRIVATE src/pki/BoundedEntropy.cpp src/pki/ZephyrEntropy.cpp
    src/pki/PkiStorage.cpp src/pki/PkiAppendHistory.cpp src/pki/ZephyrPkiStorage.cpp src/pki/ZephyrAppendStore.cpp)
  set_source_files_properties(src/pki/BoundedEntropy.cpp src/pki/ZephyrEntropy.cpp
    src/pki/PkiStorage.cpp src/pki/PkiAppendHistory.cpp src/pki/ZephyrPkiStorage.cpp src/pki/ZephyrAppendStore.cpp PROPERTIES COMPILE_OPTIONS "-fstack-usage")
  if(CONFIG_MT_PKI_STOCK_TRIAL)
    target_sources(app PRIVATE src/pki/PkiProvisioning.cpp)
    set_source_files_properties(src/pki/PkiProvisioning.cpp PROPERTIES COMPILE_OPTIONS "-fstack-usage")
  endif()
endif()
