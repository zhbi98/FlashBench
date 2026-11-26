/**
 * @file sim_norflash.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "flash_stress.h"

/*********************
 *      DEFINES
 *********************/

#define SIM_BASE 0x5F0000U
#define SIM_SEC_SIZE 4096U
#define SIM_SEC_CNT 32U
#define SIM_PAGE_SIZE 256U

/*Polls the simulated flash stays busy after an erase or a program*/
#define SIM_ERASE_BUSY 20
#define SIM_PROG_BUSY 3

/*Byte of a sector that keeps one bit programmed once the sector is worn out*/
#define SIM_WEAR_BYTE 100U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool sim_read(uint32_t addr, uint8_t * buf, uint32_t len);
static bool sim_prog(uint32_t addr, const uint8_t * buf, uint32_t len);
static bool sim_erase(uint32_t addr);
static bool sim_busy(void);
static uint32_t sim_tick(void);
static void sim_reset(uint8_t fill);
static flash_stress_res_t sim_setup(const flash_stress_ops_t * ops, flash_stress_mode_t mode, uint32_t sec_cnt, uint32_t reads);
static void sim_run(void);
static uint32_t sim_dirty(uint32_t from, uint32_t cnt);
static void sim_expect(bool cond, const char * fmt, ...);
static void sim_test_rotate(void);
static void sim_test_read(void);
static void sim_test_endurance(void);
static void sim_test_waiting(void);
static void sim_test_config(void);

/**********************
 *  STATIC VARIABLES
 **********************/

/*Content of the simulated flash and the erase count of every sector*/
static uint8_t sim_mem[SIM_SEC_CNT * SIM_SEC_SIZE];
static uint32_t sim_er_cnt[SIM_SEC_CNT];

/*Remaining busy polls and the tick counter advanced by every busy poll*/
static int32_t sim_busy_left = 0;
static uint32_t sim_now = 0U;

/*Counters of the low level calls*/
static uint32_t sim_n_read = 0U;
static uint32_t sim_n_prog = 0U;
static uint32_t sim_n_erase = 0U;
static uint32_t sim_n_read_busy = 0U;

/*Injected faults*/
static uint32_t sim_flip_every = 0U; /*Flips one bit in every Nth read, 0 is off*/
static bool sim_overrun = false; /*The read writes one byte past the requested length*/
static bool sim_misalign = false; /*The read corrupts a byte when buf is not 4-byte aligned*/
static bool sim_short_odd = false; /*The read misses the last byte of odd lengths*/
static bool sim_dead_link = false; /*The flash does not answer, every read gives 0xFF*/
static uint32_t sim_wear_limit = 0U; /*Erases after which a bit stays programmed, 0 is never*/

/*Results of the last run, kept in variables so the checks stay short*/
static bool sim_done = false;
static bool sim_aborted = false;
static uint32_t sim_ok = 0U;
static uint32_t sim_err = 0U;
static uint32_t sim_cycles = 0U;

/*Buffers of the test, the read buffer has spare bytes so an overrun stays in memory*/
static uint8_t sim_wbuf[SIM_SEC_SIZE];
static uint8_t sim_rbuf_mem[SIM_SEC_SIZE + 8U];
static uint8_t * sim_rbuf = sim_rbuf_mem + 4U;

static flash_stress_t sim_inst;
static uint32_t sim_fails = 0U;

/*Non blocking driver with a tick, blocking driver, and one without program and erase*/
static const flash_stress_ops_t sim_ops_nb = {
    .read = sim_read,
    .prog = sim_prog,
    .erase = sim_erase,
    .busy = sim_busy,
    .tick = sim_tick,
};

static const flash_stress_ops_t sim_ops_blk = {
    .read = sim_read,
    .prog = sim_prog,
    .erase = sim_erase,
};

static const flash_stress_ops_t sim_ops_ro = {
    .read = sim_read,
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Runs every test of the framework on a simulated NOR flash with fault injection and
 * prints the result of every check, built on the host from the root of the repository:
 * gcc -std=c99 -Wall -Wextra -Isrc test/sim_norflash.c src/flash_stress.c -o sim_norflash
 * @return 0 if every check passed; otherwise 1.
 */
int main(void)
{
    sim_test_rotate();
    sim_test_read();
    sim_test_endurance();
    sim_test_waiting();
    sim_test_config();

    if (sim_fails != 0U) {
        printf("\n%u FAILED\n", (unsigned)sim_fails);
        return 1;
    }

    printf("\nALL PASSED\n");
    return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Reads the simulated flash and applies the injected read faults.
 * @param addr Flash address to read from.
 * @param buf Pointer to the buffer that receives the data.
 * @param len Number of bytes to read.
 * @return Always true.
 */
static bool sim_read(uint32_t addr, uint8_t * buf, uint32_t len)
{
    bool odd = (len & 1U) != 0U;
    bool unaligned = ((uintptr_t)buf & 3U) != 0U;
    bool flip = false;

    sim_n_read++;
    if (sim_busy_left > 0) sim_n_read_busy++;
    flip = (sim_flip_every != 0U) && ((sim_n_read % sim_flip_every) == 0U);

    memcpy(buf, &sim_mem[addr - SIM_BASE], len);

    if (sim_dead_link)
        memset(buf, 0xFF, len);
    if (sim_short_odd && odd) buf[len - 1U] = (uint8_t)~buf[len - 1U];
    if (sim_overrun) buf[len] = 0x00U;
    if (sim_misalign && unaligned && (len > 2U)) buf[1] ^= 0x10U;
    if (flip) buf[len / 2U] ^= 0x04U;

    return true;
}

/**
 * Programs the simulated flash, bits can only go from 1 to 0.
 * @param addr Flash address to program.
 * @param buf Pointer to the data to program.
 * @param len Number of bytes to program.
 * @return Always true.
 */
static bool sim_prog(uint32_t addr, const uint8_t * buf, uint32_t len)
{
    uint8_t * data = &sim_mem[addr - SIM_BASE];

    sim_n_prog++;
    for (uint32_t i = 0; i < len; i++)
        data[i] &= buf[i];

    sim_busy_left = SIM_PROG_BUSY;
    return true;
}

/**
 * Erases one sector of the simulated flash, a worn out sector keeps one bit programmed.
 * @param addr Flash address of the sector to erase.
 * @return Always true.
 */
static bool sim_erase(uint32_t addr)
{
    uint32_t sec = (addr - SIM_BASE) / SIM_SEC_SIZE;
    uint8_t * data = &sim_mem[sec * SIM_SEC_SIZE];
    bool worn = false;

    sim_n_erase++;
    sim_er_cnt[sec]++;
    worn = (sim_wear_limit != 0U) && (sim_er_cnt[sec] > sim_wear_limit);

    memset(data, 0xFF, SIM_SEC_SIZE);
    if (worn) data[SIM_WEAR_BYTE] &= 0xFEU;

    sim_busy_left = SIM_ERASE_BUSY;
    return true;
}

/**
 * Tells whether the simulated flash is busy, every call advances the tick.
 * @return true while an erase or a program is in progress; otherwise false.
 */
static bool sim_busy(void)
{
    sim_now++;
    if (sim_busy_left <= 0) return false;

    sim_busy_left--;
    return true;
}

/**
 * Returns the tick of the simulated flash.
 * @return Number of busy polls so far.
 */
static uint32_t sim_tick(void)
{
    return sim_now;
}

/**
 * Fills the simulated flash and clears the counters and the injected faults.
 * @param fill Value of every byte of the flash.
 */
static void sim_reset(uint8_t fill)
{
    memset(sim_mem, fill, sizeof(sim_mem));
    memset(sim_er_cnt, 0, sizeof(sim_er_cnt));
    sim_busy_left = 0;
    sim_now = 0U;
    sim_n_read = 0U;
    sim_n_prog = 0U;
    sim_n_erase = 0U;
    sim_n_read_busy = 0U;
    sim_flip_every = 0U;
    sim_overrun = false;
    sim_misalign = false;
    sim_short_odd = false;
    sim_dead_link = false;
    sim_wear_limit = 0U;
}

/**
 * Initializes the test instance on the simulated flash.
 * @param ops Pointer to the driver to register.
 * @param mode Kind of test.
 * @param sec_cnt Sectors in the test area.
 * @param reads Read back passes, or random reads per sector in READ.
 * @return Result of flash_stress_init().
 */
static flash_stress_res_t sim_setup(const flash_stress_ops_t * ops, flash_stress_mode_t mode,
    uint32_t sec_cnt, uint32_t reads)
{
    flash_stress_cfg_t cfg = {0};

    cfg.base = SIM_BASE;
    cfg.sec_size = SIM_SEC_SIZE;
    cfg.sec_cnt = sec_cnt;
    cfg.wr_secs = 1U;
    cfg.prog_size = SIM_PAGE_SIZE;
    cfg.reads = reads;
    cfg.wbuf = sim_wbuf;
    cfg.rbuf = sim_rbuf;
    cfg.mode = mode;

    return flash_stress_init(&sim_inst, ops, &cfg);
}

/**
 * Polls the test until it ends and keeps its results.
 */
static void sim_run(void)
{
    flash_stress_state_t state = FLASH_STRESS_ST_IDLE;

    while (flash_stress_poll(&sim_inst)) { }

    state = flash_stress_state(&sim_inst);
    sim_done = state == FLASH_STRESS_ST_DONE;
    sim_aborted = state == FLASH_STRESS_ST_ABORT;
    sim_ok = flash_stress_ok_cnt(&sim_inst);
    sim_err = flash_stress_err_cnt(&sim_inst);
    sim_cycles = flash_stress_cycles(&sim_inst);
}

/**
 * Counts the sectors of the simulated flash that are not blank.
 * @param from Index of the first sector.
 * @param cnt Number of sectors.
 * @return Number of sectors holding at least one byte other than 0xFF.
 */
static uint32_t sim_dirty(uint32_t from, uint32_t cnt)
{
    uint32_t dirty = 0U;

    for (uint32_t sec = from; sec < from + cnt; sec++) {
        const uint8_t * data = &sim_mem[sec * SIM_SEC_SIZE];
        uint32_t i = 0U;

        while ((i < SIM_SEC_SIZE) && (data[i] == 0xFFU))
            i++;
        if (i < SIM_SEC_SIZE) dirty++;
    }

    return dirty;
}

/**
 * Prints the result of one check and counts the failures.
 * @param cond true if the check passed.
 * @param fmt Format of the description, followed by its arguments.
 */
static void sim_expect(bool cond, const char * fmt, ...)
{
    va_list args;

    printf(cond ? "  ok   " : "  FAIL ");
    if (!cond) sim_fails++;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

/**
 * Tests the ROTATE mode.
 */
static void sim_test_rotate(void)
{
    const flash_stress_err_t * err = flash_stress_last_err(&sim_inst);

    printf("ROTATE\n");

    /*An unrelated erase is still running when the test starts*/
    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    sim_busy_left = 5;
    flash_stress_start(&sim_inst, 40U);
    sim_run();
    sim_expect(sim_done && (sim_ok == 40U), "40 loops pass, state DONE");
    sim_expect(sim_n_read_busy == 0U, "no read while the flash is busy (got %u)",
        (unsigned)sim_n_read_busy);
    sim_expect(sim_dirty(0U, 16U) == 0U, "area blank after the test");

    /*Power cut after 16 loops, then a short run*/
    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    flash_stress_start(&sim_inst, 0U);
    while (flash_stress_loop(&sim_inst) < 16U)
        flash_stress_poll(&sim_inst);
    sim_busy_left = 0;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    flash_stress_start(&sim_inst, 2U);
    sim_run();
    sim_expect(sim_dirty(0U, 16U) == 0U, "leftovers of an interrupted run are cleaned");

    /*Power cut during an erase*/
    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    flash_stress_start(&sim_inst, 0U);
    while (flash_stress_loop(&sim_inst) < 16U)
        flash_stress_poll(&sim_inst);
    memset(&sim_mem[3U * SIM_SEC_SIZE], 0xFF, SIM_SEC_SIZE / 2U);
    sim_busy_left = 0;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    flash_stress_start(&sim_inst, 2U);
    sim_run();
    sim_expect(sim_aborted && (err->res == FLASH_STRESS_ERR_NOT_BLANK),
        "half erased sector aborts without force");
    sim_inst.cfg.force = true;
    flash_stress_start(&sim_inst, 2U);
    sim_run();
    sim_expect(sim_done && (sim_dirty(0U, 16U) == 0U), "force runs and leaves the area blank");

    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    sim_flip_every = 97U;
    flash_stress_start(&sim_inst, 20U);
    sim_run();
    sim_expect((sim_err > 0U) && (err->bad_bits == 1U),
        "injected bit flips detected (%u failed loops)", (unsigned)sim_err);

    sim_reset(0x00U);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 16U, 4U);
    flash_stress_start(&sim_inst, 1U);
    sim_run();
    sim_expect(sim_aborted && (sim_n_erase == 0U), "foreign data: abort, nothing erased");
}

/**
 * Tests the READ mode.
 */
static void sim_test_read(void)
{
    const flash_stress_err_t * err = flash_stress_last_err(&sim_inst);
    uint32_t polls = 0U;
    bool once = false;
    bool one_bit = false;

    printf("READ\n");

    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 10U);
    sim_run();
    once = (sim_er_cnt[0] == 1U) && (sim_er_cnt[15] == 1U);
    sim_expect(sim_done && (sim_ok == 10U), "clean driver: 10 passes ok, %u reads",
        (unsigned)sim_n_read);
    sim_expect((sim_n_erase == 16U) && once, "each sector erased and written once per run");
    sim_expect(sim_dirty(0U, 16U) == 16U, "known data kept after the test");

    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 2U);
    sim_run();
    sim_expect((sim_ok == 2U) && (sim_er_cnt[0] == 2U), "next run writes the data once again");

    sim_dead_link = true;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 3U);
    sim_run();
    sim_dead_link = false;
    sim_expect((sim_err == 3U) && (err->res == FLASH_STRESS_ERR_DATA_VERIFY),
        "dead link reading 0xFF fails every pass");

    sim_flip_every = 1500U;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 10U);
    sim_run();
    sim_flip_every = 0U;
    one_bit = (err->res == FLASH_STRESS_ERR_DATA_VERIFY) && (err->bad_bits == 1U);
    sim_expect((sim_err > 0U) && one_bit, "occasional bit flip on the link detected");

    sim_overrun = true;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 1U);
    sim_run();
    sim_overrun = false;
    sim_expect((sim_err > 0U) && (err->res == FLASH_STRESS_ERR_READ_OVERRUN),
        "buffer overrun detected");

    sim_misalign = true;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 1U);
    sim_run();
    sim_misalign = false;
    sim_expect(sim_err > 0U, "DMA alignment bug detected");

    sim_short_odd = true;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 1U);
    sim_run();
    sim_short_odd = false;
    sim_expect(sim_err > 0U, "short read on odd lengths detected");

    sim_reset(0x00U);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 1U);
    sim_run();
    sim_expect(sim_aborted && (sim_n_erase == 0U), "foreign data: abort, nothing erased");

    /*Past the writes of the first loop, a stop ends the test within one sector*/
    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_READ, 16U, 64U);
    flash_stress_start(&sim_inst, 0U);
    while (flash_stress_loop(&sim_inst) < 1U)
        flash_stress_poll(&sim_inst);
    for (uint32_t i = 0; i < 100U; i++)
        flash_stress_poll(&sim_inst);
    flash_stress_stop(&sim_inst);
    while (flash_stress_poll(&sim_inst))
        polls++;
    sim_expect(polls <= 65U, "stop ends READ within one sector (%u polls)", (unsigned)polls);
}

/**
 * Tests the ENDURANCE mode.
 */
static void sim_test_endurance(void)
{
    const flash_stress_err_t * err = flash_stress_last_err(&sim_inst);
    const flash_stress_stat_t * stat = flash_stress_stat(&sim_inst);
    const flash_stress_time_t * er = &stat->er_time;
    bool equal = false;

    printf("ENDURANCE\n");

    /*The sectors wear out after 300 erases*/
    sim_reset(0xFFU);
    sim_wear_limit = 300U;
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ENDURANCE, 2U, 1U);
    sim_inst.cfg.max_errs = 3U;
    flash_stress_start(&sim_inst, 0U);
    sim_run();
    equal = sim_er_cnt[0] == sim_er_cnt[1];
    sim_expect((stat->first_fail == 301U) && (sim_err == 3U),
        "wear out at erase 301 found, first_fail=%u", (unsigned)stat->first_fail);
    sim_expect((err->res == FLASH_STRESS_ERR_ERASE_VERIFY) && (err->bad_bits == 1U),
        "stuck bit reported as erase error");
    sim_expect(equal && (sim_er_cnt[0] == sim_cycles),
        "every sector cycled once per loop (%u cycles)", (unsigned)sim_cycles);
    sim_expect((er->cnt == 2U * sim_cycles) && (er->max >= SIM_ERASE_BUSY),
        "erase timing recorded: max %u ticks", (unsigned)er->max);

    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ENDURANCE, 2U, 1U);
    flash_stress_start(&sim_inst, 50U);
    sim_run();
    sim_expect(sim_dirty(0U, 2U) == 2U, "area keeps its data after the test");
    sim_expect(sim_mem[SIM_SEC_SIZE + SIM_WEAR_BYTE] == 0x00U, "even cycle programs every bit");

    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ENDURANCE, 2U, 1U);
    flash_stress_start(&sim_inst, 10U);
    sim_run();
    sim_expect((stat->cycle_base == 50U) && (sim_cycles == 60U),
        "restart resumes: base %u, now %u", (unsigned)stat->cycle_base, (unsigned)sim_cycles);

    /*Power cut while sector 1 is erased*/
    memset(&sim_mem[SIM_SEC_SIZE], 0xFF, 300U);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ENDURANCE, 2U, 1U);
    flash_stress_start(&sim_inst, 10U);
    sim_run();
    sim_expect(sim_aborted, "half erased sector aborts without force");
    sim_inst.cfg.force = true;
    flash_stress_start(&sim_inst, 10U);
    sim_run();
    sim_expect(sim_done && (stat->cycle_base == 60U),
        "force resumes from the intact sector: base %u", (unsigned)stat->cycle_base);

    sim_reset(0xFFU);
    sim_setup(&sim_ops_blk, FLASH_STRESS_MODE_ENDURANCE, 1U, 2U);
    flash_stress_start(&sim_inst, 20U);
    sim_run();
    sim_expect(sim_ok == 20U, "blocking driver, single sector");
}

/**
 * Tests flash_stress_waiting(), which tells the caller when a poll would do nothing.
 */
static void sim_test_waiting(void)
{
    uint32_t waits = 0U;
    uint32_t polls = 0U;
    bool waiting = false;

    printf("WAITING\n");

    /*Nothing is waited for without a running test*/
    sim_reset(0xFFU);
    sim_setup(&sim_ops_nb, FLASH_STRESS_MODE_ROTATE, 4U, 2U);
    sim_expect(!flash_stress_waiting(NULL), "no instance");
    sim_expect(!flash_stress_waiting(&sim_inst), "not started");

    /*A started erase is waited for until the flash is no longer busy*/
    flash_stress_start(&sim_inst, 2U);
    while ((sim_n_erase == 0U) && flash_stress_poll(&sim_inst)) { }
    sim_expect(flash_stress_waiting(&sim_inst), "erase in progress");

    /*Polling only when nothing is waited for still runs the whole test*/
    while (flash_stress_running(&sim_inst)) {
        if (flash_stress_waiting(&sim_inst)) {
            waits++;
            continue;
        }
        flash_stress_poll(&sim_inst);
        polls++;
    }
    sim_run();
    sim_expect(sim_done && (sim_ok == 2U) && (sim_err == 0U), "test passes when waits are skipped");
    sim_expect((waits > 0U) && (polls > 0U), "waits were skipped");
    sim_expect(!flash_stress_waiting(&sim_inst), "finished");

    /*A blocking driver has finished when its function returns*/
    sim_reset(0xFFU);
    sim_setup(&sim_ops_blk, FLASH_STRESS_MODE_ROTATE, 4U, 2U);
    flash_stress_start(&sim_inst, 1U);
    while ((sim_n_erase == 0U) && flash_stress_poll(&sim_inst)) { }
    waiting = flash_stress_waiting(&sim_inst);
    sim_run();
    sim_expect(!waiting && (sim_ok == 1U), "blocking driver never waits");
}

/**
 * Tests the configuration checks of flash_stress_init().
 */
static void sim_test_config(void)
{
    flash_stress_cfg_t cfg = {0};
    flash_stress_res_t res = FLASH_STRESS_OK;

    printf("CONFIG\n");

    cfg.base = SIM_BASE;
    cfg.sec_size = SIM_SEC_SIZE;
    cfg.sec_cnt = 4U;
    cfg.wbuf = sim_wbuf;
    cfg.rbuf = sim_rbuf;

    cfg.mode = FLASH_STRESS_MODE_ROTATE;
    res = flash_stress_init(&sim_inst, &sim_ops_ro, &cfg);
    sim_expect(res == FLASH_STRESS_ERR_PARAM, "ROTATE needs prog");

    cfg.mode = FLASH_STRESS_MODE_READ;
    res = flash_stress_init(&sim_inst, &sim_ops_ro, &cfg);
    sim_expect(res == FLASH_STRESS_ERR_PARAM, "READ needs prog");

    cfg.mode = (flash_stress_mode_t)7;
    res = flash_stress_init(&sim_inst, &sim_ops_nb, &cfg);
    sim_expect(res == FLASH_STRESS_ERR_PARAM, "unknown mode rejected");

    cfg.mode = FLASH_STRESS_MODE_ROTATE;
    cfg.base = 0U;
    cfg.sec_size = 16U;
    res = flash_stress_init(&sim_inst, &sim_ops_nb, &cfg);
    sim_expect(res == FLASH_STRESS_ERR_PARAM, "16-byte sector rejected");

    cfg.base = 0xFFFFF000U;
    cfg.sec_size = SIM_SEC_SIZE;
    cfg.sec_cnt = 2U;
    res = flash_stress_init(&sim_inst, &sim_ops_nb, &cfg);
    sim_expect(res == FLASH_STRESS_ERR_PARAM, "area past 4GB rejected");

    cfg.mode = FLASH_STRESS_MODE_ENDURANCE;
    cfg.base = SIM_BASE;
    cfg.wr_secs = 9U;
    res = flash_stress_init(&sim_inst, &sim_ops_nb, &cfg);
    sim_expect(res == FLASH_STRESS_OK, "wr_secs ignored outside ROTATE");
}
