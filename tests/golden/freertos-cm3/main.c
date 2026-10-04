/* Two tasks and a queue under FreeRTOS: the producer sends 1..5, the
 * consumer prints each with the tick it arrived at (rounded), and a
 * software timer and a mutex take part. Everything the kernel does --
 * the first task started by SVC, every switch by PendSV, the delays by
 * SysTick -- goes through the port's naked handlers. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

#define UART0_DR (*(volatile unsigned *)0x4000C000u)

static void puts_(const char *s)
{
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
    vTaskStartScheduler();
    puts_("scheduler returned\n");
    for (;;)
        ;
}
