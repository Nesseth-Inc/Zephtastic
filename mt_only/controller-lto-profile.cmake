# Preserve the pinned timing-sensitive BLE controller sources outside LTO.
if(CONFIG_MT_LTO_RUNTIME)
  function(mt_lto_keep_controller_source_profiles)
    foreach(controller_target IN ITEMS subsys__bluetooth__controller subsys__bluetooth__controller__ll_sw__nordic)
      if(NOT TARGET ${controller_target})
        message(FATAL_ERROR "Expected T096 BLE controller target is absent")
      endif()
      if(controller_target STREQUAL "subsys__bluetooth__controller")
        set(controller_relative_sources
          subsys/bluetooth/controller/crypto/crypto.c
          subsys/bluetooth/controller/flash/soc_flash_nrf_ticker.c
          subsys/bluetooth/controller/hci/hci.c
          subsys/bluetooth/controller/hci/hci_driver.c
          subsys/bluetooth/controller/ll_sw/ll_addr.c
          subsys/bluetooth/controller/ll_sw/ll_feat.c
          subsys/bluetooth/controller/ll_sw/ll_tx_pwr.c
          subsys/bluetooth/controller/ll_sw/lll_chan.c
          subsys/bluetooth/controller/ll_sw/lll_common.c
          subsys/bluetooth/controller/ll_sw/ull.c
          subsys/bluetooth/controller/ll_sw/ull_adv.c
          subsys/bluetooth/controller/ll_sw/ull_chan.c
          subsys/bluetooth/controller/ll_sw/ull_conn.c
          subsys/bluetooth/controller/ll_sw/ull_filter.c
          subsys/bluetooth/controller/ll_sw/ull_llcp.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_chmu.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_common.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_conn_upd.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_enc.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_local.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_pdu.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_phy.c
          subsys/bluetooth/controller/ll_sw/ull_llcp_remote.c
          subsys/bluetooth/controller/ll_sw/ull_peripheral.c
          subsys/bluetooth/controller/ll_sw/ull_tx_queue.c
          subsys/bluetooth/controller/ticker/ticker.c
          subsys/bluetooth/controller/util/dbuf.c
          subsys/bluetooth/controller/util/mayfly.c
          subsys/bluetooth/controller/util/mem.c
          subsys/bluetooth/controller/util/memq.c
          subsys/bluetooth/controller/util/util.c
        )
        if(CONFIG_BT_OBSERVER)
          list(APPEND controller_relative_sources subsys/bluetooth/controller/ll_sw/ull_scan.c)
        endif()
      elseif(controller_target STREQUAL "subsys__bluetooth__controller__ll_sw__nordic")
        set(controller_relative_sources
          subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/cntr.c
          subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/ecb.c
          subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/mayfly.c
          subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/radio/radio.c
          subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/ticker.c
          subsys/bluetooth/controller/ll_sw/nordic/hci/hci_vendor.c
          subsys/bluetooth/controller/ll_sw/nordic/lll/lll.c
          subsys/bluetooth/controller/ll_sw/nordic/lll/lll_adv.c
          subsys/bluetooth/controller/ll_sw/nordic/lll/lll_clock.c
          subsys/bluetooth/controller/ll_sw/nordic/lll/lll_conn.c
          subsys/bluetooth/controller/ll_sw/nordic/lll/lll_peripheral.c
        )
        if(CONFIG_BT_OBSERVER)
          list(APPEND controller_relative_sources subsys/bluetooth/controller/ll_sw/nordic/lll/lll_scan.c)
        endif()
      endif()
      get_target_property(controller_source_dir ${controller_target} SOURCE_DIR)
      get_target_property(controller_actual_sources ${controller_target} SOURCES)
      set(controller_absolute_sources)
      foreach(controller_actual IN LISTS controller_actual_sources)
        if(controller_actual MATCHES "^\\$<")
          message(FATAL_ERROR "Unsupported generator-expression T096 BLE controller source")
        endif()
        get_filename_component(controller_absolute "${controller_actual}" ABSOLUTE BASE_DIR "${controller_source_dir}")
        list(APPEND controller_absolute_sources "${controller_absolute}")
      endforeach()
      list(LENGTH controller_absolute_sources controller_actual_count)
      list(LENGTH controller_relative_sources controller_expected_count)
      if(NOT controller_actual_count EQUAL controller_expected_count)
        message(FATAL_ERROR "Unexpected T096 BLE controller source count")
      endif()
      foreach(controller_relative IN LISTS controller_relative_sources)
        get_filename_component(controller_source "${ZEPHYR_BASE}/${controller_relative}" ABSOLUTE)
        list(FIND controller_absolute_sources "${controller_source}" controller_source_index)
        if(controller_source_index LESS 0)
          message(FATAL_ERROR "Expected exact T096 BLE controller source is absent: ${controller_relative}")
        endif()
        set_property(SOURCE "${controller_source}" TARGET_DIRECTORY ${controller_target}
          APPEND PROPERTY COMPILE_OPTIONS -fno-lto -O2)
      endforeach()
    endforeach()
  endfunction()
  cmake_language(DEFER CALL mt_lto_keep_controller_source_profiles)
endif()

# Explicitly opt-in size canary. Ordinary source and link profiles remain unchanged.
option(MT_LTO_CANARY_NO_PARTIAL_INLINING
  "Disable GCC partial inlining only for app main.cpp and PkiStorage.cpp (measurement canary)" OFF)
if(MT_LTO_CANARY_NO_PARTIAL_INLINING)
  if(NOT CONFIG_MT_LTO_RUNTIME OR NOT CONFIG_LTO)
    message(FATAL_ERROR "Partial-inlining canary requires the existing MT LTO runtime profile")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "Partial-inlining canary requires GNU C++")
  endif()
  if(NOT COMMAND zephyr_ld_options)
    message(FATAL_ERROR "Partial-inlining canary requires Zephyr's final-link option helper")
  endif()
  if(NOT COMMAND zephyr_check_compiler_flag)
    message(FATAL_ERROR "Partial-inlining canary requires Zephyr compiler-flag checks")
  endif()
  zephyr_check_compiler_flag(CXX "-fno-partial-inlining" mt_lto_cxx_partial_inlining_supported)
  zephyr_check_compiler_flag(C "-fno-partial-inlining" mt_lto_c_partial_inlining_supported)
  if(NOT mt_lto_cxx_partial_inlining_supported OR NOT mt_lto_c_partial_inlining_supported)
    message(FATAL_ERROR "Partial-inlining canary flag is unsupported by the selected compiler")
  endif()

  function(mt_lto_apply_no_partial_inlining_canary profile_source_dir)
    if(NOT TARGET app)
      message(FATAL_ERROR "Partial-inlining canary requires the Zephyr app target")
    endif()
    get_target_property(app_type app TYPE)
    if(NOT app_type STREQUAL "STATIC_LIBRARY")
      message(FATAL_ERROR "Partial-inlining canary expected Zephyr's static app library")
    endif()
    get_target_property(app_source_dir app SOURCE_DIR)
    get_target_property(app_sources app SOURCES)
    if(NOT app_source_dir OR NOT app_sources)
      message(FATAL_ERROR "Partial-inlining canary could not inspect app sources")
    endif()

    foreach(relative_source IN ITEMS src/main.cpp src/pki/PkiStorage.cpp)
      get_filename_component(expected_source "${relative_source}" ABSOLUTE
        BASE_DIR "${profile_source_dir}")
      set(source_matches 0)
      foreach(app_source IN LISTS app_sources)
        if(app_source MATCHES "^\\$<")
          continue()
        endif()
        if(IS_ABSOLUTE "${app_source}")
          get_filename_component(actual_source "${app_source}" ABSOLUTE)
          if(actual_source STREQUAL expected_source)
            math(EXPR source_matches "${source_matches}+1")
          endif()
        else()
          get_filename_component(actual_from_target "${app_source}" ABSOLUTE
            BASE_DIR "${app_source_dir}")
          get_filename_component(actual_from_profile "${app_source}" ABSOLUTE
            BASE_DIR "${profile_source_dir}")
          if(actual_from_target STREQUAL expected_source OR actual_from_profile STREQUAL expected_source)
            math(EXPR source_matches "${source_matches}+1")
          endif()
        endif()
      endforeach()
      if(NOT source_matches EQUAL 1)
        message(FATAL_ERROR "Partial-inlining canary requires exactly one app source: ${relative_source}")
      endif()
      set_property(SOURCE "${expected_source}" TARGET_DIRECTORY app
        APPEND PROPERTY COMPILE_OPTIONS -fno-partial-inlining)
    endforeach()

    # GCC LTO requires the optimization option at the final compiler-driver link too.
    zephyr_ld_options(-fno-partial-inlining)
  endfunction()
  mt_lto_apply_no_partial_inlining_canary("${CMAKE_CURRENT_SOURCE_DIR}")
endif()


# Keep identical internal helper functions in one LTO partition on the tightly
# sized T096. The Nordic controller's per-source O2/no-LTO exclusions still apply.
if(CONFIG_MT_PKI_FRESH_DOMAIN_ONLY AND CONFIG_MT_LTO_RUNTIME)
  # The extended companion entry point contains many related switch tails.
  # Keep their source order for GCC's size-oriented tail merging.
  if(CONFIG_MT_T096_FULL_COMPANION)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" TARGET_DIRECTORY app
      APPEND PROPERTY COMPILE_OPTIONS -fno-reorder-blocks --param=min-crossjump-insns=1)
  endif()
  # Scalarize aggregate arguments across the application link and share short
  # identical branch tails. The fixed Nordic controller objects do not use LTO.
  zephyr_ld_options(-flto-partition=one -fipa-sra --param=min-crossjump-insns=1)
endif()
