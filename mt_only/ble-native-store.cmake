# Native bond storage depends on these exact source-owned SDK bodies, including
# the retained NVS error propagation. This check never modifies a dependency.
foreach(mt_settings_file IN ITEMS
    subsys/settings/src/settings.c
    subsys/settings/src/settings_store.c
    subsys/settings/src/settings_nvs.c
    subsys/bluetooth/host/settings.c
    subsys/bluetooth/host/keys.c)
  file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/dependency-overlay/zephyr/${mt_settings_file}" mt_settings_expected)
  file(SHA256 "$ENV{ZEPHYR_BASE}/${mt_settings_file}" mt_settings_actual)
  if(NOT mt_settings_actual STREQUAL mt_settings_expected)
    message(FATAL_ERROR "Wrong MT native Settings dependency: ${mt_settings_file}; use the pinned overlay in an isolated SDK")
  endif()
endforeach()
