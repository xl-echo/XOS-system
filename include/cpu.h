#ifndef _XOS_CPU_H
#define _XOS_CPU_H
#include "types.h"

#define GDT_R0_CODE  0x08u
#define GDT_R0_DATA  0x10u
#define GDT_R3_CODE  0x18u
#define GDT_R3_DATA  0x20u
#define GDT_TSS      0x28u

void cpu_gdt_init(u32 kernel_esp0);
void cpu_set_esp0(u32 esp0);
u32  cpu_tss_get_esp0(void);
int  cpu_selftest(void);
int  cpu_user_supported(void);

#endif
