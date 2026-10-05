/* FreeRTOS on the MPS2 boards QEMU models: BOARD_PRIO_BITS priority bits
 * and a BOARD_CLOCK_HZ core clock, set by freertos-ports.sh. */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configCPU_CLOCK_HZ                      BOARD_CLOCK_HZ
#define configTICK_RATE_HZ                      1000
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configMAX_PRIORITIES                    5
#define configMINIMAL_STACK_SIZE                128
#define configTOTAL_HEAP_SIZE                   (16 * 1024)
#define configMAX_TASK_NAME_LEN                 8
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               3
#define configTIMER_QUEUE_LENGTH                4
#define configTIMER_TASK_STACK_DEPTH            128
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configENABLE_BACKWARD_COMPATIBILITY     0

#define configPRIO_BITS                         BOARD_PRIO_BITS
#define configKERNEL_INTERRUPT_PRIORITY         (7 << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (5 << (8 - configPRIO_BITS))

#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_uxTaskGetStackHighWaterMark     1

/* The ARMv8-M ports' switches: the Cortex-M33 runs the kernel in the
 * secure state, without TrustZone or the MPU, with its FPU. */
#if BOARD_V8M
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configENABLE_MVE                        0
#endif

/* RISC-V: the CLINT's timer, which counts at BOARD_CLOCK_HZ (10 MHz on
 * QEMU's virt), and a stack of the port's own for interrupts. */
#if BOARD_RISCV
#define configMTIME_BASE_ADDRESS                0x0200BFF8UL
#define configMTIMECMP_BASE_ADDRESS             0x02004000UL
#define configISR_STACK_SIZE_WORDS              256
#endif

void vAssertCalled(const char *file, int line);
#define configASSERT(x) do { if (!(x)) vAssertCalled(__FILE__, __LINE__); } while (0)

#endif
