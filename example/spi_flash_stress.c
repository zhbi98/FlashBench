/**
 * @file spi_flash_stress.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include <stddef.h>
#include <string.h>
#include "flash_stress.h"
#include "spi_flash_stress.h"
#include "spi_flash.h"
#include "sys_spi.h"
#include "sys_types.h"
#include "sys_uart.h"

/*********************
 *      DEFINES
 *********************/

/*Commands and status bit of the SPI NOR flash*/
#define STRESS_CMD_SEC_ERASE 0x20U
#define STRESS_CMD_LEN 4U
#define STRESS_READ_CMD_LEN 5U
#define STRESS_SR_BUSY 0x01U

/*One tagged line on the debug UART*/
#define STRESS_LOG(fmt, ...) sysprintf("[FLASH STRESS] " fmt "\r\n", ##__VA_ARGS__)

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool stress_read(uint32_t addr, uint8_t * buf, uint32_t len);
static bool stress_prog(uint32_t addr, const uint8_t * buf, uint32_t len);
static bool stress_erase(uint32_t addr);
static bool stress_busy(void);
static void stress_on_error(const flash_stress_err_t * err);
static void stress_on_loop(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt);
static void stress_set_cmd(uint8_t * cmd, uint8_t code, uint32_t addr);
static void stress_report(const flash_stress_t * pstress);
static const char * stress_phase_name(flash_stress_state_t state);
static const char * stress_err_name(flash_stress_res_t res);

/*Helpers of spi_flash.c that are not declared in its header*/
extern u8 SPI_Flash_ReadSR(int32_t SPI);
extern int32_t SPI_FLASH_Write_Enable(int32_t SPI);

/**********************
 *  STATIC VARIABLES
 **********************/

/**The only place where the flash driver of this project is bound to the
 * framework, replace these entries to test another flash or driver*/
static const flash_stress_ops_t stress_ops = {
    .read = stress_read,
    .prog = stress_prog,
    .erase = stress_erase,
    .busy = stress_busy,
    .on_error = stress_on_error,
    .on_loop = stress_on_loop,
};

static flash_stress_t stress_inst = {0};
static uint8_t stress_wbuf[SPI_FLASH_STRESS_SEC_SIZE] = {0};
static uint8_t stress_rbuf[SPI_FLASH_STRESS_SEC_SIZE] = {0};

/*Set from the start of a test until its result has been reported*/
static bool stress_active = false;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Starts the stress test on the SPI flash, the work is done in spi_flash_stress_poll().
 * @param req Pointer to the test request.
 * @return 0 if the test was started; -1 if the request is invalid or a test is running.
 */
int32_t spi_flash_stress_start(const spi_flash_stress_req_t * req)
{
    flash_stress_t * pstress = &stress_inst;
    flash_stress_cfg_t cfg = {0};

    if (!req) return -1;
    if (stress_active) return -1;

    /*Describe the test area*/
    cfg.base = req->base;
    cfg.sec_size = SPI_FLASH_STRESS_SEC_SIZE;
    cfg.sec_cnt = req->sec_cnt;
    cfg.wr_secs = req->wr_secs;
    cfg.prog_size = SPI_FLASH_STRESS_PAGE_SIZE;
    cfg.reads = req->reads;
    cfg.wbuf = stress_wbuf;
    cfg.rbuf = stress_rbuf;

    /*Register the driver, the flash is not accessed yet*/
    if (flash_stress_init(pstress, &stress_ops, &cfg) != FLASH_STRESS_OK) {
        STRESS_LOG("ABORT: invalid request");
        return -1;
    }
    if (flash_stress_start(pstress, req->loops) != FLASH_STRESS_OK) return -1;

    STRESS_LOG("start: addr=0x%08x sector=%d x %d write=%d sectors loops=%d",
        cfg.base, cfg.sec_size, cfg.sec_cnt, pstress->cfg.wr_secs, req->loops);

    stress_active = true;
    return 0;
}

/**
 * Advances the test by one small step, to be called repeatedly from a main loop or a task.
 * @return 1 while the test is running; 0 once it is finished or when no test was started.
 */
int32_t spi_flash_stress_poll(void)
{
    flash_stress_t * pstress = &stress_inst;

    if (!stress_active) return 0;
    if (flash_stress_poll(pstress)) return 1;

    /*The test has just ended, report it once*/
    stress_active = false;
    stress_report(pstress);

    return 0;
}

/**
 * Tells whether a test is running, normal flash accesses are skipped during that time.
 * @return 1 from the start of a test until it has finished; otherwise 0.
 */
int32_t spi_flash_stress_running(void)
{
    return stress_active ? 1 : 0;
}

/**
 * Requests the running test to end after the current loop.
 */
void spi_flash_stress_stop(void)
{
    flash_stress_stop(&stress_inst);
}

/**
 * Copies the live readings of the test, for example to refresh a display.
 * @param info Pointer to the structure that receives the readings.
 */
void spi_flash_stress_get(spi_flash_stress_info_t * info)
{
    const flash_stress_t * pstress = &stress_inst;
    const flash_stress_stat_t * stat = flash_stress_stat(pstress);
    const flash_stress_err_t * err = flash_stress_last_err(pstress);

    if (!info) return;

    memset(info, 0, sizeof(*info));
    info->running = stress_active ? 1U : 0U;
    info->loops = pstress->loops;
    info->loop = flash_stress_loop(pstress);
    info->ok_cnt = flash_stress_ok_cnt(pstress);
    info->err_cnt = flash_stress_err_cnt(pstress);
    info->rd_bytes = stat->rd_bytes;
    info->wr_bytes = stat->wr_bytes;
    info->er_bytes = stat->er_bytes;
    info->phase = stress_phase_name(flash_stress_state(pstress));
    info->err = stress_err_name(err->res);
    info->err_addr = err->addr;
    info->err_expect = err->expect;
    info->err_got = err->got;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Reads data from the SPI flash with the fast read command.
 * @param addr Flash address to read from.
 * @param buf Pointer to the buffer that receives the data.
 * @param len Number of bytes to read.
 * @return true if the transfer succeeded; otherwise false.
 */
static bool stress_read(uint32_t addr, uint8_t * buf, uint32_t len)
{
    uint8_t cmd[STRESS_READ_CMD_LEN] = {0};
    int32_t res = 0;

    /**The driver read is not used here, it is skipped while a test is running
     * The fifth byte is the dummy byte required by the fast read command*/
    stress_set_cmd(cmd, W25X_FastReadData, addr);
    sys_spi_select(SPI0);
    res = sys_spi_write_then_read(SPI0, cmd, STRESS_READ_CMD_LEN, buf, (int32_t)len);
    sys_spi_deselect(SPI0);

    return res >= 0;
}

/**
 * Sends a page program command and returns without waiting for its completion.
 * @param addr Flash address to program.
 * @param buf Pointer to the data to program.
 * @param len Number of bytes to program, up to one page.
 * @return true if the command was sent; otherwise false.
 */
static bool stress_prog(uint32_t addr, const uint8_t * buf, uint32_t len)
{
    uint8_t cmd[STRESS_CMD_LEN] = {0};
    int32_t res = 0;

    if (SPI_FLASH_Write_Enable(SPI0) < 0) return false;

    stress_set_cmd(cmd, W25X_PageProgram, addr);
    sys_spi_select(SPI0);
    res = sys_spi_write_then_write(SPI0, cmd, STRESS_CMD_LEN, (void *)buf, (int32_t)len);
    sys_spi_deselect(SPI0);

    return res >= 0;
}

/**
 * Sends a 4KB sector erase command and returns without waiting for its completion.
 * @param addr Flash address of the sector to erase.
 * @return true if the command was sent; otherwise false.
 */
static bool stress_erase(uint32_t addr)
{
    uint8_t cmd[STRESS_CMD_LEN] = {0};
    int32_t res = 0;

    if (SPI_FLASH_Write_Enable(SPI0) < 0) return false;

    stress_set_cmd(cmd, STRESS_CMD_SEC_ERASE, addr);
    sys_spi_select(SPI0);
    res = sys_spi_transfer(SPI0, cmd, NULL, STRESS_CMD_LEN);
    sys_spi_deselect(SPI0);

    return res == (int32_t)STRESS_CMD_LEN;
}

/**
 * Reads the status register to tell whether an erase or a program is still in progress.
 * @return true while the flash is busy; otherwise false.
 */
static bool stress_busy(void)
{
    return (SPI_Flash_ReadSR(SPI0) & STRESS_SR_BUSY) != 0U;
}

/**
 * Prints one detected error on the debug UART.
 * @param err Pointer to the error reported by the framework.
 */
static void stress_on_error(const flash_stress_err_t * err)
{
    const char * kind = NULL;

    if (!err) return;

    if (err->res == FLASH_STRESS_ERR_ERASE_VERIFY) {
        kind = "ERASE";
    } else if (err->res == FLASH_STRESS_ERR_DATA_VERIFY) {
        kind = "DATA";
    }

    /*IO errors carry no compare details*/
    if (!kind) {
        STRESS_LOG("loop %d IO ERR res=%d addr=0x%08x", err->loop, err->res, err->addr);
        return;
    }

    STRESS_LOG("loop %d %s VERIFY ERR addr=0x%08x expect=0x%02x got=0x%02x bad_cnt=%d",
        err->loop, kind, err->addr, err->expect, err->got, err->bad_cnt);
}

/**
 * Prints the counters after every finished loop.
 * @param loop Index of the finished loop, starting from 0.
 * @param ok_cnt Number of passed loops so far.
 * @param err_cnt Number of failed loops so far.
 */
static void stress_on_loop(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt)
{
    STRESS_LOG("loop %d: ok=%d err=%d", loop, ok_cnt, err_cnt);
}

/**
 * Builds a flash command made of the command code and a 24-bit address.
 * @param cmd Pointer to the command buffer of STRESS_CMD_LEN bytes.
 * @param code Command code.
 * @param addr Flash address the command applies to.
 */
static void stress_set_cmd(uint8_t * cmd, uint8_t code, uint32_t addr)
{
    cmd[0] = code;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)(addr >> 0);
}

/**
 * Prints the result of a test that has just ended.
 * @param pstress Pointer to the test instance.
 */
static void stress_report(const flash_stress_t * pstress)
{
    const flash_stress_err_t * err = flash_stress_last_err(pstress);
    const flash_stress_stat_t * stat = flash_stress_stat(pstress);

    /*The test area could not be used, nothing was erased*/
    if (flash_stress_state(pstress) == FLASH_STRESS_ST_ABORT) {
        if (err->res == FLASH_STRESS_ERR_NOT_BLANK) {
            STRESS_LOG("ABORT: test area not blank at 0x%08x (0x%02x), nothing erased",
                err->addr, err->got);
        } else {
            STRESS_LOG("ABORT: test area check failed, res=%d addr=0x%08x", err->res, err->addr);
        }
        return;
    }

    /*Printed in KB as 32-bit values, the traffic is always a multiple of the page size*/
    STRESS_LOG("bytes: write=%dKB read=%dKB erase=%dKB", (uint32_t)(stat->wr_bytes >> 10),
        (uint32_t)(stat->rd_bytes >> 10), (uint32_t)(stat->er_bytes >> 10));
    STRESS_LOG("end: ok=%d err=%d", flash_stress_ok_cnt(pstress), flash_stress_err_cnt(pstress));
}

/**
 * Returns the display name of a test state.
 * @param state State reported by the framework.
 * @return Pointer to a constant string.
 */
static const char * stress_phase_name(flash_stress_state_t state)
{
    switch (state) {
        case FLASH_STRESS_ST_CHECK:
            return "CHECK";
        case FLASH_STRESS_ST_ERASE:
            return "ERASE";
        case FLASH_STRESS_ST_BLANK:
            return "BLANK";
        case FLASH_STRESS_ST_PROG:
            return "WRITE";
        case FLASH_STRESS_ST_VERIFY:
            return "VERIFY";
        case FLASH_STRESS_ST_CLEAN:
            return "CLEAN";
        case FLASH_STRESS_ST_DONE:
            return "DONE";
        case FLASH_STRESS_ST_ABORT:
            return "ABORT";
        default:
            return "IDLE";
    }
}

/**
 * Returns the display name of an error type.
 * @param res Error type reported by the framework.
 * @return Pointer to a constant string, or NULL when res is not an error.
 */
static const char * stress_err_name(flash_stress_res_t res)
{
    switch (res) {
        case FLASH_STRESS_ERR_NOT_BLANK:
            return "AREA USED";
        case FLASH_STRESS_ERR_ERASE:
            return "ERASE IO";
        case FLASH_STRESS_ERR_PROG:
            return "WRITE IO";
        case FLASH_STRESS_ERR_READ:
            return "READ IO";
        case FLASH_STRESS_ERR_ERASE_VERIFY:
            return "ERASE";
        case FLASH_STRESS_ERR_DATA_VERIFY:
            return "DATA";
        default:
            return NULL;
    }
}
