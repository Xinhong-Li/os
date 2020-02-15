#include <inc/elf.h>
#include <arch/i386/inc.h>
#include <traps.h>

// Large Prime Number: https://planetmath.org/goodhashtableprimes
#define HASH(x)         (((uint32_t)x) % PROC_BUCKET_SIZE)
#define PROC_EXISTS(p) (list_find(&ptable.hlist[HASH(p)], &(p)->hlist) && (p)->magic == PROC_MAGIC)

struct context {
	uint32_t edi;
	uint32_t esi;
	uint32_t ebx;
	uint32_t ebp;
	uint32_t eip;
} __attribute__((packed));

struct ptable ptable;

void trapret(); // In trapasm.S
void swtch(struct context **old, struct context *new);// In swtch.S

struct proc *
thisproc()
{
    return thiscpu()->proc;
}

void 
proc_init()
{
    for (int i = 0; i < PROC_BUCKET_SIZE; i ++)
        list_init(&ptable.hlist[i]);
    list_init(&ptable.ready_list);
    list_init(&ptable.zombie_list);
}

void
forkret()
{
    cprintf("forkret\n");
    spinlock_release(&ptable.lock);
}

//  Initial Kerenl 
//  Stack Layout     
//
//  +------------+  top
//  |  proc      |
//  +------------+
//  | trap frame |
//  +------------+
//  | forkret    |
//  +------------+
//  | context    |
//  +------------+
//  |  ...       |
//  +------------+   bottom
struct proc *
proc_alloc()
{
    struct hackframe {
        struct context context;
        void *retaddr;
        struct trapframe tf;
        struct proc p;
    } __attribute__((packed)) *hf;

    hf = (void *)kalloc(KSTKSIZE) + KSTKSIZE - sizeof(*hf);

    struct trapframe *tf = &hf->tf;
    tf->ds = SEG_SELECTOR(SEG_UDATA, TI_GDT, DPL_USER);
    tf->cs = SEG_SELECTOR(SEG_UCODE, TI_GDT, DPL_USER);
    tf->es = tf->ss = tf->fs = tf->gs = tf->ds;
    tf->esp = USTACKTOP;
    tf->eflags = FL_IF;
    tf->eip = 0;// will be initialized by ucode_load
    tf->err = 0;

    hf->retaddr = trapret;
    hf->context.eip = (uint32_t) forkret;

    struct proc *p = &hf->p;
    p->context = &hf->context; // stack pointer
    p->tf = tf;
    p->magic = PROC_MAGIC;
    //p->pgdir = vm_fork(entry_pgdir);

    // insert to hash table
    list_push_back(&ptable.hlist[HASH(p)], &p->hlist);
    list_init(&p->wait_list);

    cprintf("proc_alloc: 0x%x, hash: %d\n", p, HASH(p));
    return p;
}

static void
proc_free(struct proc *p)
{
    assert(PROC_EXISTS(p));
    list_drop(&p->hlist);
    vm_free(p->pgdir);
    kfree(p + sizeof(struct proc) - KSTKSIZE);

    assert(!list_find(&ptable.hlist[HASH(p)], &p->hlist));
    assert(!list_find(&ptable.ready_list, &p->hlist));
    assert(!list_find(&ptable.zombie_list, &p->hlist));
    assert(list_empty(&p->wait_list));
}

static void
ucode_load(struct proc *p, uint8_t *binary) 
{
    struct elfhdr *elf = (struct elfhdr*)binary;

    if(elf->magic != ELF_MAGIC)
        panic("Not an ELF.");

    struct proghdr *ph = (struct proghdr*)((uint8_t*)elf + elf->phoff);
    struct proghdr *eph = ph + elf->phnum;


    vm_switch(p->pgdir);
    // Load each program segment (ignores ph flags).
    for(; ph < eph; ph++) {
        if (ph->type != ELF_PROG_LOAD)
            continue;

        assert(ph->va + ph->memsz > ph->va);
        assert(ph->va + ph->memsz <= KERNBASE);

        vm_alloc(p->pgdir, ph->va, ph->memsz);

        //copy to proc's virtual memory
        memmove((void *)ph->va, binary + ph->offset, ph->filesz);
        //BSS initialization
        if(ph->memsz > ph->filesz) 
            memset((void *)ph->va + ph->filesz, 0, ph->memsz - ph->filesz);
    }

    vm_switch(entry_pgdir);

    // prepare the entry point
    p->tf->eip = elf->entry;

	// Now map one page for the program's initial stack
	// at virtual address USTACKTOP - PGSIZE.
    vm_alloc(p->pgdir, USTACKTOP - PGSIZE, PGSIZE);

    cprintf("finish ucode loading.\n");
}

// The first process
void
user_init()
{
    spinlock_acquire(&ptable.lock);
    struct proc *p = proc_alloc();
    p->pgdir = vm_fork(entry_pgdir);

    extern uint8_t _binary_obj_user_test_elf_start[];
    ucode_load(p, _binary_obj_user_test_elf_start);

    list_push_back(&ptable.ready_list, &p->pos);

    spinlock_release(&ptable.lock);
    //proc_stat();
    cprintf("user init finished.\n");
}

void
scheduler() {
    while(1) {
        spinlock_acquire(&ptable.lock);

        if (!list_empty(&ptable.ready_list)) {

            struct list_head *i = list_front(&ptable.ready_list);
            list_drop(i);
            struct proc *p = CONTAINER_OF(i, struct proc, pos);
            assert(PROC_EXISTS(p));

            uvm_switch(p);
            thiscpu()->proc = p;

            cprintf("cpuidx %d get proc %x\n", cpuidx(), p);
            swtch(&thiscpu()->scheduler, p->context);
            cprintf("cpuidx %d in scheduler\n", cpuidx());

            thiscpu()->proc = 0;
            vm_switch(entry_pgdir);
        }

        spinlock_release(&ptable.lock);
    }
}

// Context switch from thisproc to scheduler.
// Caller should hold ptable.lock
void
sched() 
{
    swtch(&thisproc()->context, thiscpu()->scheduler);
}
 
int
fork()
{
    spinlock_acquire(&ptable.lock);
    struct proc *tp = thisproc();
    struct proc *p = proc_alloc();

    p->pgdir = vm_fork(tp->pgdir);
    *p->tf = *tp->tf;
    p->tf->eax = 0; // fork return 0 in child
    //test_pgdir(p->pgdir);

    list_push_back(&ptable.ready_list, &p->pos);
    spinlock_release(&ptable.lock);
    return (int)p;
}

// Sleep and wait for process p
// Reap all ZOMBIE process first
// Caller should hold ptable.lock
// Return 0 if success else -1
int 
wait(struct proc *p)
{
    while(!list_empty(&ptable.zombie_list)) {
        struct proc *zp = CONTAINER_OF(list_front(&ptable.zombie_list), struct proc, pos);
        list_drop(&zp->pos);
        proc_free(zp);
    }

    if (PROC_EXISTS(p)) {
        list_push_back(&p->wait_list, &thisproc()->pos);
        sched();
        return 0;
    }
    cprintf("wait: proc not exists or zombie\n");
    return -1;
}

// Serve and return the first process in waiting list
// Caller should hold ptable.lock
struct proc *
serve()
{
    struct proc *tp = thisproc();
    while(list_empty(&tp->wait_list)) {
        list_push_back(&ptable.ready_list, &tp->pos);
        sched();
    }
    struct proc *p = CONTAINER_OF(list_front(&tp->wait_list), struct proc, pos);
    assert(PROC_EXISTS(p));
    list_drop(&p->pos);
    list_push_back(&ptable.ready_list, &p->pos);
    return p;
}

void
exit() 
{
    struct proc *p = thisproc();

    spinlock_acquire(&ptable.lock);
    assert(PROC_EXISTS(p));

    // Remove waiters
    while(!list_empty(&p->wait_list)) 
        list_drop(list_front(&p->wait_list));

    list_push_back(&ptable.zombie_list, &p->pos);

    cprintf("exit: proc 0x%x exit.\n", p);
    proc_stat();
    sched();
    panic("exit: return\n");
}

void
proc_stat()
{
    struct proc *p;
    cprintf("ready_list: ");
    LIST_FOREACH_ENTRY(p, &ptable.ready_list, pos) {
        assert(PROC_EXISTS(p));
        cprintf("0x%x", p);
        if (!list_empty(&p->wait_list)) {
            cprintf("(");
            struct proc *wp;
            LIST_FOREACH_ENTRY(wp, &p->wait_list, pos) {
                cprintf("0x%x, ", p);
            }
            cprintf(")");
        }
        cprintf(", ");
    }
    cprintf("\n");
    cprintf("zombie_list: ");
    LIST_FOREACH_ENTRY(p, &ptable.zombie_list, pos) {
        assert(PROC_EXISTS(p));
        assert(list_empty(&p->wait_list));
        cprintf("0x%x, ", p);
    }
    cprintf("\n");
}
