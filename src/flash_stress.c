/**
 * @file flash_stress.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include <string.h>
#include "flash_stress.h"

/*********************
 *      DEFINES
 *********************/

#define FLASH_STRESS_MAGIC_LEN 8U
#define FLASH_STRESS_BLANK 0xFFU
#define FLASH_STRESS_LCG_MUL 1664525U
#define FLASH_STRESS_LCG_INC 1013904223U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool flash_stress_cfg_valid(const flash_stress_ops_t * ops, const flash_stress_cfg_t * cfg);
static uint32_t flash_stress_addr(const flash_stress_t * pstress, uint32_t index);
static void flash_stress_fill(uint8_t * buf, uint32_t len, uint32_t seed);
static void flash_stress_fail(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr);
static void flash_stress_abort(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr);
static bool flash_stress_compare(flash_stress_t * pstress, uint32_t addr, const uint8_t * ref, flash_stress_res_t verify_res);
static void flash_stress_next_loop(flash_stress_t * pstress);
static void flash_stress_end_loop(flash_stress_t * pstress, bool passed);
static void flash_stress_do_check(flash_stress_t * pstress);
static void flash_stress_do_erase(flash_stress_t * pstress);
static void flash_stress_do_blank(flash_stress_t * pstress);
static void flash_stress_do_prog(flash_stress_t * pstress);
static void flash_stress_do_verify(flash_stress_t * pstress);
static void flash_stress_do_clean(flash_stress_t * pstress);
static bool flash_stress_io_read(flash_stress_t * pstress, uint32_t addr, uint8_t * buf);
static bool flash_stress_io_prog(flash_stress_t * pstress, uint32_t addr, const uint8_t * buf, uint32_t len);
static bool flash_stress_io_erase(flash_stress_t * pstress, uint32_t addr);

/**********************
 *  STATIC VARIABLES
 **********************/

/*Leading mark of the test data, identifies a sector left by a previous test*/
static const uint8_t flash_stress_magic[FLASH_STRESS_MAGIC_LEN] = {
    'F', 'R', 'W', 'T', 'E', 'S', 'T', 0xA5
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Initializes a test instance and registers the flash interface, without touching the flash.
 * @param pstress Pointer to the test instance to initialize.
 * @param ops Pointer to the flash interface to register, copied into the instance.
 * @param cfg Pointer to the test configuration, copied into the instance.
 * @return FLASH_STRESS_OK if the instance can be started; otherwise FLASH_STRESS_ERR_PARAM.
 */
flash_stress_res_t flash_stress_init(flash_stress_t * pstress, const flash_stress_ops_t * ops,
    const flash_stress_cfg_t * cfg)
{
    if (!pstress) return FLASH_STRESS_ERR_PARAM;
    if (!flash_stress_cfg_valid(ops, cfg)) return FLASH_STRESS_ERR_PARAM;

    memset(pstress, 0, sizeof(*pstress));
    pstress->ops = *ops;
    pstress->cfg = *cfg;

    /*Replace the values left at 0 by their defaults*/
    if (pstress->cfg.wr_secs == 0U) pstress->cfg.wr_secs = 1U;
    if (pstress->cfg.reads == 0U) pstress->cfg.reads = 1U;
    if (pstress->cfg.prog_size == 0U) pstress->cfg.prog_size = pstress->cfg.sec_size;

    pstress->wr_cnt = pstress->cfg.sec_cnt / pstress->cfg.wr_secs;
    pstress->state = FLASH_STRESS_ST_IDLE;
    pstress->ready = true;

    return FLASH_STRESS_OK;
}

/**
 * Starts a test, the work itself is done step by step in flash_stress_poll().
 * @param pstress Pointer to the test instance.
 * @param loops Number of loops to run, or 0 to run until flash_stress_stop() is called.
 * @return FLASH_STRESS_OK if the test was started; otherwise FLASH_STRESS_ERR_PARAM.
 */
flash_stress_res_t flash_stress_start(flash_stress_t * pstress, uint32_t loops)
{
    if (!pstress) return FLASH_STRESS_ERR_PARAM;
    if (!pstress->ready) return FLASH_STRESS_ERR_PARAM;
    if (flash_stress_running(pstress)) return FLASH_STRESS_ERR_PARAM;

    /*Every start is a fresh test*/
    pstress->loops = loops;
    pstress->loop = 0U;
    pstress->ok_cnt = 0U;
    pstress->err_cnt = 0U;
    pstress->wait = false;
    pstress->stop = false;
    memset(&pstress->last_err, 0, sizeof(pstress->last_err));
    memset(&pstress->stat, 0, sizeof(pstress->stat));

    /*The test area is checked first, sector by sector*/
    pstress->sec = 0U;
    pstress->state = FLASH_STRESS_ST_CHECK;

    return FLASH_STRESS_OK;
}

/**
 * Advances the test by one small step and returns without waiting for the flash.
 * @param pstress Pointer to the test instance.
 * @return true while the test is running; false once it is finished, aborted or not started.
 */
bool flash_stress_poll(flash_stress_t * pstress)
{
    if (!flash_stress_running(pstress)) return false;

    /*An erase or a program started by a previous call may still be in progress*/
    if (pstress->wait) {
        if (pstress->ops.busy && pstress->ops.busy()) return true;
        pstress->wait = false;
    }

    switch (pstress->state) {
        case FLASH_STRESS_ST_CHECK:
            flash_stress_do_check(pstress);
            break;
        case FLASH_STRESS_ST_ERASE:
            flash_stress_do_erase(pstress);
            break;
        case FLASH_STRESS_ST_BLANK:
            flash_stress_do_blank(pstress);
            break;
        case FLASH_STRESS_ST_PROG:
            flash_stress_do_prog(pstress);
            break;
        case FLASH_STRESS_ST_VERIFY:
            flash_stress_do_verify(pstress);
            break;
        case FLASH_STRESS_ST_CLEAN:
            flash_stress_do_clean(pstress);
            break;
        default:
            break;
    }

    return flash_stress_running(pstress);
}

/**
 * Requests the test to end after the current loop and to clean the test area.
 * @param pstress Pointer to the test instance.
 */
void flash_stress_stop(flash_stress_t * pstress)
{
    if (!flash_stress_running(pstress)) return;
    pstress->stop = true;
}

/**
 * Returns whether the test still needs flash_stress_poll() to be called.
 * @param pstress Pointer to the test instance.
 * @return true while the test is running; otherwise false.
 */
bool flash_stress_running(const flash_stress_t * pstress)
{
    if (!pstress) return false;
    if (!pstress->ready) return false;
    if (pstress->state == FLASH_STRESS_ST_IDLE) return false;
    if (pstress->state == FLASH_STRESS_ST_DONE) return false;
    if (pstress->state == FLASH_STRESS_ST_ABORT) return false;
    return true;
}

/**
 * Returns the current state of the test.
 * @param pstress Pointer to the test instance.
 * @return Current state, or FLASH_STRESS_ST_IDLE when the instance is invalid.
 */
flash_stress_state_t flash_stress_state(const flash_stress_t * pstress)
{
    if (!pstress) return FLASH_STRESS_ST_IDLE;
    return pstress->state;
}

/**
 * Returns the number of finished loops.
 * @param pstress Pointer to the test instance.
 * @return Number of finished loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_loop(const flash_stress_t * pstress)
{
    if (!pstress) return 0U;
    return pstress->loop;
}

/**
 * Returns the number of passed loops.
 * @param pstress Pointer to the test instance.
 * @return Number of passed loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_ok_cnt(const flash_stress_t * pstress)
{
    if (!pstress) return 0U;
    return pstress->ok_cnt;
}

/**
 * Returns the number of failed loops.
 * @param pstress Pointer to the test instance.
 * @return Number of failed loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_err_cnt(const flash_stress_t * pstress)
{
    if (!pstress) return 0U;
    return pstress->err_cnt;
}

/**
 * Returns the most recent error of the test.
 * @param pstress Pointer to the test instance.
 * @return Pointer to the stored error, or NULL when the instance is invalid.
 */
const flash_stress_err_t * flash_stress_last_err(const flash_stress_t * pstress)
{
    if (!pstress) return NULL;
    return &pstress->last_err;
}

/**
 * Returns the byte traffic of the test.
 * @param pstress Pointer to the test instance.
 * @return Pointer to the stored statistics, or NULL when the instance is invalid.
 */
const flash_stress_stat_t * flash_stress_stat(const flash_stress_t * pstress)
{
    if (!pstress) return NULL;
    return &pstress->stat;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Checks whether the registered interface and the configuration can be used.
 * @param ops Pointer to the flash interface to check.
 * @param cfg Pointer to the test configuration to check.
 * @return true if both can be used; otherwise false.
 */
static bool flash_stress_cfg_valid(const flash_stress_ops_t * ops, const flash_stress_cfg_t * cfg)
{
    if ((!ops) || (!cfg)) return false;
    if ((!ops->read) || (!ops->prog) || (!ops->erase)) return false;
    if ((!cfg->wbuf) || (!cfg->rbuf)) return false;
    if (cfg->sec_cnt == 0U) return false;
    if (cfg->sec_size <= FLASH_STRESS_MAGIC_LEN) return false;
    if ((cfg->base % cfg->sec_size) != 0U) return false;

    /*One write has to fit into the test area*/
    if (cfg->wr_secs > cfg->sec_cnt) return false;

    /*A sector has to be programmed by a whole number of program calls*/
    if (cfg->prog_size > cfg->sec_size) return false;
    if ((cfg->prog_size != 0U) && ((cfg->sec_size % cfg->prog_size) != 0U)) return false;

    return true;
}

/**
 * Returns the flash address of a sector in the test area.
 * @param pstress Pointer to the test instance.
 * @param index Index of the sector inside the test area.
 * @return Flash address of the first byte of the sector.
 */
static uint32_t flash_stress_addr(const flash_stress_t * pstress, uint32_t index)
{
    return pstress->cfg.base + index * pstress->cfg.sec_size;
}

/**
 * Fills a buffer with the test mark followed by pseudo random bytes.
 * @param buf Pointer to the buffer to fill.
 * @param len Number of bytes to fill.
 * @param seed Seed of the pseudo random bytes.
 */
static void flash_stress_fill(uint8_t * buf, uint32_t len, uint32_t seed)
{
    uint32_t x = seed * FLASH_STRESS_LCG_MUL + FLASH_STRESS_LCG_INC;

    memcpy(buf, flash_stress_magic, FLASH_STRESS_MAGIC_LEN);
    for (uint32_t i = FLASH_STRESS_MAGIC_LEN; i < len; i++) {
        x = x * FLASH_STRESS_LCG_MUL + FLASH_STRESS_LCG_INC;
        buf[i] = (uint8_t)(x >> 24);
    }
}

/**
 * Stores an error of the current loop in last_err, notifies the user and ends the loop.
 * @param pstress Pointer to the test instance.
 * @param res Error type to store.
 * @param addr Flash address where the error was detected.
 */
static void flash_stress_fail(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr)
{
    /*The verify details in last_err are filled by the caller beforehand*/
    pstress->last_err.res = res;
    pstress->last_err.loop = pstress->loop;
    pstress->last_err.addr = addr;

    if (pstress->ops.on_error)
        pstress->ops.on_error(&pstress->last_err);

    flash_stress_end_loop(pstress, false);
}

/**
 * Stores the reason in last_err and aborts the test before anything is erased.
 * @param pstress Pointer to the test instance.
 * @param res Error type to store.
 * @param addr Flash address where the problem was found.
 */
static void flash_stress_abort(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr)
{
    pstress->last_err.res = res;
    pstress->last_err.loop = 0U;
    pstress->last_err.addr = addr;
    pstress->state = FLASH_STRESS_ST_ABORT;
}

/**
 * Reads a sector back and compares it with the reference data, or with blank if ref is NULL.
 * @param pstress Pointer to the test instance.
 * @param addr Flash address of the sector to read.
 * @param ref Pointer to the expected data, or NULL to expect a blank sector.
 * @param verify_res Error type to report when the data differs.
 * @return true if the data matches; false after the error has been reported.
 */
static bool flash_stress_compare(flash_stress_t * pstress, uint32_t addr, const uint8_t * ref,
    flash_stress_res_t verify_res)
{
    uint8_t * rbuf = pstress->cfg.rbuf;
    uint32_t len = pstress->cfg.sec_size;
    uint32_t first = 0U;
    uint32_t bad = 0U;
    uint8_t first_expect = 0U;
    uint8_t first_got = 0U;

    /**Pre-fill the buffer so a read that returns no data cannot pass
     * with the content left by the previous read*/
    memset(rbuf, 0x5A, len);
    if (!flash_stress_io_read(pstress, addr, rbuf)) {
        memset(&pstress->last_err, 0, sizeof(pstress->last_err));
        flash_stress_fail(pstress, FLASH_STRESS_ERR_READ, addr);
        return false;
    }

    for (uint32_t i = 0; i < len; i++) {
        uint8_t expect = ref ? ref[i] : FLASH_STRESS_BLANK;

        if (rbuf[i] == expect) continue;
        if (bad == 0U) {
            first = i;
            first_expect = expect;
            first_got = rbuf[i];
        }
        bad++;
    }

    if (bad == 0U) return true;

    /*The most recent error is kept until the next one, a passed compare does not clear it*/
    memset(&pstress->last_err, 0, sizeof(pstress->last_err));
    pstress->last_err.expect = first_expect;
    pstress->last_err.got = first_got;
    pstress->last_err.bad_cnt = bad;
    flash_stress_fail(pstress, verify_res, addr + first);
    return false;
}

/**
 * Starts the next loop, or the cleaning when the last loop is finished or a stop is requested.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_next_loop(flash_stress_t * pstress)
{
    bool finished = (pstress->loops != 0U) && (pstress->loop >= pstress->loops);

    pstress->sec = 0U;
    pstress->off = 0U;
    pstress->pass = 0U;

    if (finished || pstress->stop) {
        pstress->state = FLASH_STRESS_ST_CLEAN;
        return;
    }

    /*The write positions are used in turn to spread the wear*/
    uint32_t index = (pstress->loop % pstress->wr_cnt) * pstress->cfg.wr_secs;

    pstress->pos = flash_stress_addr(pstress, index);
    pstress->state = FLASH_STRESS_ST_ERASE;
}

/**
 * Counts the finished loop, notifies the user and moves on to the next loop.
 * @param pstress Pointer to the test instance.
 * @param passed true if the loop passed; false if an error was detected.
 */
static void flash_stress_end_loop(flash_stress_t * pstress, bool passed)
{
    if (passed) pstress->ok_cnt++;
    else pstress->err_cnt++;

    if (pstress->ops.on_loop)
        pstress->ops.on_loop(pstress->loop, pstress->ok_cnt, pstress->err_cnt);

    pstress->loop++;
    flash_stress_next_loop(pstress);
}

/**
 * Checks one sector of the test area, it has to be blank or left by a previous test.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_check(flash_stress_t * pstress)
{
    const uint8_t * rbuf = pstress->cfg.rbuf;
    uint32_t addr = flash_stress_addr(pstress, pstress->sec);
    bool marked = false;

    if (!flash_stress_io_read(pstress, addr, pstress->cfg.rbuf)) {
        flash_stress_abort(pstress, FLASH_STRESS_ERR_READ, addr);
        return;
    }

    /*Refuse to touch an area that holds anything but blank or test data*/
    marked = memcmp(rbuf, flash_stress_magic, FLASH_STRESS_MAGIC_LEN) == 0;
    for (uint32_t i = 0; (!marked) && (i < pstress->cfg.sec_size); i++) {
        if (rbuf[i] == FLASH_STRESS_BLANK) continue;

        pstress->last_err.expect = FLASH_STRESS_BLANK;
        pstress->last_err.got = rbuf[i];
        flash_stress_abort(pstress, FLASH_STRESS_ERR_NOT_BLANK, addr + i);
        return;
    }

    pstress->sec++;
    if (pstress->sec >= pstress->cfg.sec_cnt)
        flash_stress_next_loop(pstress);
}

/**
 * Starts the erase of one sector of the current loop.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_erase(flash_stress_t * pstress)
{
    uint32_t addr = pstress->pos + pstress->sec * pstress->cfg.sec_size;

    if (!flash_stress_io_erase(pstress, addr)) {
        memset(&pstress->last_err, 0, sizeof(pstress->last_err));
        flash_stress_fail(pstress, FLASH_STRESS_ERR_ERASE, addr);
        return;
    }

    pstress->wait = true;
    pstress->sec++;
    if (pstress->sec < pstress->cfg.wr_secs) return;

    pstress->sec = 0U;
    pstress->state = FLASH_STRESS_ST_BLANK;
}

/**
 * Checks that one erased sector of the current loop is read back blank.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_blank(flash_stress_t * pstress)
{
    uint32_t addr = pstress->pos + pstress->sec * pstress->cfg.sec_size;

    if (!flash_stress_compare(pstress, addr, NULL, FLASH_STRESS_ERR_ERASE_VERIFY)) return;

    pstress->sec++;
    if (pstress->sec < pstress->cfg.wr_secs) return;

    pstress->sec = 0U;
    pstress->off = 0U;
    pstress->state = FLASH_STRESS_ST_PROG;
}

/**
 * Starts one program call with test data that differs for every loop and address.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_prog(flash_stress_t * pstress)
{
    uint32_t addr = pstress->pos + pstress->sec * pstress->cfg.sec_size;

    /*The data of a sector is built once, before its first program call*/
    if (pstress->off == 0U)
        flash_stress_fill(pstress->cfg.wbuf, pstress->cfg.sec_size, pstress->loop ^ addr);

    if (!flash_stress_io_prog(pstress, addr + pstress->off, pstress->cfg.wbuf + pstress->off,
            pstress->cfg.prog_size)) {
        memset(&pstress->last_err, 0, sizeof(pstress->last_err));
        flash_stress_fail(pstress, FLASH_STRESS_ERR_PROG, addr + pstress->off);
        return;
    }

    pstress->wait = true;
    pstress->off += pstress->cfg.prog_size;
    if (pstress->off < pstress->cfg.sec_size) return;

    pstress->off = 0U;
    pstress->sec++;
    if (pstress->sec < pstress->cfg.wr_secs) return;

    pstress->sec = 0U;
    pstress->pass = 0U;
    pstress->state = FLASH_STRESS_ST_VERIFY;
}

/**
 * Reads one programmed sector back and compares it with the data that was written.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_verify(flash_stress_t * pstress)
{
    uint32_t addr = pstress->pos + pstress->sec * pstress->cfg.sec_size;

    /*The expected data is rebuilt from the same seed, so one sector of buffer is enough*/
    flash_stress_fill(pstress->cfg.wbuf, pstress->cfg.sec_size, pstress->loop ^ addr);
    if (!flash_stress_compare(pstress, addr, pstress->cfg.wbuf, FLASH_STRESS_ERR_DATA_VERIFY))
        return;

    pstress->sec++;
    if (pstress->sec < pstress->cfg.wr_secs) return;

    pstress->sec = 0U;
    pstress->pass++;
    if (pstress->pass < pstress->cfg.reads) return;

    flash_stress_end_loop(pstress, true);
}

/**
 * Starts the erase of one used sector so that the test area is blank again.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_clean(flash_stress_t * pstress)
{
    uint32_t addr = flash_stress_addr(pstress, pstress->sec);
    uint32_t used = pstress->wr_cnt;

    /*Sectors that were never reached are still blank*/
    if (pstress->loop < used) used = pstress->loop;
    used *= pstress->cfg.wr_secs;

    if (pstress->sec >= used) {
        pstress->state = FLASH_STRESS_ST_DONE;
        return;
    }

    /*A failed erase is reported but not counted as a failed loop*/
    if (flash_stress_io_erase(pstress, addr)) {
        pstress->wait = true;
    } else {
        memset(&pstress->last_err, 0, sizeof(pstress->last_err));
        pstress->last_err.res = FLASH_STRESS_ERR_ERASE;
        pstress->last_err.loop = pstress->loop;
        pstress->last_err.addr = addr;
        if (pstress->ops.on_error)
            pstress->ops.on_error(&pstress->last_err);
    }

    pstress->sec++;
}

/**
 * Reads one sector through the registered interface and counts the bytes.
 * @param pstress Pointer to the test instance.
 * @param addr Flash address of the sector to read.
 * @param buf Pointer to the buffer that receives the data.
 * @return true if the low level read succeeded; otherwise false.
 */
static bool flash_stress_io_read(flash_stress_t * pstress, uint32_t addr, uint8_t * buf)
{
    if (!pstress->ops.read(addr, buf, pstress->cfg.sec_size)) return false;

    pstress->stat.rd_bytes += pstress->cfg.sec_size;
    return true;
}

/**
 * Starts a program through the registered interface and counts the bytes.
 * @param pstress Pointer to the test instance.
 * @param addr Flash address to program.
 * @param buf Pointer to the data to program.
 * @param len Number of bytes to program.
 * @return true if the low level program was started; otherwise false.
 */
static bool flash_stress_io_prog(flash_stress_t * pstress, uint32_t addr, const uint8_t * buf,
    uint32_t len)
{
    if (!pstress->ops.prog(addr, buf, len)) return false;

    pstress->stat.wr_bytes += len;
    return true;
}

/**
 * Starts the erase of one sector through the registered interface and counts the bytes.
 * @param pstress Pointer to the test instance.
 * @param addr Flash address of the sector to erase.
 * @return true if the low level erase was started; otherwise false.
 */
static bool flash_stress_io_erase(flash_stress_t * pstress, uint32_t addr)
{
    if (!pstress->ops.erase(addr)) return false;

    pstress->stat.er_bytes += pstress->cfg.sec_size;
    return true;
}
