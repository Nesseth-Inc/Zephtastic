# Only xiao_release.py adds this module to the XIAO MCUboot build.
zephyr_library()
zephyr_library_sources(erase.c)
zephyr_link_libraries(-Wl,--wrap=boot_erase_region)
