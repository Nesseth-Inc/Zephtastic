#pragma once
#include <stdint.h>
#include <stddef.h>
struct flash_area { int id; };
#define PARTITION_ID(name) name
#define slot0_partition 0
#define slot1_partition 1
#define scratch_partition 2
#define PARTITION_OFFSET(i) ((i)==0?0x1d0000:(i)==1?0x10000:0x3e0000)
#define PARTITION_SIZE(i) ((i)==2?0x10000:0x1c0000)
#define DT_REG_ADDR(x) 0x7f0000
int flash_area_open(int,const flash_area**);
void flash_area_close(const flash_area*);
int flash_area_read(const flash_area*,size_t,void*,size_t);
