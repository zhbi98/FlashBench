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

/*Header at the start of every programmed sector, magic, tag, cycle and inverted cycle*/
#define FLASH_STRESS_MAGIC_LEN 7U
#define FLASH_STRESS_TAG_POS 7U
#define FLASH_STRESS_CYCLE_POS 8U
#define FLASH_STRESS_CHECK_POS 12U
#define FLASH_STRESS_HDR_LEN 16U

/*0xA5 is the tag written by earlier versions, their data is still recognized*/
#define FLASH_STRESS_TAG_ROTATE 0xA5U
#define FLASH_STRESS_TAG_ENDURANCE 0xE5U
#define FLASH_STRESS_TAG_READ 0xD5U

#define FLASH_STRESS_BLANK 0xFFU
#define FLASH_STRESS_GUARD 0xC3U
#define FLASH_STRESS_SHORT_READ 16U
#define FLASH_STRESS_LCG_MUL 1664525U
#define FLASH_STRESS_LCG_INC 1013904223U
#define FLASH_STRESS_RNG_SEED 0x2545F491U

/*Kind of the operation being timed while waiting for the flash*/
#define FLASH_STRESS_OP_NONE 0U
#define FLASH_STRESS_OP_ERASE 1U
#define FLASH_STRESS_OP_PROG 2U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool flash_stress_cfg_valid(const flash_stress_ops_t * ops, const flash_stress_cfg_t * cfg);
static uint32_t flash_stress_addr(const flash_stress_t * pstress, uint32_t index);
static uint32_t flash_stress_rand(flash_stress_t * pstress);
static void flash_stress_put32(uint8_t * buf, uint32_t val);
static uint32_t flash_stress_get32(const uint8_t * buf);
static uint8_t flash_stress_tag(const uint8_t * buf);
static uint32_t flash_stress_not_blank(const uint8_t * buf, uint32_t len);
static uint32_t flash_stress_cycle(const flash_stress_t * pstress);
static void flash_stress_fill(const flash_stress_t * pstress, uint8_t * buf, uint32_t cycle, uint32_t addr);
static void flash_stress_time_add(flash_stress_time_t * t, uint32_t dt);
static void flash_stress_fail(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr);
static void flash_stress_abort(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr);
static bool flash_stress_check_data(flash_stress_t * pstress, const uint8_t * got, const uint8_t * ref, uint32_t len, uint32_t addr, flash_stress_res_t res);
static bool flash_stress_compare(flash_stress_t * pstress, uint32_t addr, const uint8_t * ref, flash_stress_res_t verify_res);
static bool flash_stress_guard_ok(flash_stress_t * pstress, const uint8_t * rbuf, uint32_t boff, uint32_t len, uint32_t addr);
static void flash_stress_pick(flash_stress_t * pstress, uint32_t * off, uint32_t * len, uint32_t * boff);
static void flash_stress_next_loop(flash_stress_t * pstress);
static void flash_stress_next_group(flash_stress_t * pstress);
static void flash_stress_end_loop(flash_stress_t * pstress, bool passed);
static void flash_stress_do_check(flash_stress_t * pstress);
static void flash_stress_do_erase(flash_stress_t * pstress);
static void flash_stress_do_blank(flash_stress_t * pstress);
static void flash_stress_do_prog(flash_stress_t * pstress);
static void flash_stress_do_verify(flash_stress_t * pstress);
static void flash_stress_do_read(flash_stress_t * pstress);
static void flash_stress_do_clean(flash_stress_t * pstress);
static bool flash_stress_io_read(flash_stress_t * pstress, uint32_t addr, uint8_t * buf, uint32_t len);
static bool flash_stress_io_prog(flash_stress_t * pstress, uint32_t addr, const uint8_t * buf, uint32_t len);
static bool flash_stress_io_erase(flash_stress_t * pstress, uint32_t addr);
static void flash_stress_io_timed(flash_stress_t * pstress, uint8_t op, uint32_t t0);

/**********************
 *  STATIC VARIABLES
 **********************/

/*Leading mark of the test data, identifies a sector left by a previous test*/
static const uint8_t flash_stress_magic[FLASH_STRESS_MAGIC_LEN] = {
    'F', 'R', 'W', 'T', 'E', 'S', 'T'
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

    /**ROTATE handles one group of wr_secs sectors per loop, at a position that changes every loop
     * The other modes handle every sector of the area in every loop, one after the other*/
    if (pstress->cfg.mode == FLASH_STRESS_MODE_ROTATE) {
        pstress->wr_cnt = pstress->cfg.sec_cnt / pstress->cfg.wr_secs;
        pstress->grp_cnt = 1U;
        pstress->grp_secs = pstress->cfg.wr_secs;
    } else {
        pstress->cfg.wr_secs = 1U;
        pstress->wr_cnt = 1U;
        pstress->grp_cnt = pstress->cfg.sec_cnt;
        pstress->grp_secs = 1U;
    }

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
    pstress->stop = false;
    pstress->dirty_end = 0U;
    pstress->rng = FLASH_STRESS_RNG_SEED;
    memset(&pstress->last_err, 0, sizeof(pstress->last_err));
    memset(&pstress->stat, 0, sizeof(pstress->stat));

    /*An erase or a program started before the test may still be running, wait for it first*/
    pstress->wait = true;
    pstress->wait_op = FLASH_STRESS_OP_NONE;

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

        /*The duration includes the delay until this poll, call poll often for a fine timing*/
        if (pstress->wait_op != FLASH_STRESS_OP_NONE) {
            flash_stress_time_t * t = &pstress->stat.pg_time;
            uint32_t dt = pstress->ops.tick() - pstress->t0;

            if (pstress->wait_op == FLASH_STRESS_OP_ERASE) t = &pstress->stat.er_time;
            flash_stress_time_add(t, dt);
            pstress->wait_op = FLASH_STRESS_OP_NONE;
        }
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
    case FLASH_STRESS_ST_READ:
        flash_stress_do_read(pstress);
        break;
    case FLASH_STRESS_ST_CLEAN:
        flash_stress_do_clean(pstress);
        break;
    default: break;
    }

    return flash_stress_running(pstress);
}

/**
 * Requests the test to end after the current loop, READ ends after the current sector.
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
 * Returns the statistics of the test, byte traffic, endurance cycles and timing.
 * @param pstress Pointer to the test instance.
 * @return Pointer to the stored statistics, or NULL when the instance is invalid.
 */
const flash_stress_stat_t * flash_stress_stat(const flash_stress_t * pstress)
{
    if (!pstress) return NULL;
    return &pstress->stat;
}

/**
 * Returns the cycles done on every sector of an ENDURANCE area, earlier runs included.
 * @param pstress Pointer to the test instance.
 * @return cycle_base plus the finished loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_cycles(const flash_stress_t * pstress)
{
    if (!pstress) return 0U;
    return pstress->stat.cycle_base + pstress->loop;
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
    uint64_t end = 0U;
    bool rotate = false;

    if ((!ops) || (!cfg)) return false;
    if ((uint32_t)cfg->mode > (uint32_t)FLASH_STRESS_MODE_ENDURANCE) return false;
    if ((!ops->read) || (!ops->prog) || (!ops->erase)) return false;
    if ((!cfg->wbuf) || (!cfg->rbuf)) return false;
    if (cfg->sec_cnt == 0U) return false;
    if (cfg->sec_size <= FLASH_STRESS_HDR_LEN) return false;
    if ((cfg->base % cfg->sec_size) != 0U) return false;

    /*The test area has to fit into the 32-bit address space*/
    end = (uint64_t)cfg->base + (uint64_t)cfg->sec_cnt * cfg->sec_size;
    if (end > 0x100000000ULL) return false;

    /*One write has to fit into the test area*/
    rotate = cfg->mode == FLASH_STRESS_MODE_ROTATE;
    if (rotate && (cfg->wr_secs > cfg->sec_cnt)) return false;

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
 * Returns the next value of the generator choosing the READ mode accesses (xorshift32).
 * @param pstress Pointer to the test instance.
 * @return Pseudo random 32-bit value.
 */
static uint32_t flash_stress_rand(flash_stress_t * pstress)
{
    uint32_t x = pstress->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    pstress->rng = x;

    return x;
}

/**
 * Stores a 32-bit value in little endian order.
 * @param buf Pointer to the 4 destination bytes.
 * @param val Value to store.
 */
static void flash_stress_put32(uint8_t * buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 0);
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val >> 16);
    buf[3] = (uint8_t)(val >> 24);
}

/**
 * Loads a 32-bit value stored in little endian order.
 * @param buf Pointer to the 4 source bytes.
 * @return Loaded value.
 */
static uint32_t flash_stress_get32(const uint8_t * buf)
{
    uint32_t val = buf[0];

    val |= (uint32_t)buf[1] << 8;
    val |= (uint32_t)buf[2] << 16;
    val |= (uint32_t)buf[3] << 24;

    return val;
}

/**
 * Returns the tag of a sector written by a test.
 * @param buf Pointer to the start of the sector data.
 * @return FLASH_STRESS_TAG_xxx of the mode that wrote it; 0 if the sector holds no test data.
 */
static uint8_t flash_stress_tag(const uint8_t * buf)
{
    uint8_t tag = buf[FLASH_STRESS_TAG_POS];

    if (memcmp(buf, flash_stress_magic, FLASH_STRESS_MAGIC_LEN) != 0) return 0U;
    if (tag == FLASH_STRESS_TAG_ROTATE) return tag;
    if (tag == FLASH_STRESS_TAG_ENDURANCE) return tag;
    if (tag == FLASH_STRESS_TAG_READ) return tag;

    return 0U;
}

/**
 * Finds the first byte of a buffer that is not blank.
 * @param buf Pointer to the data.
 * @param len Number of bytes.
 * @return Index of the first byte other than 0xFF, or len if every byte is blank.
 */
static uint32_t flash_stress_not_blank(const uint8_t * buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (buf[i] != FLASH_STRESS_BLANK) return i;

    return len;
}

/**
 * Returns the cycle number of the data programmed in the current loop.
 * @param pstress Pointer to the test instance.
 * @return Cycle stored in the sector header, always 0 for the READ data that never changes.
 */
static uint32_t flash_stress_cycle(const flash_stress_t * pstress)
{
    if (pstress->cfg.mode == FLASH_STRESS_MODE_READ) return 0U;
    return pstress->stat.cycle_base + pstress->loop + 1U;
}

/**
 * Fills a sector buffer with the header followed by the test data of a cycle.
 * @param pstress Pointer to the test instance.
 * @param buf Pointer to the buffer to fill, sec_size bytes.
 * @param cycle Cycle number stored in the header, it also seeds the data.
 * @param addr Flash address of the sector, it also seeds the data.
 */
static void flash_stress_fill(const flash_stress_t * pstress, uint8_t * buf, uint32_t cycle,
    uint32_t addr)
{
    uint32_t len = pstress->cfg.sec_size;
    uint32_t x = (cycle ^ addr) * FLASH_STRESS_LCG_MUL + FLASH_STRESS_LCG_INC;
    bool endurance = pstress->cfg.mode == FLASH_STRESS_MODE_ENDURANCE;
    uint8_t tag = FLASH_STRESS_TAG_ROTATE;

    if (endurance) tag = FLASH_STRESS_TAG_ENDURANCE;
    else if (pstress->cfg.mode == FLASH_STRESS_MODE_READ) tag = FLASH_STRESS_TAG_READ;

    memcpy(buf, flash_stress_magic, FLASH_STRESS_MAGIC_LEN);
    buf[FLASH_STRESS_TAG_POS] = tag;
    flash_stress_put32(buf + FLASH_STRESS_CYCLE_POS, cycle);
    flash_stress_put32(buf + FLASH_STRESS_CHECK_POS, ~cycle);

    /**Every other ENDURANCE cycle programs every bit of the sector,
     * random data leaves about half of the cells erased in each cycle*/
    if (endurance && ((cycle & 1U) == 0U)) {
        memset(buf + FLASH_STRESS_HDR_LEN, 0x00, len - FLASH_STRESS_HDR_LEN);
        return;
    }

    for (uint32_t i = FLASH_STRESS_HDR_LEN; i < len; i++) {
        x = x * FLASH_STRESS_LCG_MUL + FLASH_STRESS_LCG_INC;
        buf[i] = (uint8_t)(x >> 24);
    }
}

/**
 * Adds one measured duration to the timing of an operation.
 * @param t Pointer to the timing to update.
 * @param dt Measured duration in tick units.
 */
static void flash_stress_time_add(flash_stress_time_t * t, uint32_t dt)
{
    if ((t->cnt == 0U) || (dt < t->min)) t->min = dt;
    if (dt > t->max) t->max = dt;
    t->last = dt;
    t->sum += dt;
    t->cnt++;
}

/**
 * Stores an error of the current loop in last_err, notifies the user and moves on.
 * ROTATE ends the loop, the other modes go on with the next sector of the loop.
 * @param pstress Pointer to the test instance.
 * @param res Error type to store.
 * @param addr Flash address where the error was detected.
 */
static void flash_stress_fail(flash_stress_t * pstress, flash_stress_res_t res, uint32_t addr)
{
    flash_stress_stat_t * stat = &pstress->stat;

    /*The verify details in last_err are filled by the caller beforehand*/
    pstress->last_err.res = res;
    pstress->last_err.loop = pstress->loop;
    pstress->last_err.addr = addr;

    if (stat->first_fail == 0U) stat->first_fail = stat->cycle_base + pstress->loop + 1U;

    if (pstress->ops.on_error)
        pstress->ops.on_error(&pstress->last_err);

    /*Every sector of an ENDURANCE area still gets its cycle, so the cycle counts stay equal*/
    pstress->loop_bad = true;
    flash_stress_next_group(pstress);
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
 * Compares data read back with the expected data and reports the first difference.
 * @param pstress Pointer to the test instance.
 * @param got Pointer to the data read back.
 * @param ref Pointer to the expected data, or NULL to expect blank bytes.
 * @param len Number of bytes to compare.
 * @param addr Flash address of the first compared byte.
 * @param res Error type to report when the data differs.
 * @return true if the data matches; false after the error has been reported.
 */
static bool flash_stress_check_data(flash_stress_t * pstress, const uint8_t * got,
    const uint8_t * ref, uint32_t len, uint32_t addr, flash_stress_res_t res)
{
    uint32_t first = 0U;
    uint32_t bad = 0U;
    uint32_t bits = 0U;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t expect = ref ? ref[i] : FLASH_STRESS_BLANK;
        uint8_t diff = (uint8_t)(got[i] ^ expect);

        if (diff == 0U) continue;
        if (bad == 0U) first = i;
        bad++;

        /*Counting the bits tells a worn cell from a broken transfer*/
        while (diff != 0U) {
            diff &= (uint8_t)(diff - 1U);
            bits++;
        }
    }

    if (bad == 0U) return true;

    /*The most recent error is kept until the next one, a passed compare does not clear it*/
    memset(&pstress->last_err, 0, sizeof(pstress->last_err));
    pstress->last_err.expect = ref ? ref[first] : FLASH_STRESS_BLANK;
    pstress->last_err.got = got[first];
    pstress->last_err.bad_cnt = bad;
    pstress->last_err.bad_bits = bits;
    flash_stress_fail(pstress, res, addr + first);
    return false;
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

    /**Every byte is pre-filled with the inverse of its expected value,
     * so a byte the read does not deliver can never pass*/
    for (uint32_t i = 0; i < len; i++)
        rbuf[i] = (uint8_t)~(ref ? ref[i] : FLASH_STRESS_BLANK);

    if (!flash_stress_io_read(pstress, addr, rbuf, len)) {
        memset(&pstress->last_err, 0, sizeof(pstress->last_err));
        flash_stress_fail(pstress, FLASH_STRESS_ERR_READ, addr);
        return false;
    }

    return flash_stress_check_data(pstress, rbuf, ref, len, addr, verify_res);
}

/**
 * Checks that a read left the guard bytes around its target range untouched.
 * @param pstress Pointer to the test instance.
 * @param rbuf Pointer to the read buffer, sec_size bytes.
 * @param boff Offset of the target range in the buffer.
 * @param len Length of the target range.
 * @param addr Flash address of the read, reported on error.
 * @return true if every guard byte is intact; false after the error has been reported.
 */
static bool flash_stress_guard_ok(flash_stress_t * pstress, const uint8_t * rbuf, uint32_t boff,
    uint32_t len, uint32_t addr)
{
    uint32_t size = pstress->cfg.sec_size;
    uint32_t bad = 0U;
    uint8_t first_got = 0U;

    for (uint32_t i = 0; i < size; i++) {
        if ((i >= boff) && (i < boff + len)) continue;
        if (rbuf[i] == FLASH_STRESS_GUARD) continue;
        if (bad == 0U) first_got = rbuf[i];
        bad++;
    }

    if (bad == 0U) return true;

    memset(&pstress->last_err, 0, sizeof(pstress->last_err));
    pstress->last_err.expect = FLASH_STRESS_GUARD;
    pstress->last_err.got = first_got;
    pstress->last_err.bad_cnt = bad;
    flash_stress_fail(pstress, FLASH_STRESS_ERR_READ_OVERRUN, addr);
    return false;
}

/**
 * Chooses the flash offset, length and buffer offset of one READ mode access.
 * The mix covers whole sectors, short odd reads, reads across page boundaries
 * and unaligned buffers, the usual weak spots of a driver.
 * @param pstress Pointer to the test instance.
 * @param off Receives the offset of the read inside the sector.
 * @param len Receives the number of bytes to read, at least 1.
 * @param boff Receives the offset of the destination inside the read buffer.
 */
static void flash_stress_pick(flash_stress_t * pstress, uint32_t * off, uint32_t * len,
    uint32_t * boff)
{
    uint32_t size = pstress->cfg.sec_size;
    uint32_t page = pstress->cfg.prog_size;
    uint32_t r = flash_stress_rand(pstress);

    *off = 0U;
    *boff = 0U;

    switch (r & 3U) {
    case 0U:
        *len = size;
        return;
    case 1U:
        *len = 1U + flash_stress_rand(pstress) % FLASH_STRESS_SHORT_READ;
        break;
    case 2U:
        *len = 1U + flash_stress_rand(pstress) % page;
        break;
    default:
        *len = 1U + flash_stress_rand(pstress) % size;
        break;
    }

    /*An unaligned destination catches the DMA alignment bugs of a driver*/
    *boff = (r >> 2) & 3U;
    if (*len > size - *boff) *len = size - *boff;
    *off = flash_stress_rand(pstress) % (size - *len + 1U);
}

/**
 * Starts the next loop, or ends the test when the last loop is finished or a stop is requested.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_next_loop(flash_stress_t * pstress)
{
    bool finished = (pstress->loops != 0U) && (pstress->loop >= pstress->loops);
    bool rotate = pstress->cfg.mode == FLASH_STRESS_MODE_ROTATE;
    bool read = pstress->cfg.mode == FLASH_STRESS_MODE_READ;

    pstress->grp = 0U;
    pstress->sec = 0U;
    pstress->off = 0U;
    pstress->pass = 0U;
    pstress->loop_bad = false;

    if (finished || pstress->stop) {
        /*ENDURANCE keeps its data, the header of every sector records the cycle count*/
        pstress->state = rotate ? FLASH_STRESS_ST_CLEAN : FLASH_STRESS_ST_DONE;
        return;
    }

    if (rotate) {
        /*The write positions are used in turn to spread the wear*/
        uint32_t index = (pstress->loop % pstress->wr_cnt) * pstress->cfg.wr_secs;
        uint32_t end = index + pstress->cfg.wr_secs;

        if (end > pstress->dirty_end) pstress->dirty_end = end;
        pstress->pos = flash_stress_addr(pstress, index);
        pstress->state = FLASH_STRESS_ST_ERASE;
        return;
    }

    pstress->pos = pstress->cfg.base;
    pstress->state = read ? FLASH_STRESS_ST_READ : FLASH_STRESS_ST_ERASE;
}

/**
 * Moves on to the next sector group of the loop, or ends the loop after the last group.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_next_group(flash_stress_t * pstress)
{
    bool read = pstress->cfg.mode == FLASH_STRESS_MODE_READ;

    pstress->grp++;
    pstress->sec = 0U;
    pstress->off = 0U;
    pstress->pass = 0U;

    /*A READ loop over a large area takes long, a stop request ends it at the next sector*/
    if ((pstress->grp >= pstress->grp_cnt) || (read && pstress->stop)) {
        flash_stress_end_loop(pstress, !pstress->loop_bad);
        return;
    }

    pstress->pos = flash_stress_addr(pstress, pstress->grp * pstress->grp_secs);
    pstress->state = read ? FLASH_STRESS_ST_READ : FLASH_STRESS_ST_ERASE;
}

/**
 * Counts the finished loop, notifies the user and moves on to the next loop.
 * @param pstress Pointer to the test instance.
 * @param passed true if the loop passed; false if an error was detected.
 */
static void flash_stress_end_loop(flash_stress_t * pstress, bool passed)
{
    uint32_t max_errs = pstress->cfg.max_errs;

    if (passed) pstress->ok_cnt++;
    else pstress->err_cnt++;

    if (pstress->ops.on_loop)
        pstress->ops.on_loop(pstress->loop, pstress->ok_cnt, pstress->err_cnt);

    pstress->loop++;

    /*A worn out ENDURANCE area keeps failing, the limit ends the test*/
    if ((max_errs != 0U) && (pstress->err_cnt >= max_errs)) pstress->stop = true;

    flash_stress_next_loop(pstress);
}

/**
 * Checks one sector of the test area, it has to be blank or left by a previous test.
 * An ENDURANCE sector also gives back the cycle count recorded in its header.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_check(flash_stress_t * pstress)
{
    uint8_t * rbuf = pstress->cfg.rbuf;
    uint32_t size = pstress->cfg.sec_size;
    uint32_t addr = flash_stress_addr(pstress, pstress->sec);
    bool endurance = pstress->cfg.mode == FLASH_STRESS_MODE_ENDURANCE;
    flash_stress_stat_t * stat = &pstress->stat;
    uint32_t first = 0U;
    uint8_t tag = 0U;

    /*A read that returns no data must not look blank*/
    memset(rbuf, 0x00, size);
    if (!flash_stress_io_read(pstress, addr, rbuf, size)) {
        flash_stress_abort(pstress, FLASH_STRESS_ERR_READ, addr);
        return;
    }

    tag = flash_stress_tag(rbuf);
    first = flash_stress_not_blank(rbuf, size);

    if (tag != 0U) {
        /*Test data, erased by the cleaning even if this run does not reach the sector*/
        pstress->dirty_end = pstress->sec + 1U;

        /*An ENDURANCE area continues the count recorded in its headers*/
        if (endurance && (tag == FLASH_STRESS_TAG_ENDURANCE)) {
            uint32_t cycle = flash_stress_get32(rbuf + FLASH_STRESS_CYCLE_POS);
            uint32_t check = flash_stress_get32(rbuf + FLASH_STRESS_CHECK_POS);

            if ((check == ~cycle) && (cycle > stat->cycle_base)) stat->cycle_base = cycle;
        }
    } else if (first < size) {
        /*Refuse to touch an area that holds anything but blank or test data*/
        if (!pstress->cfg.force) {
            pstress->last_err.expect = FLASH_STRESS_BLANK;
            pstress->last_err.got = rbuf[first];
            flash_stress_abort(pstress, FLASH_STRESS_ERR_NOT_BLANK, addr + first);
            return;
        }
        pstress->dirty_end = pstress->sec + 1U;
    }

    pstress->sec++;
    if (pstress->sec >= pstress->cfg.sec_cnt)
        flash_stress_next_loop(pstress);
}

/**
 * Starts the erase of one sector of the current group.
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
    if (pstress->sec < pstress->grp_secs) return;

    pstress->sec = 0U;
    pstress->state = FLASH_STRESS_ST_BLANK;
}

/**
 * Checks that one erased sector of the current group is read back blank.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_blank(flash_stress_t * pstress)
{
    uint32_t addr = pstress->pos + pstress->sec * pstress->cfg.sec_size;

    if (!flash_stress_compare(pstress, addr, NULL, FLASH_STRESS_ERR_ERASE_VERIFY)) return;

    pstress->sec++;
    if (pstress->sec < pstress->grp_secs) return;

    pstress->sec = 0U;
    pstress->off = 0U;
    pstress->state = FLASH_STRESS_ST_PROG;
}

/**
 * Starts one program call with test data that differs for every cycle and address.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_prog(flash_stress_t * pstress)
{
    uint32_t addr = pstress->pos + pstress->sec * pstress->cfg.sec_size;

    /*The data of a sector is built once, before its first program call*/
    if (pstress->off == 0U)
        flash_stress_fill(pstress, pstress->cfg.wbuf, flash_stress_cycle(pstress), addr);

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
    if (pstress->sec < pstress->grp_secs) return;

    pstress->sec = 0U;

    /*READ wrote its known data once, wbuf still holds it, the random reads verify it*/
    if (pstress->cfg.mode == FLASH_STRESS_MODE_READ) {
        pstress->pass = 1U;
        pstress->state = FLASH_STRESS_ST_READ;
        return;
    }

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
    flash_stress_fill(pstress, pstress->cfg.wbuf, flash_stress_cycle(pstress), addr);
    if (!flash_stress_compare(pstress, addr, pstress->cfg.wbuf, FLASH_STRESS_ERR_DATA_VERIFY))
        return;

    pstress->sec++;
    if (pstress->sec < pstress->grp_secs) return;

    pstress->sec = 0U;
    pstress->pass++;
    if (pstress->pass < pstress->cfg.reads) return;

    flash_stress_next_group(pstress);
}

/**
 * Runs one READ step, the first loop writes known data into every sector once,
 * then every loop reads random ranges of the sector and compares them with it.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_read(flash_stress_t * pstress)
{
    uint32_t size = pstress->cfg.sec_size;
    uint8_t * ref = pstress->cfg.wbuf;
    uint8_t * rbuf = pstress->cfg.rbuf;
    uint32_t addr = pstress->pos;
    uint32_t off = 0U;
    uint32_t len = 0U;
    uint32_t boff = 0U;
    uint32_t from = 0U;
    uint8_t * got = NULL;
    const uint8_t * expect = NULL;

    /*The known data never changes, it is rebuilt from the address instead of being stored*/
    if (pstress->pass == 0U) {
        flash_stress_fill(pstress, ref, 0U, addr);
        pstress->pass = 1U;

        /*Written once in the first loop, erase, blank check and program, then back here*/
        if (pstress->loop == 0U) {
            pstress->sec = 0U;
            pstress->state = FLASH_STRESS_ST_ERASE;
        }
        return;
    }

    flash_stress_pick(pstress, &off, &len, &boff);
    from = addr + off;
    got = rbuf + boff;
    expect = ref + off;

    /*Guard bytes around the target range, inverted known data inside it*/
    memset(rbuf, FLASH_STRESS_GUARD, size);
    for (uint32_t i = 0; i < len; i++)
        got[i] = (uint8_t)~expect[i];

    if (!flash_stress_io_read(pstress, from, got, len)) {
        memset(&pstress->last_err, 0, sizeof(pstress->last_err));
        flash_stress_fail(pstress, FLASH_STRESS_ERR_READ, from);
        return;
    }

    if (!flash_stress_guard_ok(pstress, rbuf, boff, len, from)) return;
    if (!flash_stress_check_data(pstress, got, expect, len, from, FLASH_STRESS_ERR_DATA_VERIFY))
        return;

    /*Pass 0 set up the known data, passes 1..reads are the random reads*/
    pstress->pass++;
    if (pstress->pass <= pstress->cfg.reads) return;

    flash_stress_next_group(pstress);
}

/**
 * Starts the erase of one sector that may hold test data, so the ROTATE area is blank again.
 * @param pstress Pointer to the test instance.
 */
static void flash_stress_do_clean(flash_stress_t * pstress)
{
    uint32_t addr = flash_stress_addr(pstress, pstress->sec);

    /*Covers the positions used by this run and the test data found by the check*/
    if (pstress->sec >= pstress->dirty_end) {
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
 * Reads data through the registered interface and counts the bytes.
 * @param pstress Pointer to the test instance.
 * @param addr Flash address to read from.
 * @param buf Pointer to the buffer that receives the data.
 * @param len Number of bytes to read.
 * @return true if the low level read succeeded; otherwise false.
 */
static bool flash_stress_io_read(flash_stress_t * pstress, uint32_t addr, uint8_t * buf,
    uint32_t len)
{
    if (!pstress->ops.read(addr, buf, len)) return false;

    pstress->stat.rd_bytes += len;
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
    uint32_t t0 = pstress->ops.tick ? pstress->ops.tick() : 0U;

    if (!pstress->ops.prog(addr, buf, len)) return false;

    pstress->stat.wr_bytes += len;
    flash_stress_io_timed(pstress, FLASH_STRESS_OP_PROG, t0);
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
    uint32_t t0 = pstress->ops.tick ? pstress->ops.tick() : 0U;

    if (!pstress->ops.erase(addr)) return false;

    pstress->stat.er_bytes += pstress->cfg.sec_size;
    flash_stress_io_timed(pstress, FLASH_STRESS_OP_ERASE, t0);
    return true;
}

/**
 * Times a started erase or program, at once or when the next polls see busy clear.
 * @param pstress Pointer to the test instance.
 * @param op FLASH_STRESS_OP_ERASE or FLASH_STRESS_OP_PROG.
 * @param t0 Tick taken just before the operation was started.
 */
static void flash_stress_io_timed(flash_stress_t * pstress, uint8_t op, uint32_t t0)
{
    flash_stress_time_t * t = &pstress->stat.pg_time;

    if (!pstress->ops.tick) return;
    if (op == FLASH_STRESS_OP_ERASE) t = &pstress->stat.er_time;

    /*A blocking driver has already finished, a non blocking one is timed when busy clears*/
    if (!pstress->ops.busy) {
        flash_stress_time_add(t, pstress->ops.tick() - t0);
        return;
    }

    pstress->wait_op = op;
    pstress->t0 = t0;
}
