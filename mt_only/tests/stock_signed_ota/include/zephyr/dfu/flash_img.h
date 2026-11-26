#pragma once
#include <zephyr/storage/flash_map.h>
struct flash_img_context { const struct flash_area *flash_area; size_t offset; };
int flash_img_init_id(flash_img_context*,int);
int flash_img_buffered_write(flash_img_context*,const uint8_t*,size_t,bool);
