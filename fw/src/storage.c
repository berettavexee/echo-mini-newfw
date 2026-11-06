/* The storage task: see storage.h. A coroutine: its own stack, and a switch of context that saves
 * the callee-saved registers (r4-r11), the stack pointer and the return address; no scheduler,
 * no interrupt: the main loop resumes it, it comes back when its budget is spent or its jobs are
 * done. An interrupt taken while it runs uses its stack: the size leaves room for one. */
#include "storage.h"
#include "board.h"
#include "log.h"

#define STACK_WORDS 1536                        /* 6 KiB (the reader's depth measured: §100) */
#define QUEUE       4
#define PAINT       0xa5a5a5a5u

static uint32_t stack[STACK_WORDS] __attribute__((aligned(8)));
static uint32_t *main_sp, *task_sp;
static struct { storage_job fn; void *arg; } queue[QUEUE];
static int q_head, q_count, running, started, guard_said, held;
static uint32_t deadline;

/* Saves r4-r11 and the return address on the current stack, its pointer in *save, then takes
 * 'to' and returns where that one left off (or, the first time, into task_entry) */
__attribute__((naked, noinline)) static void co_swap(uint32_t **save, uint32_t *to)
{
    __asm volatile(
        "push {r4-r11, lr}\n"
        "mov r2, sp\n"
        "str r2, [r0]\n"
        "mov sp, r1\n"
        "pop {r4-r11, pc}\n");
    (void)save;
    (void)to;
}

static void task_entry(void)
{
    for (;;) {
        while (q_count) {
            storage_job fn = queue[q_head].fn;
            void *arg = queue[q_head].arg;
            fn(arg);
            q_head = (q_head + 1) % QUEUE;
            q_count--;
        }
        co_swap(&task_sp, main_sp);             /* nothing left: back to the loop */
    }
}

static void start(void)
{
    for (int i = 0; i < STACK_WORDS; i++)
        stack[i] = PAINT;
    uint32_t *sp = stack + STACK_WORDS - 9;     /* r4-r11 then the entry, popped by co_swap */
    for (int i = 0; i < 8; i++)
        sp[i] = 0;
    sp[8] = (uint32_t)task_entry | 1;           /* Thumb */
    task_sp = sp;
    started = 1;
}

int storage_submit(storage_job fn, void *arg)
{
    if (!started)
        start();
    if (q_count == QUEUE)
        return -1;
    int i = (q_head + q_count) % QUEUE;
    queue[i].fn = fn;
    queue[i].arg = arg;
    q_count++;
    return 0;
}

int storage_busy(void)
{
    return q_count != 0;
}

int storage_in_task(void)
{
    return running;
}

static void resume(uint32_t budget_us)
{
    deadline = ticks() + budget_us;
    running = 1;
    co_swap(&main_sp, task_sp);
    running = 0;
    if (stack[0] != PAINT && !guard_said) {     /* the bottom word reached: the stack is too */
        guard_said = 1;                         /* small (said once) */
        log_printf("storage: the task's stack reached its end (%u bytes)", (unsigned)sizeof stack);
    }
}

void storage_run(uint32_t budget_us)
{
    if (q_count && !running)
        resume(budget_us);
}

void storage_drain(void)
{
    while (q_count && !running)
        resume(0xffffffffu / 2);                /* no budget: each job to its end */
}

void storage_hold(int on)
{
    held = on;
}

void storage_yield(void)
{
    wdt_kick();                                 /* a job drained by the loop: it goes on */
    if (running && !held && (int32_t)(ticks() - deadline) >= 0)
        co_swap(&task_sp, main_sp);
}

uint32_t storage_stack_free(void)
{
    uint32_t n = 0;
    while (n < STACK_WORDS && stack[n] == PAINT)
        n++;
    return n * 4;
}
