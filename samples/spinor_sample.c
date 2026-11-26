/**
 * @file spinor_sample.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include <stdint.h>
#include <stdbool.h>

/*Include the declarations of the board_flash_* functions of the application here*/

/*********************
 *      DEFINES
 *********************/

/**Commands of the SPI NOR flash, check them against its datasheet
 * They use 24-bit addresses, a flash above 16MB needs the 4-byte address commands*/
#define STRESS_NOR_READ_SR 0x05U
#define STRESS_NOR_WRITE_EN 0x06U
#define STRESS_NOR_PAGE_PROG 0x02U
#define STRESS_NOR_FAST_READ 0x0BU
#define STRESS_NOR_SEC_ERASE 0x20U

/*Command and address bytes, the fast read adds one dummy byte*/
#define STRESS_NOR_CMD_LEN 4U
#define STRESS_NOR_READ_LEN 5U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void stress_nor_cmd(uint8_t * cmd, uint8_t code, uint32_t addr);
static int32_t stress_nor_wren(void);

/**SPI bus of the application, replace them by the real ones, negative on failure
 * board_spi_select(true) drives CS low, the bus runs in SPI mode 0 or 3 at a clock
 * the flash supports for the fast read, write and read share the selected transfer*/
extern void board_spi_select(bool active);
extern int32_t board_spi_write(const uint8_t * buf, uint32_t len);
extern int32_t board_spi_read(uint8_t * buf, uint32_t len);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Reads data with the fast read command, any length and any buffer alignment.
 * @param addr Flash address to read from.
 * @param buf Pointer to the buffer that receives the data.
 * @param len Number of bytes to read.
 * @return 0 or more on success; negative on failure.
 */
int32_t board_flash_read(uint32_t addr, uint8_t * buf, uint32_t len)
{
    uint8_t cmd[STRESS_NOR_READ_LEN] = {0};
    int32_t res = 0;

    /*The fifth byte stays 0, it is the dummy byte of the fast read*/
    stress_nor_cmd(cmd, STRESS_NOR_FAST_READ, addr);

    board_spi_select(true);
    res = board_spi_write(cmd, STRESS_NOR_READ_LEN);
    if (res >= 0) res = board_spi_read(buf, len);
    board_spi_select(false);

    return res;
}

/**
 * Sends a page program command and returns without waiting for its end.
 * A flash with block protection set ignores it silently, the test then reports DATA errors.
 * @param addr Flash address to program, the data must not cross a page boundary.
 * @param buf Pointer to the data to program.
 * @param len Number of bytes to program, up to one page.
 * @return 0 or more if the command was sent; negative on failure.
 */
int32_t board_flash_page_program(uint32_t addr, const uint8_t * buf, uint32_t len)
{
    uint8_t cmd[STRESS_NOR_CMD_LEN] = {0};
    int32_t res = 0;

    if (stress_nor_wren() < 0) return -1;
    stress_nor_cmd(cmd, STRESS_NOR_PAGE_PROG, addr);

    /*The data is sent before returning, the caller may reuse its buffer*/
    board_spi_select(true);
    res = board_spi_write(cmd, STRESS_NOR_CMD_LEN);
    if (res >= 0) res = board_spi_write(buf, len);
    board_spi_select(false);

    return res;
}

/**
 * Sends a 4KB sector erase command and returns without waiting for its end.
 * A flash with block protection set ignores it silently, the test then reports ERASE errors.
 * @param addr Flash address of the sector to erase.
 * @return 0 or more if the command was sent; negative on failure.
 */
int32_t board_flash_sector_erase(uint32_t addr)
{
    uint8_t cmd[STRESS_NOR_CMD_LEN] = {0};
    int32_t res = 0;

    if (stress_nor_wren() < 0) return -1;
    stress_nor_cmd(cmd, STRESS_NOR_SEC_ERASE, addr);

    board_spi_select(true);
    res = board_spi_write(cmd, STRESS_NOR_CMD_LEN);
    board_spi_select(false);

    return res;
}

/**
 * Reads the status register, bit 0 is BUSY while an erase or a program runs.
 * @return Status register, 0 when the transfer failed.
 */
uint8_t board_flash_read_status(void)
{
    uint8_t cmd = STRESS_NOR_READ_SR;
    uint8_t status = 0U;
    int32_t res = 0;

    board_spi_select(true);
    res = board_spi_write(&cmd, 1U);
    if (res >= 0) res = board_spi_read(&status, 1U);
    board_spi_select(false);

    if (res < 0) status = 0U;
    return status;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Builds a command made of the command code and a 24-bit address.
 * @param cmd Pointer to the command buffer, at least STRESS_NOR_CMD_LEN bytes.
 * @param code Command code.
 * @param addr Flash address the command applies to.
 */
static void stress_nor_cmd(uint8_t * cmd, uint8_t code, uint32_t addr)
{
    cmd[0] = code;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr >> 0);
}

/**
 * Sends the write enable command, needed before every program and erase.
 * @return 0 or more if the command was sent; negative on failure.
 */
static int32_t stress_nor_wren(void)
{
    uint8_t cmd = STRESS_NOR_WRITE_EN;
    int32_t res = 0;

    board_spi_select(true);
    res = board_spi_write(&cmd, 1U);
    board_spi_select(false);

    return res;
}
