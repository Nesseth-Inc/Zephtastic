# SPDX-License-Identifier: GPL-3.0-only
# Include from ZephCore only when CONFIG_ZEPHCORE_AUDIO_CODEC2=y. This exact
# Meshtastic fork commit contains generated codebooks, so no host generator or
# nested cross-build is required.
include(FetchContent)
FetchContent_Declare(codec2
  GIT_REPOSITORY https://github.com/meshtastic/codec2.git
  GIT_TAG 3e347e0b70fc72af9357fbbd32b10a125321d6c5
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(codec2)
if(NOT codec2_POPULATED)
  FetchContent_Populate(codec2)
endif()

# This pinned upstream library has no CMakeLists.txt. Its checked-in `src`
# contains codec2.c, the full required C implementation and generated codebook
# C sources. Compile those actual sources into the target linked by Audio.
file(GLOB MT_CODEC2_SOURCES CONFIGURE_DEPENDS "${codec2_SOURCE_DIR}/src/*.c")
if(NOT MT_CODEC2_SOURCES)
  message(FATAL_ERROR "Pinned Meshtastic Codec2 source tree is empty")
endif()
add_library(codec2 STATIC ${MT_CODEC2_SOURCES})
target_include_directories(codec2 PUBLIC "${codec2_SOURCE_DIR}/src")
target_compile_definitions(codec2 PUBLIC CODEC2_MODE_450PWB_EN=0)
target_link_libraries(codec2 PUBLIC m)
