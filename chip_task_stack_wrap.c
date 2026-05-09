#include <string.h>

#if defined(ESP32C5) && defined(ENABLE_MATTER)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// Matter's prebuilt CHIP stack may create the "CHIP" task with a static 4 KB
// stack. On ESP32-C5 with many endpoints this can overflow during Matter.begin().
// Intercept static task creation and use a larger dynamic stack for CHIP only.
#ifndef OS_CHIP_TASK_STACK_SIZE_BYTES
#define OS_CHIP_TASK_STACK_SIZE_BYTES 12288U
#endif

extern TaskHandle_t __real_xTaskCreateStaticPinnedToCore(
    TaskFunction_t pxTaskCode,
    const char * const pcName,
    const uint32_t ulStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    StackType_t * const pxStackBuffer,
    StaticTask_t * const pxTaskBuffer,
    const BaseType_t xCoreID);

extern BaseType_t __real_xTaskCreatePinnedToCore(
    TaskFunction_t pxTaskCode,
    const char * const pcName,
    const uint32_t usStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t * const pxCreatedTask,
    const BaseType_t xCoreID);

static uint32_t chip_stack_depth(uint32_t requested)
{
    return (requested < OS_CHIP_TASK_STACK_SIZE_BYTES) ? OS_CHIP_TASK_STACK_SIZE_BYTES : requested;
}

static int is_chip_task_name(const char *name)
{
    return (name != NULL && strcmp(name, "CHIP") == 0);
}

static void trace_chip_wrap(const char *api_name, uint32_t requested, uint32_t applied)
{
    static int traced = 0;
    if (!traced) {
        traced = 1;
        esp_rom_printf("[CHIP WRAP] %s requested=%u applied=%u\n", api_name, requested, applied);
    }
}

BaseType_t __wrap_xTaskCreatePinnedToCore(
    TaskFunction_t pxTaskCode,
    const char * const pcName,
    const uint32_t usStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t * const pxCreatedTask,
    const BaseType_t xCoreID)
{
    if (is_chip_task_name(pcName)) {
        uint32_t stack_depth = chip_stack_depth(usStackDepth);
        trace_chip_wrap("xTaskCreatePinnedToCore", usStackDepth, stack_depth);
        return __real_xTaskCreatePinnedToCore(
            pxTaskCode, pcName, stack_depth, pvParameters, uxPriority, pxCreatedTask, xCoreID);
    }

    return __real_xTaskCreatePinnedToCore(
        pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, xCoreID);
}

TaskHandle_t __wrap_xTaskCreateStaticPinnedToCore(
    TaskFunction_t pxTaskCode,
    const char * const pcName,
    const uint32_t ulStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    StackType_t * const pxStackBuffer,
    StaticTask_t * const pxTaskBuffer,
    const BaseType_t xCoreID)
{
    if (is_chip_task_name(pcName)) {
        TaskHandle_t handle = NULL;
        uint32_t stack_depth = chip_stack_depth(ulStackDepth);
        trace_chip_wrap("xTaskCreateStaticPinnedToCore", ulStackDepth, stack_depth);

        BaseType_t result = __real_xTaskCreatePinnedToCore(
            pxTaskCode,
            pcName,
            stack_depth,
            pvParameters,
            uxPriority,
            &handle,
            xCoreID);

        if (result == pdPASS) {
            return handle;
        }
    }

    return __real_xTaskCreateStaticPinnedToCore(
        pxTaskCode,
        pcName,
        ulStackDepth,
        pvParameters,
        uxPriority,
        pxStackBuffer,
        pxTaskBuffer,
        xCoreID);
}

#endif