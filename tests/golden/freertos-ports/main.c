/* freertos-cm3's program on the MPS2 boards: two tasks and a queue, a
 * mutex, a software timer and time slicing, through the port's naked
 * handlers. With BOARD_FPU, two more tasks add up floats at the
 * spinner's priority, yielding now and then and preempted by every time
 * slice in between: the port's PendSV must save and restore the FPU
 * state of a task that used it (s16-s31 by the handler, s0-s15 and FPSCR
 * by the lazy exception frame), or a sum comes out wrong. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

/* The CMSDK UART: CTRL's bit 0 enables the transmitter, off at reset. */
#define UART0_DR   (*(volatile unsigned *)BOARD_UART)
#define UART0_CTRL (*(volatile unsigned *)(BOARD_UART + 8))

static void puts_(const char *s)
{
    UART0_CTRL = 1u;
    while (*s)
        UART0_DR = (unsigned)*s++;
}

static void putn(unsigned long v)
{
    char b[12];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n)
        UART0_DR = (unsigned)b[--n];
}

void vAssertCalled(const char *file, int line)
{
    taskDISABLE_INTERRUPTS();
    puts_("ASSERT ");
    puts_(file);
    puts_(":");
    putn((unsigned long)line);
    puts_("\n");
    for (;;)
        ;
}

void vApplicationMallocFailedHook(void)
{
    puts_("MALLOC FAILED\n");
    for (;;)
        ;
}

void vApplicationStackOverflowHook(TaskHandle_t t, char *name)
{
    (void)t;
    puts_("STACK OVERFLOW ");
    puts_(name);
    puts_("\n");
    for (;;)
        ;
}

static QueueHandle_t q;
static SemaphoreHandle_t lock;
static volatile int timer_fired;
#if BOARD_FPU
static volatile int fpu_ok[2];
#endif

static void say(const char *s, unsigned long n)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    puts_(s);
    putn(n);
    puts_("\n");
    xSemaphoreGive(lock);
}

static void producer(void *arg)
{
    unsigned long base = (unsigned long)arg;
    for (unsigned long i = 1; i <= 5; i++) {
        xQueueSend(q, &i, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(base));
    }
    vTaskDelete(NULL);
}

static void consumer(void *arg)
{
    (void)arg;
    unsigned long v, sum = 0;
    TickType_t t0 = xTaskGetTickCount();
    for (int k = 0; k < 5; k++) {
        xQueueReceive(q, &v, portMAX_DELAY);
        sum += v;
        say("got ", v);
    }
    TickType_t dt = xTaskGetTickCount() - t0;
    say("sum ", sum);
    /* four delays of 20 ms between five sends: at least 80 ticks */
    say("waited enough ", dt >= 80);
    say("timer ", (unsigned long)timer_fired);
#if BOARD_FPU
    say("fpu ", fpu_ok[0] == 1 && fpu_ok[1] == 1);
#endif
    puts_("done\n");
    for (;;)
        vTaskDelay(1000);
}

static void spinner(void *arg)
{
    /* a task that never blocks, at the consumer's priority: time
     * slicing has to preempt it for the others to run at all */
    (void)arg;
    volatile unsigned long n = 0;
    for (;;)
        n++;
}

#if BOARD_FPU
/* Sums of i * k for i = 1..2000 are exact in float -- every partial sum
 * is a multiple of k, and the largest, 1000500 = 2001000 * 0.5, needs 21
 * bits -- so any difference is a register the switch lost. */
/* A call, so that the sums live in s16-s31 across it (the callee-saved
 * half, which only the port's PendSV saves -- the core stacks s0-s15),
 * and the switch it asks for happens inside it. */
__attribute__((noinline)) static void yield_now(void)
{
    taskYIELD();
}

static void floater(void *arg)
{
    int id = (int)(long)arg;
    float k = id ? 0.25f : 0.5f;
    int ok = 1;
    for (int round = 0; round < 3; round++) {
        float acc = 0.0f;
        for (int i = 1; i <= 2000; i++) {
            acc += (float)i * k;
            if (i % 500 == 0)
                yield_now();
        }
        if (acc != (id ? 500250.0f : 1000500.0f))
            ok = 0;
    }
    fpu_ok[id] = ok ? 1 : -1;
    vTaskDelete(NULL);
}
#endif

static void tick_timer(TimerHandle_t t)
{
    (void)t;
    timer_fired = 1;
}

int main(void)
{
    puts_("FreeRTOS " tskKERNEL_VERSION_NUMBER " on EmbCC\n");
    q = xQueueCreate(4, sizeof(unsigned long));
    lock = xSemaphoreCreateMutex();
    TimerHandle_t tm = xTimerCreate("t", pdMS_TO_TICKS(30), pdFALSE, NULL,
                                    tick_timer);
    configASSERT(q && lock && tm);
    xTimerStart(tm, 0);
    xTaskCreate(producer, "prod", 256, (void *)20, 2, NULL);
    xTaskCreate(consumer, "cons", 256, NULL, 1, NULL);
    xTaskCreate(spinner, "spin", 128, NULL, 1, NULL);
#if BOARD_FPU
    xTaskCreate(floater, "f0", 256, (void *)0, 1, NULL);
    xTaskCreate(floater, "f1", 256, (void *)1, 1, NULL);
#endif
    vTaskStartScheduler();
    puts_("scheduler returned\n");
    for (;;)
        ;
}
