/**
 * @file display_sample.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdio.h>
#include "flash_stress.h"

/*Include the declarations of the stress_* functions of the application here*/

/*********************
 *      DEFINES
 *********************/
/*Size of one line of text on the display, and of one byte count*/
#define STRESS_TEXT_LEN 40U
#define STRESS_SIZE_LEN 12U

/*ENDURANCE cycles between two progress lines on the log*/
#define STRESS_LOG_EVERY 1000U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void size_text(char * text, uint32_t size, uint64_t bytes);
static const char * mode_name(flash_stress_mode_t mode);
static const char * state_name(flash_stress_state_t state);
static const char * err_name(flash_stress_res_t res);

/**Display of the application, replace it by an LCD driver or a label of the
 * GUI*/
extern void board_display_line(uint32_t row, const char * text);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/
/**One tagged line on the debug UART, replace printf by the UART output of the
 * application*/
#define STRESS_LOG(fmt, ...) printf("[FLASH STRESS] " fmt "\r\n", ##__VA_ARGS__)

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
/**
 * Logs every detected error, the framework calls it from stress_process().
 * @param err  Pointer to the error, valid only during the call.
 */
void stress_on_error(const flash_stress_err_t * err)
{
    const char * name = err_name(err->res);
    unsigned long loop = err->loop;
    unsigned long addr = err->addr;
    unsigned long bits = err->bad_bits;

    /*Failed low level calls carry no compare details*/
    if (err->bad_cnt == 0U) {
        STRESS_LOG("loop %lu %s at 0x%06lX", loop, name, addr);
        return;
    }

    STRESS_LOG("loop %lu %s at 0x%06lX expect 0x%02X got 0x%02X bits %lu", loop,
               name, addr, err->expect, err->got, bits);
}

/**
 * Logs the counters after every loop, the framework calls it from
 * stress_process().
 * @param loop     Index of the finished loop, starting from 0.
 * @param ok_cnt   Number of passed loops.
 * @param err_cnt  Number of failed loops.
 */
void stress_on_loop(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt)
{
    bool endurance = stress_mode() == FLASH_STRESS_MODE_ENDURANCE;
    bool due = ((loop + 1U) % STRESS_LOG_EVERY) == 0U;
    unsigned long n = loop;
    unsigned long ok = ok_cnt;
    unsigned long bad = err_cnt;

    /*ENDURANCE runs a loop per cycle, one line per cycle would flood the UART*/
    if (endurance && (!due)) return;

    STRESS_LOG("loop %lu: ok=%lu err=%lu", n, ok, bad);
}

/**
 * Reads the live data of the test and draws it as six text lines,
 * to be called periodically, for example every 500 ms.
 */
void stress_show(void)
{
    flash_stress_t * pstress = stress_object();
    const flash_stress_stat_t * stat = flash_stress_stat(pstress);
    const flash_stress_err_t * err = flash_stress_last_err(pstress);
    const flash_stress_time_t * er = &stat->er_time;
    bool endurance = stress_mode() == FLASH_STRESS_MODE_ENDURANCE;
    const char * mode = mode_name(stress_mode());
    const char * state = state_name(flash_stress_state(pstress));
    const char * name = err_name(err->res);
    unsigned long loops = flash_stress_loop(pstress);
    unsigned long ok_cnt = flash_stress_ok_cnt(pstress);
    unsigned long err_cnt = flash_stress_err_cnt(pstress);
    unsigned long cycles = flash_stress_cycles(pstress);
    unsigned long first = stat->first_fail;
    unsigned long er_avg = 0U;
    unsigned long er_max = er->max;
    unsigned long addr = err->addr;
    unsigned int expect = err->expect;
    unsigned int got = err->got;
    char rd[STRESS_SIZE_LEN] = {0};
    char wr[STRESS_SIZE_LEN] = {0};
    char text[STRESS_TEXT_LEN] = {0};

    if (er->cnt != 0U) er_avg = (unsigned long)(er->sum / er->cnt);
    size_text(rd, sizeof(rd), stat->rd_bytes);
    size_text(wr, sizeof(wr), stat->wr_bytes);

    /*Mode and state, for example "READ  VERIFY"*/
    snprintf(text, sizeof(text), "%s  %s", mode, state);
    board_display_line(0U, text);

    /*Finished, passed and failed loops*/
    snprintf(text, sizeof(text), "Loop %lu  Pass %lu  Fail %lu", loops, ok_cnt,
             err_cnt);
    board_display_line(1U, text);

    /*Byte traffic, below 1 MB in KB, above in MB*/
    snprintf(text, sizeof(text), "Read %s  Write %s", rd, wr);
    board_display_line(2U, text);

    /**Cycles of every ENDURANCE sector, and the loop or cycle of the first
     * failure*/
    if (endurance)
        snprintf(text, sizeof(text), "Cycle %lu  First fail %lu", cycles,
                 first);
    else
        snprintf(text, sizeof(text), "First fail %lu", first);
    board_display_line(3U, text);

    /*Sector erase time in ms, it grows as the sectors wear out*/
    snprintf(text, sizeof(text), "Erase avg %lu max %lu ms", er_avg, er_max);
    board_display_line(4U, text);

    /*Most recent error, kept until the next one*/
    if (err->res == FLASH_STRESS_OK)
        snprintf(text, sizeof(text), "Error None");
    else
        snprintf(text, sizeof(text), "Error %s @%06lX %02X>%02X", name, addr,
                 expect, got);
    board_display_line(5U, text);
}

/**
 * Logs the result of a test that has just ended, called by the mode sample.
 */
void stress_report(void)
{
    flash_stress_t * pstress = stress_object();
    const flash_stress_stat_t * stat = flash_stress_stat(pstress);
    const flash_stress_err_t * err = flash_stress_last_err(pstress);
    const char * mode = mode_name(stress_mode());
    unsigned long ok_cnt = flash_stress_ok_cnt(pstress);
    unsigned long err_cnt = flash_stress_err_cnt(pstress);
    unsigned long cycles = flash_stress_cycles(pstress);
    unsigned long first = stat->first_fail;
    unsigned long addr = err->addr;
    char rd[STRESS_SIZE_LEN] = {0};

    /*The area holds foreign data or could not be read, nothing was erased*/
    if (flash_stress_state(pstress) == FLASH_STRESS_ST_ABORT) {
        STRESS_LOG("%s abort: %s at 0x%06lX", mode, err_name(err->res), addr);
        return;
    }

    size_text(rd, sizeof(rd), stat->rd_bytes);
    STRESS_LOG("%s end: ok=%lu err=%lu read=%s", mode, ok_cnt, err_cnt, rd);

    if (stress_mode() == FLASH_STRESS_MODE_ENDURANCE)
        STRESS_LOG("cycles=%lu first fail=%lu", cycles, first);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
/**
 * Writes a byte count as text, in KB below 1 MB and in MB above.
 * @param text   Pointer to the destination text.
 * @param size   Size of the destination text.
 * @param bytes  Byte count to write.
 */
static void size_text(char * text, uint32_t size, uint64_t bytes)
{
    unsigned long kb = (unsigned long)(bytes >> 10);

    if (kb < 1024U)
        snprintf(text, size, "%luKB", kb);
    else
        snprintf(text, size, "%luMB", kb >> 10);
}

/**
 * Returns the display name of a mode.
 * @param mode  Kind of test.
 * @return      Pointer to a constant string.
 */
static const char * mode_name(flash_stress_mode_t mode)
{
    switch (mode) {
        case FLASH_STRESS_MODE_READ: return "READ";
        case FLASH_STRESS_MODE_ENDURANCE: return "ENDURANCE";
        default: return "ROTATE";
    }
}

/**
 * Returns the display name of a state.
 * @param state  State of the test.
 * @return       Pointer to a constant string.
 */
static const char * state_name(flash_stress_state_t state)
{
    switch (state) {
        case FLASH_STRESS_ST_CHECK: return "CHECK";
        case FLASH_STRESS_ST_ERASE: return "ERASE";
        case FLASH_STRESS_ST_BLANK: return "BLANK";
        case FLASH_STRESS_ST_PROG: return "WRITE";
        case FLASH_STRESS_ST_VERIFY: return "VERIFY";
        case FLASH_STRESS_ST_READ: return "READ";
        case FLASH_STRESS_ST_CLEAN: return "CLEAN";
        case FLASH_STRESS_ST_DONE: return "DONE";
        case FLASH_STRESS_ST_ABORT: return "ABORT";
        default: return "IDLE";
    }
}

/**
 * Returns the display name of an error type.
 * @param res  Error type.
 * @return     Pointer to a constant string.
 */
static const char * err_name(flash_stress_res_t res)
{
    switch (res) {
        case FLASH_STRESS_ERR_NOT_BLANK: return "AREA USED";
        case FLASH_STRESS_ERR_ERASE: return "ERASE IO";
        case FLASH_STRESS_ERR_PROG: return "WRITE IO";
        case FLASH_STRESS_ERR_READ: return "READ IO";
        case FLASH_STRESS_ERR_ERASE_VERIFY: return "ERASE";
        case FLASH_STRESS_ERR_DATA_VERIFY: return "DATA";
        case FLASH_STRESS_ERR_READ_OVERRUN: return "OVERRUN";
        default: return "None";
    }
}
