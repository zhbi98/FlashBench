/**
 * @file spi_flash_stress.h
 *
 */

#ifndef __SPI_FLASH_STRESS_H__
#define __SPI_FLASH_STRESS_H__

/*********************
 *      INCLUDES
 *********************/

/**Only <stdint.h> is used here, this header is included by project files
 * that define their own bool type, which clashes with <stdbool.h>*/
#include <stdint.h>

/*********************
 *      DEFINES
 *********************/

/**Start of the default test area, 0x5F0000..0x5FFFFF (64KB)
 * It is the free gap between the parameter partition (ends at 0x54B000)
 * and the FPGA partition (starts at 0x600000)*/
#define SPI_FLASH_STRESS_BASE        0x5F0000U

/*Number of sectors in the test area, used in turn to spread the wear*/
#define SPI_FLASH_STRESS_SEC_CNT     16U

/*Erase unit of the 0x20 sector erase command*/
#define SPI_FLASH_STRESS_SEC_SIZE    4096U

/*Bytes of one page program command*/
#define SPI_FLASH_STRESS_PAGE_SIZE   256U

/*Sectors written by one loop, 1..SPI_FLASH_STRESS_SEC_CNT*/
#define SPI_FLASH_STRESS_WR_SECS     1U

/*Read back passes after each program*/
#define SPI_FLASH_STRESS_READS       4U

/**********************
 *      TYPEDEFS
 **********************/

/**Test request filled by the caller*/
typedef struct {
    uint32_t base;      /**< Start of the test area, aligned to the sector size*/
    uint32_t sec_cnt;   /**< Number of sectors in the test area*/
    uint32_t wr_secs;   /**< Sectors written by one loop, 1..sec_cnt*/
    uint32_t reads;     /**< Read back passes after each program*/
    uint32_t loops;     /**< Number of loops to run, 0 runs until stopped*/
} spi_flash_stress_req_t;

/**Live readings of the test, valid while it runs and after it has finished*/
typedef struct {
    uint32_t running;   /**< 1 while the test is running, 0 otherwise*/
    const char * phase; /**< Name of the current state, for example "ERASE" or "DONE"*/
    uint32_t loops;     /**< Number of loops requested, 0 means until stopped*/
    uint32_t loop;      /**< Number of loops finished*/
    uint32_t ok_cnt;    /**< Number of passed loops*/
    uint32_t err_cnt;   /**< Number of failed loops*/
    uint64_t rd_bytes;  /**< Bytes read from the flash*/
    uint64_t wr_bytes;  /**< Bytes programmed into the flash*/
    uint64_t er_bytes;  /**< Bytes erased, in whole sectors*/
    const char * err;   /**< Name of the most recent error, NULL when there is none*/
    uint32_t err_addr;  /**< Flash address of the most recent error*/
    uint8_t err_expect; /**< Expected value of the most recent verify error*/
    uint8_t err_got;    /**< Value read back by the most recent verify error*/
} spi_flash_stress_info_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Starts the stress test on the SPI flash, the work is done in spi_flash_stress_poll().
 * @param req Pointer to the test request.
 * @return 0 if the test was started; -1 if the request is invalid or a test is running.
 */
int32_t spi_flash_stress_start(const spi_flash_stress_req_t * req);

/**
 * Advances the test by one small step, to be called repeatedly from a main loop or a task.
 * @return 1 while the test is running; 0 once it is finished or when no test was started.
 */
int32_t spi_flash_stress_poll(void);

/**
 * Tells whether a test is running, normal flash accesses are skipped during that time.
 * @return 1 from the start of a test until it has finished; otherwise 0.
 */
int32_t spi_flash_stress_running(void);

/**
 * Requests the running test to end after the current loop.
 */
void spi_flash_stress_stop(void);

/**
 * Copies the live readings of the test, for example to refresh a display.
 * @param info Pointer to the structure that receives the readings.
 */
void spi_flash_stress_get(spi_flash_stress_info_t * info);

#endif /*__SPI_FLASH_STRESS_H__*/
