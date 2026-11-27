/**
 * @file mode_sample.c
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

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static bool launch(flash_stress_mode_t mode);

/**********************
 *  STATIC VARIABLES
 **********************/
/*Mode of the test that runs, or that ran last*/
static flash_stress_mode_t cur_mode = FLASH_STRESS_MODE_ROTATE;

/**Mode started once the running test has ended, a switch waits for its clean
 * end*/
static flash_stress_mode_t next_mode = FLASH_STRESS_MODE_ROTATE;
static bool pending = false;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
/**
 * Starts a test in a mode, or switches to that mode while a test runs.
 * The running test is stopped first, ROTATE and ENDURANCE end their loop, READ
 * its
 * sector, then stress_on_end() starts the new mode.
 * @param mode  Kind of test to run.
 * @return      true if the test was started or the switch is pending; otherwise
 *              false.
 */
bool stress_start(flash_stress_mode_t mode)
{
    if (!stress_running()) return launch(mode);

    next_mode = mode;
    pending = true;
    flash_stress_stop(stress_object());

    return true;
}

/**
 * Stops the running test, ROTATE also erases its area back to blank before it
 * ends.
 */
void stress_stop(void)
{
    pending = false;
    flash_stress_stop(stress_object());
}

/**
 * Example of a menu or key handler of the application.
 * @param item  Selected item, 0..2 start a mode, any other value stops the
 *              test.
 */
void stress_select(uint32_t item)
{
    switch (item) {
        case 0U:
            stress_start(FLASH_STRESS_MODE_ROTATE);
            break;
        case 1U:
            stress_start(FLASH_STRESS_MODE_READ);
            break;
        case 2U:
            stress_start(FLASH_STRESS_MODE_ENDURANCE);
            break;
        default:
            stress_stop();
            break;
    }
}

/**
 * Handles the end of a test, called once by stress_process() of the run sample.
 * Logs the result, then starts the mode that waited for the switch.
 */
void stress_on_end(void)
{
    stress_report();
    if (!pending) return;

    pending = false;
    launch(next_mode);
}

/**
 * Returns the mode of the test that runs, or that ran last.
 * @return  Kind of test.
 */
flash_stress_mode_t stress_mode(void)
{
    return cur_mode;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
/**
 * Starts a test when no test runs and remembers its mode.
 * @param mode  Kind of test to run.
 * @return      true if the test was started; otherwise false.
 */
static bool launch(flash_stress_mode_t mode)
{
    if (!stress_begin(mode)) return false;

    cur_mode = mode;
    return true;
}
