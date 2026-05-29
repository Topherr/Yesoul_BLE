/* Linker script — nRF52840 + S340 SoftDevice v6.1.1 on PCA10059
 *
 * PCA10059 flash map:
 *   0x000000-0x000FFF  MBR (1 KB)              \_ written together as the
 *   0x001000-0x02F73F  S340 SoftDevice         /  "SoftDevice + MBR" bundle
 *                      (S340 v6.1.1 .hex extends to 0x2F73F — verified with
 *                       `srec_info` on the SoftDevice .hex itself)
 *   0x030000-0x030FFF  (padding to next 4 KB sector)
 *   0x031000-0x0DFFFF  Application slot (~700 KB available — way more than enough)
 *   0x0E0000-0x0FDFFF  Nordic Open USB Bootloader (120 KB) ← DO NOT TOUCH
 *   0x0FE000-0x0FEFFF  MBR parameters
 *   0x0FF000-0x0FFFFF  Bootloader settings page
 *
 * Net app region: 0x31000 → 0xDFFFF, length 0xAF000 (700 KB).
 *
 * First version of this script had FLASH ORIGIN=0x27000, which OVERLAPPED
 * ~32 KB of the SoftDevice (0x27000-0x2F73F). The DFU package wrote both
 * and they collided in flash → chip HardFaulted before reaching main()
 * (observed: zero LED activity, dongle silently dark, recoverable only by
 * RESET into bootloader). 0x31000 matches turbodonkey's working linker
 * (theirs targeted the same S340 binary on SparkFun Pro nRF52840 Mini).
 *
 * RAM ORIGIN starts just above the SoftDevice's RAM reservation. S340 v6.1.1
 * with default service configs uses approximately 0x2200 of RAM; we start
 * the app at 0x20002800 (a hair above that). If the linker complains about
 * insufficient RAM the SD_RAM_RESERVATION will need adjusting upward.
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  /* App slot only — leaves 0xE0000-0xFFFFF clear for Nordic Open Bootloader. */
  FLASH (rx) : ORIGIN = 0x31000, LENGTH = 0xAF000
  RAM   (rwx): ORIGIN = 0x20002800, LENGTH = 0x3D800
}

SECTIONS
{
}

SECTIONS
{
  . = ALIGN(4);
  .mem_section_dummy_ram :
  {
  }
  .log_dynamic_data :
  {
    PROVIDE(__start_log_dynamic_data = .);
    KEEP(*(SORT(.log_dynamic_data*)))
    PROVIDE(__stop_log_dynamic_data = .);
  } > RAM
  .log_filter_data :
  {
    PROVIDE(__start_log_filter_data = .);
    KEEP(*(SORT(.log_filter_data*)))
    PROVIDE(__stop_log_filter_data = .);
  } > RAM
  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM
} INSERT AFTER .data;

SECTIONS
{
  .mem_section_dummy_rom :
  {
  }
  .sdh_soc_observers :
  {
    PROVIDE(__start_sdh_soc_observers = .);
    KEEP(*(SORT(.sdh_soc_observers*)))
    PROVIDE(__stop_sdh_soc_observers = .);
  } > FLASH
  .sdh_ble_observers :
  {
    PROVIDE(__start_sdh_ble_observers = .);
    KEEP(*(SORT(.sdh_ble_observers*)))
    PROVIDE(__stop_sdh_ble_observers = .);
  } > FLASH
  .sdh_ant_observers :
  {
    PROVIDE(__start_sdh_ant_observers = .);
    KEEP(*(SORT(.sdh_ant_observers*)))
    PROVIDE(__stop_sdh_ant_observers = .);
  } > FLASH
  .pwr_mgmt_data :
  {
    PROVIDE(__start_pwr_mgmt_data = .);
    KEEP(*(SORT(.pwr_mgmt_data*)))
    PROVIDE(__stop_pwr_mgmt_data = .);
  } > FLASH
  .sdh_req_observers :
  {
    PROVIDE(__start_sdh_req_observers = .);
    KEEP(*(SORT(.sdh_req_observers*)))
    PROVIDE(__stop_sdh_req_observers = .);
  } > FLASH
  .sdh_state_observers :
  {
    PROVIDE(__start_sdh_state_observers = .);
    KEEP(*(SORT(.sdh_state_observers*)))
    PROVIDE(__stop_sdh_state_observers = .);
  } > FLASH
  .sdh_stack_observers :
  {
    PROVIDE(__start_sdh_stack_observers = .);
    KEEP(*(SORT(.sdh_stack_observers*)))
    PROVIDE(__stop_sdh_stack_observers = .);
  } > FLASH
  .log_const_data :
  {
    PROVIDE(__start_log_const_data = .);
    KEEP(*(SORT(.log_const_data*)))
    PROVIDE(__stop_log_const_data = .);
  } > FLASH
  .log_backends :
  {
    PROVIDE(__start_log_backends = .);
    KEEP(*(SORT(.log_backends*)))
    PROVIDE(__stop_log_backends = .);
  } > FLASH
  .nrf_balloc :
  {
    PROVIDE(__start_nrf_balloc = .);
    KEEP(*(.nrf_balloc))
    PROVIDE(__stop_nrf_balloc = .);
  } > FLASH
} INSERT AFTER .text

INCLUDE "nrf_common.ld"
