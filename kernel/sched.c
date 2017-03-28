#include <levos/kernel.h>
#include <levos/types.h>
#include <levos/task.h>
#include <levos/x86.h> /* FIXME */
#include <levos/intr.h>
#include <levos/arch.h>
#include <levos/palloc.h>
#include <levos/page.h>

#define TIME_SLICE 5

static pid_t last_pid = 0;
struct task *current_task;

static struct task *all_tasks[128];
static int last_task;

void __noreturn late_init(void);
void __noreturn __idle_thread(void);

void intr_yield(struct pt_regs *);
void sched_yield(void);
void reschedule(void);

inline int
task_is_kernel(struct task *t)
{
    return t->mm == 0;
}

pid_t
allocate_pid(void)
{
    return ++last_pid;
}

void __noreturn
sched_init(void)
{
    uint32_t kernel_stack, new_stack;

    printk("sched: init\n");
    current_task = malloc(sizeof(*current_task));
    if (!current_task)
        panic("Kernel ran out of memory when starting threading\n");

    current_task->mm = 0;
    current_task->pid = 0;
    current_task->state = TASK_RUNNING;
    current_task->time_ran = 0;

    memset(all_tasks, 0, sizeof(struct task *) * 128);
    all_tasks[0] = current_task;
    last_task = 0;

    intr_register_user(0x2f, intr_yield);

    /* map a new stack */
    new_stack = (uint32_t) malloc(4096);
    if (!new_stack)
        panic("ENOMEM for new kernel stack\n");

    new_stack = (int)new_stack + 4096;

    /* switch stack */
    asm volatile ("movl %%esp, %0; movl %1, %%esp; movl %1, %%ebp"
            :"=r"(kernel_stack)
            :"r"(new_stack));

//    printk("sched: kernel stack was: 0x%x\n", kernel_stack);

    /* start executing the process */
    __idle_thread();

    /* this is weird */
    panic("tasking screwed\n");
}

struct task *create_kernel_task(void (*func)(void))
{
    uint32_t *new_stack, tmp;
    struct task *task = malloc(sizeof(*task));

    if (!task)
        return NULL;

    task->pid = allocate_pid();
    task->state = TASK_PREEMPTED;
    task->time_ran = 0;
    task->parent = NULL;
    task->exit_code = 0;
    task->mm = kernel_pgd;
    
    /* get a stack for the task, since this is a kernel thread
     * we can use malloc
     */
    new_stack = malloc(4096);
    if (!new_stack) {
        free(task);
        return NULL;
    }
    memset(new_stack, 0, 4096);

    new_stack = (uint32_t *)((int)new_stack + 4096);
    task->irq_stack_top = (uint32_t *) (((uint8_t *) malloc(0x1000)) + 0x1000);
    task->irq_stack_bot = ((uint32_t)task->irq_stack_top) - 0x1000;

    /* setup the stack */
    new_stack -= 16;
    *--new_stack = 0x10; /* ss */
    *--new_stack = 0; /* esp */
    *--new_stack = 0x202; /* eflags */
    *--new_stack = 0x08; /* cs  */
    *--new_stack = (uint32_t) func; /* eip */
    *--new_stack = 0x00; /* framepointer */
    *--new_stack = 0x00; /* error_code */
    *--new_stack = 0x00; /* vec_no */
    *--new_stack = 0x10; /* ds  */
    *--new_stack = 0x10; /* es  */
    *--new_stack = 0x10; /* fs  */
    *--new_stack = 0x10; /* gs  */
    /* pushad */
    tmp = (uint32_t) new_stack;
    *--new_stack = 0;    /* eax */
    *--new_stack = 0;    /* ecx */
    *--new_stack = 0;    /* edx */
    *--new_stack = 0;    /* ebx */
    *--new_stack = tmp;  /* esp_dummy */
    *--new_stack = 0;    /* ebp */
    *--new_stack = 0;    /* esi */
    *--new_stack = 0;    /* edi */

    task->regs = (void *) new_stack;
    task->new_stack = new_stack;
    return task;
}

void
task_block(struct task *task)
{
    task->state = TASK_BLOCKED;
    if (task == current_task)
        sched_yield();
}

void
task_unblock(struct task *task)
{
    task->state = TASK_PREEMPTED;
}

void
setup_filetable(struct task *task)
{
    /* for stdin, we currently just pass through to the serial,
     * but in the future we really should implement ttys and
     * give the init process a different stdin
     */

    extern struct file serial_base_file;

    /* stdin */
    task->file_table[0] = dup_file(&serial_base_file);
    /* stdout */
    task->file_table[1] = dup_file(&serial_base_file);
    /* stderr */
    task->file_table[2] = NULL;
}

void
task_exit(struct task *t)
{
    /* TODO: preliminary cleanup, but don't get rid of thread */
    t->state = TASK_ZOMBIE;
    if (t->pid == 1)
        panic("Attempting to exit from init\n");
    if (t->pid == 0)
        panic("Kernel bug: Attempting to kill idle/swapper\n");
}

void
send_signal(struct task *task, int signal)
{
    printk("task pid=%d caught signal %s\n", task->pid, signal_to_string(signal));
    /* for now, just kill */
    task_exit(task);
    if (task == current_task)
        sched_yield();
    return;
}

struct task *create_user_task_withmm(pagedir_t mm, void (*func)(void))
{
    uint32_t *new_stack, p_new_stack, tmp;
    struct task *task = malloc(sizeof(*task));

    if (!task)
        return NULL;

    task->pid = allocate_pid();
    task->state = TASK_PREEMPTED;
    task->time_ran = 0;
    task->parent = NULL;
    task->exit_code = 0;
    task->mm = mm;
    
    /* get a stack for the task, since this is a kernel thread
     * we can use malloc
     */
    p_new_stack = palloc_get_page();
    if (!p_new_stack) {
        free(task);
        return NULL;
    }

    task->irq_stack_top = (uint32_t *) (((uint8_t *) malloc(0x1000)) + 0x1000);
    task->irq_stack_bot = ((uint32_t)task->irq_stack_top) - 0x1000;

    map_page(task->mm, p_new_stack, VIRT_BASE - 4096, 1);
    map_page_curr(p_new_stack, 0xD0000000 - 4096, 1);
    new_stack = (uint32_t *) (0xD0000000 - 4096);
    memset(new_stack, 0, 4096);

    new_stack = (uint32_t *)((int)new_stack + 4096);

    /* setup the stack */
    tmp = (uint32_t) new_stack;
    *--new_stack = 0x23; /* ss */
    *--new_stack = tmp; /* esp */
    *--new_stack = 0x202; /* eflags */
    *--new_stack = 0x1B; /* cs  */
    *--new_stack = (uint32_t) func; /* eip */
    *--new_stack = 0x00; /* framepointer */
    *--new_stack = 0x00; /* error_code */
    *--new_stack = 0x00; /* vec_no */
    *--new_stack = 0x23; /* ds  */
    *--new_stack = 0x23; /* es  */
    *--new_stack = 0x23; /* fs  */
    *--new_stack = 0x23; /* gs  */
    /* pushad */
    tmp = (uint32_t) new_stack;
    *--new_stack = 0;    /* eax */
    *--new_stack = 0;    /* ecx */
    *--new_stack = 0;    /* edx */
    *--new_stack = 0;    /* ebx */
    *--new_stack = tmp;  /* esp_dummy */
    *--new_stack = tmp;    /* ebp */
    *--new_stack = 0;    /* esi */
    *--new_stack = 0;    /* edi */

    task->regs = (void *) (VIRT_BASE - sizeof(struct pt_regs));
    ((struct pt_regs *) new_stack)->esp = task->regs;

    task->new_stack = (struct pt_regs *) new_stack;

    setup_filetable(task);
    return task;
}

struct task *
create_user_task(void (*func)(void))
{
    pagedir_t mm = new_page_directory();
    return create_user_task_withmm(mm, func);
}

struct task *
create_user_task_fork(void (*func)(void))
{
    pagedir_t mm = copy_page_dir(current_task->mm);
    return create_user_task_withmm(mm, func);
}

void
kernel_task_exit()
{
    current_task->state = TASK_ZOMBIE;
    sched_yield();
}

void
sched_add_rq(struct task *task)
{
    //printk("%s: task->pid: %d task->regs: 0x%x\n", __func__, task->pid, task->regs);
    for (int i = 0; i < 128; i ++) {
        if (all_tasks[i] == 0) {
            all_tasks[i] = task;
            return;
        }
    }
    panic("RQ is full\n");
}

extern void init_task(void);
void __noreturn
__idle_thread(void)
{
    struct task *n;
    arch_switch_timer_sched();

    /* simulate a timer interrupt to get some values to regs */
    asm volatile("mov $0xC0FFEEEE, %%eax; int $32":::"eax");
    panic_on(current_task->regs->eax != 0xC0FFEEEE, "pt_regs is not stable");

    /* start another task */

    n = create_kernel_task(init_task);
    panic_on(n == NULL, "failed to create init task\n");
    sched_add_rq(n);

    late_init();

    __not_reached();
}

struct task *
pick_next_task(void)
{
    //printk("%s\n", __func__);
retry:
    for (int i = last_task; i < 128; i ++) {
        if (all_tasks[i] != 0) { 
            if (all_tasks[i]->state == TASK_DYING) {
                task_exit(all_tasks[i]);
                continue;
            } else if (!task_runnable(all_tasks[i]))
                continue;
            last_task = i + 1;
            //if (all_tasks[i]->pid == 2)
                //printk("pid:%d\n", all_tasks[i]->pid);
            return all_tasks[i];
        }
    }

    if (last_task == 0)
        panic("No task to run\n");
    last_task = 0;
    goto retry;
}

void
intr_yield(struct pt_regs *r)
{
    current_task->regs = r;
    reschedule();
}

void
sched_yield()
{
    asm volatile("int $0x2F");
}

void
reschedule(void)
{
    if (current_task->state == TASK_RUNNING)
        current_task->state = TASK_PREEMPTED;
    struct task *next = pick_next_task();
    next->time_ran = 0;
    next->state = TASK_RUNNING;
    current_task = next;

    if (current_task->mm)
        activate_pgd(current_task->mm);
    //else activate_pgd(kernel_pgd);
    
    tss_update(current_task);

    /* switch stack */
    asm volatile("movl %0, %%esp;"
                 "movw $0x20, %%dx;"
                 "movb $0x20, %%al;"
                 "outb %%al, %%dx;"
                 "jmp intr_exit"::"r"(next->regs));
    __not_reached();
}

void
sched_tick(struct pt_regs *r)
{
    current_task->time_ran ++;
    current_task->regs = r;
    //printk("TICK\n");
    if (current_task->time_ran > TIME_SLICE)
        reschedule();
}
