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

/*Include the declarations of the stress_app_* functions of the application here*/

/*********************
 *      DEFINES
 *********************/

/*Period of the test task, a step returns at once while the flash is busy*/
#define STRESS_APP_TASK_MS 2U

/*Requests to the test task, a mode is sent as its flash_stress_mode_t value*/
#define STRESS_APP_REQ_STOP 0xFFU
#define STRESS_APP_QUEUE_LEN 4U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void stress_app_task(void * argument_p);
static void stress_app_handle(uint8_t req);

/**********************
 *  STATIC VARIABLES
 **********************/

/*Requests of the other tasks, only the test task calls the framework*/
static QueueHandle_t stress_app_queue = NULL;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Creates the request queue and the test task, once after the flash driver is ready.
 * The task needs INCLUDE_vTaskDelay = 1 in FreeRTOSConfig.h.
 * @param stack Stack depth of the task, in words of the port.
 * @param priority Priority of the task, below the tasks that must not be delayed.
 * @return true if the queue and the task were created; otherwise false.
 */
bool stress_app_task_create(configSTACK_DEPTH_TYPE stack, UBaseType_t priority)
{
    BaseType_t res = pdFAIL;

    stress_app_queue = xQueueCreate(STRESS_APP_QUEUE_LEN, sizeof(uint8_t));
    if (stress_app_queue == NULL) return false;

    res = xTaskCreate(stress_app_task, "stress", stack, NULL, priority, NULL);
    return res == pdPASS;
}

/**
 * Sends a request from another task, for example from the menu of the GUI task.
 * @param req Mode to start, or STRESS_APP_REQ_STOP to stop the test.
 * @return true if the request was queued; otherwise false.
 */
bool stress_app_request(uint8_t req)
{
    BaseType_t res = pdFAIL;

    if (stress_app_queue == NULL) return false;

    res = xQueueSend(stress_app_queue, &req, 0);
    return res == pdPASS;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Owns the test, only this task calls the framework and accesses the test area.
 * @param argument_p Unused task argument.
 */
static void stress_app_task(void * argument_p)
{
    TickType_t period = pdMS_TO_TICKS(STRESS_APP_TASK_MS);
    uint8_t req = 0U;

    (void)argument_p;

    /*At a low tick rate the period may round down to zero ticks*/
    if (period == 0U) period = 1U;

    for (;;) {
        while (xQueueReceive(stress_app_queue, &req, 0) == pdPASS)
            stress_app_handle(req);

        stress_app_process();
        vTaskDelay(period);
    }
}

/**
 * Handles one request in the context of the test task.
 * @param req Mode to start, or STRESS_APP_REQ_STOP.
 */
static void stress_app_handle(uint8_t req)
{
    if (req == STRESS_APP_REQ_STOP)
        stress_app_stop();
    else
        stress_app_start((flash_stress_mode_t)req);
}
