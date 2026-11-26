/**
 * @file run_sample.c
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

/*Sector and page size of the flash, from its datasheet*/
#define STRESS_SEC_SIZE 4096U
#define STRESS_PAGE_SIZE 256U

/*Free area of ROTATE and READ, check it against the partition table of the application*/
#define STRESS_BASE 0x5F0000U
#define STRESS_SEC_CNT 14U

/*Dedicated area of ENDURANCE, its sectors are worn out on purpose*/
#define STRESS_END_BASE 0x5FE000U
#define STRESS_END_SEC_CNT 2U

/*Read back passes of ROTATE, random reads per sector of READ, passes of ENDURANCE*/
#define STRESS_ROTATE_READS 4U
#define STRESS_READ_READS 32U
#define STRESS_END_READS 1U

/*Failed ENDURANCE cycles ending the test*/
#define STRESS_END_ERRS 10U

/*Longest time one call of stress_process() works without a break, in ticks of the port*/
#define STRESS_SLICE 5U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool stress_init(flash_stress_mode_t mode);

/**********************
 *  STATIC VARIABLES
 **********************/

/**Binds the driver of the port sample and the callbacks of the display sample
 * The table has to stay valid while the instance is used*/
static const flash_stress_ops_t stress_ops = {
    .read = stress_read,
    .prog = stress_prog,
    .erase = stress_erase,
    .busy = stress_busy,
    .on_error = stress_on_error,
    .on_loop = stress_on_loop,
    .tick = stress_tick,
};

static flash_stress_t stress_inst = {0};
static uint8_t stress_wbuf[STRESS_SEC_SIZE] = {0};
static uint8_t stress_rbuf[STRESS_SEC_SIZE] = {0};

/*Set from the start of a test until its end has been reported*/
static bool stress_active = false;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Configures the instance for a mode and starts a test, to be called after the flash
 * driver and the tick are ready. The flash is accessed by the next stress_process().
 * @param mode Kind of test.
 * @return true if the test was started; otherwise false.
 */
bool stress_begin(flash_stress_mode_t mode)
{
    flash_stress_t * pstress = &stress_inst;

    if (!stress_init(mode)) return false;
    if (flash_stress_start(pstress, 0U) != FLASH_STRESS_OK) return false;

    stress_active = true;
    return true;
}

/**
 * Advances the test, to be called continuously from the main loop or one task, never from
 * an interrupt. It works until the flash has to be waited for or the time slice is used
 * up, so the loop keeps running during an erase. The end of a test is reported once
 * through stress_on_end() of the mode sample.
 */
void stress_process(void)
{
    flash_stress_t * pstress = &stress_inst;
    uint32_t t0 = stress_tick();

    while (flash_stress_poll(pstress)) {
        /*Nothing can be done until the erase or the program has finished*/
        if (flash_stress_waiting(pstress)) break;

        /*The other work of the caller gets its turn*/
        if ((stress_tick() - t0) >= STRESS_SLICE) break;
    }

    if (!stress_active) return;
    if (flash_stress_running(pstress)) return;

    /*The test has just ended*/
    stress_active = false;
    stress_on_end();
}

/**
 * Tells whether a test runs, the other flash accesses of the application wait or are
 * skipped meanwhile, for example a save of the settings.
 * @return true from the start of a test until its end has been reported; otherwise false.
 */
bool stress_running(void)
{
    return stress_active;
}

/**
 * Gives the instance to the other samples, to read its data or to stop it.
 * @return Pointer to the test instance.
 */
flash_stress_t * stress_object(void)
{
    return &stress_inst;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Initializes the instance with the area and the parameters of a mode.
 * @param mode Kind of test.
 * @return true if the configuration is valid; otherwise false.
 */
static bool stress_init(flash_stress_mode_t mode)
{
    flash_stress_cfg_t cfg = {0};
    flash_stress_res_t res = FLASH_STRESS_OK;

    /*The same flash geometry and buffers for every mode*/
    cfg.sec_size = STRESS_SEC_SIZE;
    cfg.prog_size = STRESS_PAGE_SIZE;
    cfg.wbuf = stress_wbuf;
    cfg.rbuf = stress_rbuf;
    cfg.mode = mode;

    switch (mode) {
    case FLASH_STRESS_MODE_READ:
        cfg.base = STRESS_BASE;
        cfg.sec_cnt = STRESS_SEC_CNT;
        cfg.reads = STRESS_READ_READS;
        break;
    case FLASH_STRESS_MODE_ENDURANCE:
        cfg.base = STRESS_END_BASE;
        cfg.sec_cnt = STRESS_END_SEC_CNT;
        cfg.reads = STRESS_END_READS;
        cfg.max_errs = STRESS_END_ERRS;
        break;
    default:
        cfg.base = STRESS_BASE;
        cfg.sec_cnt = STRESS_SEC_CNT;
        cfg.wr_secs = 1U;
        cfg.reads = STRESS_ROTATE_READS;
        break;
    }

    res = flash_stress_init(&stress_inst, &stress_ops, &cfg);
    return res == FLASH_STRESS_OK;
}
