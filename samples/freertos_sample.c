/**
 * @file freertos_sample.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "flash_stress.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/*Include the declarations of the stress_* functions of the application here*/

/*********************
 *      DEFINES
 *********************/
/*Period of the test task, a step returns at once while the flash is busy*/
#define STRESS_TASK_MS 2U

/*Requests to the test task, a mode is sent as its flash_stress_mode_t value*/
#define STRESS_REQ_STOP 0xFFU
#define STRESS_QUEUE_LEN 4U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void task_main(void * argument_p);
static void handle_req(uint8_t req);

/**********************
 *  STATIC VARIABLES
 **********************/
/*Requests of the other tasks, only the test task calls the framework*/
static QueueHandle_t queue = NULL;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
/**
 * Creates the request queue and the test task, once after the flash driver is
 * ready.
 * The task needs INCLUDE_vTaskDelay = 1 in FreeRTOSConfig.h.
 * @param stack     Stack depth of the task, in words of the port.
 * @param priority  Priority of the task, below the tasks that must not be
 *                  delayed.
 * @return          true if the queue and the task were created; otherwise
 *                  false.
 */
bool stress_task_create(configSTACK_DEPTH_TYPE stack, UBaseType_t priority)
{
    BaseType_t res = pdFAIL;

    queue = xQueueCreate(STRESS_QUEUE_LEN, sizeof(uint8_t));
    if (queue == NULL) return false;

    res = xTaskCreate(task_main, "stress", stack, NULL, priority, NULL);
    return res == pdPASS;
}

/**
 * Sends a request from another task, for example from the menu of the GUI task.
 * @param req  Mode to start, or STRESS_REQ_STOP to stop the test.
 * @return     true if the request was queued; otherwise false.
 */
bool stress_request(uint8_t req)
{
    BaseType_t res = pdFAIL;

    if (queue == NULL) return false;

    res = xQueueSend(queue, &req, 0);
    return res == pdPASS;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
/**
 * Owns the test, only this task calls the framework and accesses the test area.
 * @param argument_p  Unused task argument.
 */
static void task_main(void * argument_p)
{
    TickType_t period = pdMS_TO_TICKS(STRESS_TASK_MS);
    uint8_t req = 0U;

    (void)argument_p;

    /*At a low tick rate the period may round down to zero ticks*/
    if (period == 0U) period = 1U;

    for (;;) {
        while (xQueueReceive(queue, &req, 0) == pdPASS)
            handle_req(req);

        stress_process();
        vTaskDelay(period);
    }
}

/**
 * Handles one request in the context of the test task.
 * @param req  Mode to start, or STRESS_REQ_STOP.
 */
static void handle_req(uint8_t req)
{
    if (req == STRESS_REQ_STOP)
        stress_stop();
    else
        stress_start((flash_stress_mode_t)req);
}
