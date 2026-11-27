/**
 * @file port_sample.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "flash_stress.h"

/*Include the declarations of the stress_* functions of the application here*/

/*********************
 *      DEFINES
 *********************/
/*BUSY bit of the status register, bit 0 on most SPI NOR flashes*/
#define STRESS_SR_BUSY 0x01U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
/*Flash driver and tick of the application, replace them by the real ones*/
extern int32_t board_flash_read(uint32_t addr, uint8_t * buf, uint32_t len);
extern int32_t board_flash_page_program(uint32_t addr, const uint8_t * buf,
                                        uint32_t len);
extern int32_t board_flash_sector_erase(uint32_t addr);
extern uint8_t board_flash_read_status(void);
extern uint32_t board_get_tick_ms(void);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
/**
 * Reads data through the driver used by the rest of the application. READ calls
 * it with
 * any length, flash offset and buffer alignment, so the test covers the driver
 * itself.
 * @param addr  Flash address to read from.
 * @param buf   Pointer to the buffer that receives the data.
 * @param len   Number of bytes to read, from 1 byte to one sector.
 * @return      true if the driver read the data; otherwise false.
 */
bool stress_read(uint32_t addr, uint8_t * buf, uint32_t len)
{
    return board_flash_read(addr, buf, len) >= 0;
}

/**
 * Starts programming one page and may return before the program ends.
 * The data of buf has to be sent or copied before returning, the framework
 * reuses it.
 * @param addr  Flash address to program, aligned to the page size.
 * @param buf   Pointer to the data to program.
 * @param len   Number of bytes to program, the page size of the configuration.
 * @return      true if the program was started; otherwise false.
 */
bool stress_prog(uint32_t addr, const uint8_t * buf, uint32_t len)
{
    return board_flash_page_program(addr, buf, len) >= 0;
}

/**
 * Starts erasing one sector and may return before the erase ends.
 * @param addr  Flash address of the sector, aligned to the sector size.
 * @return      true if the erase was started; otherwise false.
 */
bool stress_erase(uint32_t addr)
{
    return board_flash_sector_erase(addr) >= 0;
}

/**
 * Tells whether the last erase or program is still running. With a driver that
 * waits
 * for the end of every erase and program, leave .busy NULL in the interface
 * instead.
 * @return  true while the flash is busy; otherwise false.
 */
bool stress_busy(void)
{
    uint8_t status = board_flash_read_status();

    return (status & STRESS_SR_BUSY) != 0U;
}

/**
 * Returns the millisecond tick used to time the erases and programs.
 * @return  Free running millisecond counter.
 */
uint32_t stress_tick(void)
{
    return board_get_tick_ms();
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
