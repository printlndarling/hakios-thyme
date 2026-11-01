/*
 * The board: where the blocks of the phone's SoC are (bsd/dev/hakios.board.c). The THYME drivers use this
 * phone's SM8250 addresses unless the command line names another with board_<block>=0x..., so a phone with
 * the same blocks at other places needs no other kernel.
 */
#ifndef _SYS_HK_BOARD_H_
#define _SYS_HK_BOARD_H_

#include <mach/vm_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the physical address of a block: board_<name>= on the command line, else `dflt` */
vm_offset_t hakios_board_addr(const char *name, vm_offset_t dflt);
/* a number of the board (counts, sizes): board_<name>=, else `dflt` */
uint32_t hakios_board_number(const char *name, uint32_t dflt);
/* every board_ address that was given, once, to the log (hakios.input.c, at the drivers' start) */
void hakios_board_report(void);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_HK_BOARD_H_ */
