/**
 * @file flash_stress.h
 *
 */

#ifndef __FLASH_STRESS_H__
#define __FLASH_STRESS_H__

/*********************
 *      INCLUDES
 *********************/

#include <stdint.h>
#include <stdbool.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**Result of an operation, also used as the error type*/
typedef enum {
    FLASH_STRESS_OK = 0, /**< The operation passed*/
    FLASH_STRESS_ERR_PARAM, /**< Invalid argument or the instance cannot accept the call*/
    FLASH_STRESS_ERR_NOT_BLANK, /**< The test area holds foreign data, nothing was erased*/
    FLASH_STRESS_ERR_ERASE, /**< The low level erase function reported a failure*/
    FLASH_STRESS_ERR_PROG, /**< The low level program function reported a failure*/
    FLASH_STRESS_ERR_READ, /**< The low level read function reported a failure*/
    FLASH_STRESS_ERR_ERASE_VERIFY, /**< The sector is not all 0xFF after the erase*/
    FLASH_STRESS_ERR_DATA_VERIFY, /**< The data read back differs from the data written*/
} flash_stress_res_t;

/**State of the test, it advances by one small step per poll*/
typedef enum {
    FLASH_STRESS_ST_IDLE = 0, /**< Initialized, the test has not been started*/
    FLASH_STRESS_ST_CHECK, /**< Checking that the test area can be used*/
    FLASH_STRESS_ST_ERASE, /**< Erasing the sectors of the current loop*/
    FLASH_STRESS_ST_BLANK, /**< Checking that the erased sectors are blank*/
    FLASH_STRESS_ST_PROG, /**< Programming the test data*/
    FLASH_STRESS_ST_VERIFY, /**< Reading the test data back and comparing it*/
    FLASH_STRESS_ST_CLEAN, /**< Erasing the used sectors after the last loop*/
    FLASH_STRESS_ST_DONE, /**< The test finished and the test area is blank again*/
    FLASH_STRESS_ST_ABORT, /**< The test area cannot be used, nothing was erased*/
} flash_stress_state_t;

/**Details of one detected error*/
typedef struct {
    flash_stress_res_t res; /**< Error type*/
    uint32_t loop; /**< Index of the failed loop, starting from 0*/
    uint32_t addr; /**< Flash address, the first bad byte for verify errors*/
    uint8_t expect; /**< Expected value, verify errors only*/
    uint8_t got; /**< Value read back, verify errors only*/
    uint32_t bad_cnt; /**< Number of bad bytes in the sector, verify errors only*/
} flash_stress_err_t;

/*Low level read function, returns true on success*/
typedef bool (*flash_stress_read_cb_t)(uint32_t addr, uint8_t * buf, uint32_t len);
/*Low level function starting a program of up to prog_size bytes, returns true on success*/
typedef bool (*flash_stress_prog_cb_t)(uint32_t addr, const uint8_t * buf, uint32_t len);
/*Low level function starting the erase of the sector at addr, returns true on success*/
typedef bool (*flash_stress_erase_cb_t)(uint32_t addr);
/*Tells whether the last erase or program is still in progress (optional)*/
/*Leave it NULL when the erase and program functions return only after completion*/
typedef bool (*flash_stress_busy_cb_t)(void);
/*Called once for every detected error (optional)*/
typedef void (*flash_stress_error_cb_t)(const flash_stress_err_t * err);
/*Called after every finished loop with the updated counters (optional)*/
typedef void (*flash_stress_loop_cb_t)(uint32_t loop, uint32_t ok_cnt, uint32_t err_cnt);

/**Interface registered by the user
 * The framework never calls a flash driver or uses a flash address on its own,
 * every access goes through these functions and the user configuration*/
typedef struct {
    flash_stress_read_cb_t read; /**< Reads data from the flash*/
    flash_stress_prog_cb_t prog; /**< Starts programming data into an erased area*/
    flash_stress_erase_cb_t erase; /**< Starts erasing one sector*/
    flash_stress_busy_cb_t busy; /**< Reports a running erase or program, optional*/
    flash_stress_error_cb_t on_error; /**< Reports an error, optional*/
    flash_stress_loop_cb_t on_loop; /**< Reports the progress, optional*/
} flash_stress_ops_t;

/**Configuration structure initialized by the user*/
typedef struct {
    uint32_t base; /**< Start of the test area, aligned to the sector size*/
    uint32_t sec_size; /**< Size of the smallest erase unit in bytes*/
    uint32_t sec_cnt; /**< Sectors in the test area, used in turn to spread the wear*/
    uint32_t wr_secs; /**< Sectors written by one loop, 1..sec_cnt, 0 is treated as 1*/
    uint32_t prog_size; /**< Bytes per program call, divides sec_size, 0 means sec_size*/
    uint32_t reads; /**< Read back passes after each program, 0 is treated as 1*/
    uint8_t * wbuf; /**< Write buffer, sec_size bytes*/
    uint8_t * rbuf; /**< Read buffer, sec_size bytes*/
} flash_stress_cfg_t;

/**Byte traffic of the test, only successful low level calls are counted*/
typedef struct {
    uint64_t rd_bytes; /**< Bytes read from the flash*/
    uint64_t wr_bytes; /**< Bytes programmed into the flash*/
    uint64_t er_bytes; /**< Bytes erased, in whole sectors*/
} flash_stress_stat_t;

/**flash stress test instance*/
typedef struct {
    flash_stress_ops_t ops; /**< Stored copy of the registered interface*/
    flash_stress_cfg_t cfg; /**< Stored copy of the configuration*/
    bool ready; /**< Set once the instance has been initialized*/
    flash_stress_state_t state; /**< Current state of the test*/
    bool wait; /**< An erase or a program was started and may still be in progress*/
    bool stop; /**< A stop was requested, the test ends after the current loop*/
    uint32_t wr_cnt; /**< Write positions in the test area, used in turn*/
    uint32_t loops; /**< Number of loops to run, 0 runs until stopped*/
    uint32_t loop; /**< Number of loops finished*/
    uint32_t pos; /**< Flash address of the first sector of the current loop*/
    uint32_t sec; /**< Index of the sector handled by the current state*/
    uint32_t off; /**< Bytes of the current sector already programmed*/
    uint32_t pass; /**< Read back passes already finished in the current loop*/
    uint32_t ok_cnt; /**< Number of passed loops*/
    uint32_t err_cnt; /**< Number of failed loops*/
    flash_stress_err_t last_err; /**< Most recent error*/
    flash_stress_stat_t stat; /**< Byte traffic since the test was started*/
} flash_stress_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Initializes a test instance and registers the flash interface, without touching the flash.
 * @param pstress Pointer to the test instance to initialize.
 * @param ops Pointer to the flash interface to register, copied into the instance.
 * @param cfg Pointer to the test configuration, copied into the instance.
 * @return FLASH_STRESS_OK if the instance can be started; otherwise FLASH_STRESS_ERR_PARAM.
 */
flash_stress_res_t flash_stress_init(flash_stress_t * pstress, const flash_stress_ops_t * ops,
    const flash_stress_cfg_t * cfg);

/**
 * Starts a test, the work itself is done step by step in flash_stress_poll().
 * @param pstress Pointer to the test instance.
 * @param loops Number of loops to run, or 0 to run until flash_stress_stop() is called.
 * @return FLASH_STRESS_OK if the test was started; otherwise FLASH_STRESS_ERR_PARAM.
 */
flash_stress_res_t flash_stress_start(flash_stress_t * pstress, uint32_t loops);

/**
 * Advances the test by one small step and returns without waiting for the flash.
 * @param pstress Pointer to the test instance.
 * @return true while the test is running; false once it is finished, aborted or not started.
 */
bool flash_stress_poll(flash_stress_t * pstress);

/**
 * Requests the test to end after the current loop and to clean the test area.
 * @param pstress Pointer to the test instance.
 */
void flash_stress_stop(flash_stress_t * pstress);

/**
 * Returns whether the test still needs flash_stress_poll() to be called.
 * @param pstress Pointer to the test instance.
 * @return true while the test is running; otherwise false.
 */
bool flash_stress_running(const flash_stress_t * pstress);

/**
 * Returns the current state of the test.
 * @param pstress Pointer to the test instance.
 * @return Current state, or FLASH_STRESS_ST_IDLE when the instance is invalid.
 */
flash_stress_state_t flash_stress_state(const flash_stress_t * pstress);

/**
 * Returns the number of finished loops.
 * @param pstress Pointer to the test instance.
 * @return Number of finished loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_loop(const flash_stress_t * pstress);

/**
 * Returns the number of passed loops.
 * @param pstress Pointer to the test instance.
 * @return Number of passed loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_ok_cnt(const flash_stress_t * pstress);

/**
 * Returns the number of failed loops.
 * @param pstress Pointer to the test instance.
 * @return Number of failed loops, or 0 when the instance is invalid.
 */
uint32_t flash_stress_err_cnt(const flash_stress_t * pstress);

/**
 * Returns the most recent error of the test.
 * @param pstress Pointer to the test instance.
 * @return Pointer to the stored error, or NULL when the instance is invalid.
 */
const flash_stress_err_t * flash_stress_last_err(const flash_stress_t * pstress);

/**
 * Returns the byte traffic of the test.
 * @param pstress Pointer to the test instance.
 * @return Pointer to the stored statistics, or NULL when the instance is invalid.
 */
const flash_stress_stat_t * flash_stress_stat(const flash_stress_t * pstress);

#endif /*__FLASH_STRESS_H__*/
