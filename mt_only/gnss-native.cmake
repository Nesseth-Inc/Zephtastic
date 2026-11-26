# The GNSS byte stream and parsed publication use this exact donor adaptation.
# Read only: stage these bodies in a separate SDK before configuring a build.
if(CONFIG_MT_GNSS_T096 OR CONFIG_MT_GNSS_XIAO_L76K)
  foreach(mt_gnss_file IN ITEMS gnss_nmea_generic.c gnss_nmea0183.c gnss_nmea0183_match.c)
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/dependency-overlay/zephyr/drivers/gnss/${mt_gnss_file}" mt_gnss_expected)
    file(SHA256 "$ENV{ZEPHYR_BASE}/drivers/gnss/${mt_gnss_file}" mt_gnss_actual)
    if(NOT mt_gnss_actual STREQUAL mt_gnss_expected)
      message(FATAL_ERROR "Wrong MT GNSS dependency: ${mt_gnss_file}; use the pinned isolated SDK")
    endif()
  endforeach()
endif()
