#pragma once
constexpr int BOOT_SWAP_TYPE_NONE=1,BOOT_SWAP_TYPE_TEST=2,BOOT_UPGRADE_TEST=0;
bool boot_is_img_confirmed();
int mcuboot_swap_type();
int boot_request_upgrade(int);
int boot_write_img_confirmed();
