#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"
#include <atomic>

// kernel.cc
//
//    This is the kernel.


// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000       // initial state only

proc ptable[PID_MAX];           // array of process descriptors
                                // Note that `ptable[0]` is never used.
proc* current;                  // pointer to currently executing proc

#define HZ 100                  // timer interrupt frequency (interrupts/sec)
static std::atomic<unsigned long> ticks; // # timer interrupts so far


// Memory state - see `kernel.hh`
physpageinfo physpages[NPAGES];


[[noreturn]] void schedule();
[[noreturn]] void run(proc* p);
void exception(regstate* regs);
uintptr_t syscall(regstate* regs);
void memshow();


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

static void process_setup(pid_t pid, const char* program_name);

void kernel_start(const char* command) {
    // initialize hardware
    init_hardware();
    log_printf("Starting WeensyOS\n");

    ticks = 1;
    init_timer(HZ);

    // clear screen
    console_clear();

    // (re-)initialize kernel page table
    for (uintptr_t addr = 0; addr < MEMSIZE_PHYSICAL; addr += PAGESIZE) {
        int perm = PTE_P | PTE_W | PTE_U;
        if ((addr >= KERNEL_START_ADDR) && (addr < PROC_START_ADDR) && (addr != CONSOLE_ADDR)){
            perm = PTE_P | PTE_W ;
        }
       
        if (addr == 0) {
            // nullptr is inaccessible even to the kernel
            perm = 0;
        }
        // install identity mapping
        int r = vmiter(kernel_pagetable, addr).try_map(addr, perm);
        assert(r == 0);
        // mappings during kernel_start MUST NOT fail
        // (Note that later mappings might fail!!)
    }

    // set up process descriptors
    for (pid_t i = 0; i < PID_MAX; i++) {
        ptable[i].pid = i;
        ptable[i].state = P_FREE;
    }
    if (!command) {
        command = WEENSYOS_FIRST_PROCESS;
    }
    if (!program_image(command).empty()) {
        process_setup(1, command);
    } else {
        process_setup(1, "allocator");
        process_setup(2, "allocator2");
        process_setup(3, "allocator3");
        process_setup(4, "allocator4");
    }

    // switch to first process using run()
    run(&ptable[1]);
}


// kalloc(sz)
//    Kernel physical memory allocator. Allocates at least `sz` contiguous bytes
//    and returns a pointer to the allocated memory, or `nullptr` on failure.
//    The returned pointer’s address is a valid physical address, but since the
//    WeensyOS kernel uses an identity mapping for virtual memory, it is also a
//    valid virtual address that the kernel can access or modify.
//
//    The allocator selects from physical pages that can be allocated for
//    process use (so not reserved pages or kernel data), and from physical
//    pages that are currently unused (`physpages[N].refcount == 0`).
//
//    On WeensyOS, `kalloc` is a page-based allocator: if `sz > PAGESIZE`
//    the allocation fails; if `sz < PAGESIZE` it allocates a whole page
//    anyway.
//
//    The returned memory is initially filled with 0xCC, which corresponds to
//    the `int3` instruction. Executing that instruction will cause a `PANIC:
//    Unhandled exception 3!` This may help you debug.

void* kalloc(size_t sz) {
    if (sz > PAGESIZE) {
        return nullptr;
    }

    int pageno = 0;
    int page_increment = 3;
    // In the handout code, `kalloc` returns the first free page.
    // Alternate search strategies can be faster and/or expose bugs elsewhere.
    // This initialization returns a random free page:
    //     int pageno = rand(0, NPAGES - 1);
    // This initialization remembers the most-recently-allocated page and
    // starts the search from there:
    //     static int pageno = 0;
    // In Step 3, you must change the allocation to use non-sequential pages.
    // The easiest way to do this is to set page_increment to 3, but you can
    // also set `pageno` randomly.

    for (int tries = 0; tries != NPAGES; ++tries) {
        uintptr_t pa = pageno * PAGESIZE;
        if (allocatable_physical_address(pa)
            && physpages[pageno].refcount == 0) {
            ++physpages[pageno].refcount;
            memset((void*) pa, 0xCC, PAGESIZE);
            return (void*) pa;
        }
        pageno = (pageno + page_increment) % NPAGES;
    }

    return nullptr;
}


// kfree(kptr)
//    Free `kptr`, which must have been previously returned by `kalloc`.
//    If `kptr == nullptr` does nothing.

void kfree(void* kptr) {
    if (kptr == nullptr){
        return;
    }

    // free memory at kptr
    uintptr_t pa = (uintptr_t) kptr;
    int pageno = pa/PAGESIZE;

    assert(physpages[pageno].refcount > 0);
    
    -- physpages[pageno].refcount;

    if (physpages[pageno].refcount == 0){
        memset((void*) pa, 0x00, PAGESIZE);
    }


}

void free_memory_helper (void* fail_addr, x86_64_pagetable* pagetable);
void free_ptmemory_helper ( x86_64_pagetable* pagetable);



// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* program_name) {
    init_process(&ptable[pid], 0);

    // initialize process page table
    ptable[pid].pagetable = kalloc_pagetable();

    // copy all the kernel mappings into the process page table
    for (uintptr_t addr = 0; addr < PROC_START_ADDR; addr += PAGESIZE) {
        int perm = vmiter(kernel_pagetable,addr).perm();
        uintptr_t pa = vmiter(kernel_pagetable,addr).pa();

        if (pa != (uintptr_t) -1){
            if (addr != CONSOLE_ADDR){
                perm &= ~PTE_U;
            }
            int r = vmiter(ptable[pid].pagetable, addr).try_map(pa, perm);
            assert (r==0);
 
        }
    }
    

    // obtain reference to program image
    // (The program image models the process executable.)
    program_image pgm(program_name);


    // allocate and map process memory as specified in program image
    for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
        for (uintptr_t a = round_down(seg.va(), PAGESIZE);
             a < seg.va() + seg.size();
             a += PAGESIZE) {
                
            // `a` is the process virtual address for the next code/data page
            // map address for each segment, allowing the process to access its own memory - kalloc mapping
            uintptr_t pa = (uintptr_t) kalloc(PAGESIZE); //physical address to run the process
            int perm = PTE_P | PTE_U ; 

            if (seg.writable()) {
                perm |= PTE_W;
            }

            int r = vmiter(ptable[pid].pagetable,a).try_map(pa,perm);
            assert (r==0);
            
            uintptr_t offset =  a - seg.va();

            memset((void*) pa, 0, PAGESIZE);
            if (offset < seg.data_size()){
                memcpy((void*) pa, seg.data() + offset, min(PAGESIZE, seg.data_size() - offset));
            }
            
           
        }
    }

    set_pagetable(ptable[pid].pagetable);

    // copy instructions and data from program image into process memory
    // for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
    //     memset((void*) seg.va(), 0, seg.size());
    //     memcpy((void*) seg.va(), seg.data(), seg.data_size());
    // }


    // mark entry point
    ptable[pid].regs.reg_rip = pgm.entry();

    // allocate and map stack segment
    // Compute process virtual address for stack page
    uintptr_t stack_addr = MEMSIZE_VIRTUAL - PAGESIZE;
    uintptr_t stack_pa = (uintptr_t) kalloc(PAGESIZE);

    //allow process access to the stack
    int perm = PTE_P | PTE_W | PTE_U;
    int r = vmiter(ptable[pid].pagetable,stack_addr).try_map(stack_pa,perm);
    assert (r==0);


    // The handout code requires that the corresponding physical address
    // is currently free.
    // assert(physpages[stack_addr / PAGESIZE].refcount == 0);
    // ++physpages[stack_addr / PAGESIZE].refcount;
    ptable[pid].regs.reg_rsp = stack_addr + PAGESIZE;

    // set_pagetable(kernel_pagetable);

    // mark process as runnable
    ptable[pid].state = P_RUNNABLE;
}

// sys_fork()
//    Fork the current process. On success, returns the child's process ID to
//    the parent, and returns 0 to the child. On failure, returns a negative
//    error code without creating a new process.

int sys_fork(){

    // look for a free process slot in the pagetabe
    pid_t child_pid = -1;
    for (pid_t i = 1; i < PID_MAX; i++) {
        if (ptable[i].state == P_FREE){
            child_pid = i;
            break;
        }
    }

    if(child_pid == -1){
        return -1;
    }

    // allocate a pagetable for the child
    ptable[child_pid].pagetable = kalloc_pagetable();
    if (ptable[child_pid].pagetable == nullptr){
        kfree(ptable[child_pid].pagetable);
        return -1;
    }

    // copy the parent's mappings into the child page table
    for (uintptr_t addr = 0; addr < MEMSIZE_VIRTUAL; addr += PAGESIZE) {
        int perm = vmiter(current -> pagetable,addr).perm();
        uintptr_t pa = vmiter(current -> pagetable,addr).pa();

        if (addr < PROC_START_ADDR){
            int r = vmiter(ptable[child_pid].pagetable, addr).try_map(pa, perm);
            if (r != 0){
                free_ptmemory_helper(ptable[child_pid].pagetable);
                return -1;
            }
        }else if(((perm & PTE_PWU )== PTE_PWU) && (addr != CONSOLE_ADDR)){
            uintptr_t new_pa = (uintptr_t) kalloc(PAGESIZE);
            if (new_pa == (uintptr_t) nullptr) {
                free_memory_helper ((void*)addr, ptable[child_pid].pagetable);
                free_ptmemory_helper(ptable[child_pid].pagetable);
                return -1;

            }

            memcpy((void*)new_pa,(void*)pa, PAGESIZE);
            int r = vmiter(ptable[child_pid].pagetable, addr).try_map(new_pa, perm);


            if (r != 0){
                kfree((void*)new_pa);
                free_memory_helper((void*)addr, ptable[child_pid].pagetable);
                free_ptmemory_helper(ptable[child_pid].pagetable);
                return -1;
            }

        } else if ((perm & (PTE_P | PTE_U)) == (PTE_P | PTE_U)){
            // Read-only or shared page: Share between parent and child
            ++physpages[pa / PAGESIZE].refcount;
            int r = vmiter(ptable[child_pid].pagetable, addr).try_map(pa, perm);
            if (r != 0) {
                free_ptmemory_helper(ptable[child_pid].pagetable);
            return -1;

            }
        }
    }  

    // registers need to be copied form parent to child - regs
    ptable[child_pid].regs = current -> regs;
    
    // regs.reg_rax set to 0
    ptable[child_pid].regs.reg_rax = 0;

    // state of child set to runable
    ptable[child_pid].state = P_RUNNABLE;

    // return child pid  
    return child_pid;
}

// sys_exit should mark a process as free and free all of its memory. 
//This includes the process’s code, data, heap, and stack pages, as well 
//as the pages used for its page directory and page table pages. 
//The memory should become available again for future allocations.

int sys_exit(){

    // free process memory
    for (uintptr_t addr = 0; addr < MEMSIZE_VIRTUAL; addr += PAGESIZE){
        uintptr_t pa = vmiter(current -> pagetable,addr).pa();
        int perm = vmiter(current -> pagetable,addr).perm();
        int x = PTE_P | PTE_U;
        if (((perm & x) == x) && (pa != (uintptr_t) nullptr) && (addr != CONSOLE_ADDR)){
            kfree((void*)pa);
        }
    }

    // free page table memory
    for (ptiter it(current -> pagetable); it.va() < MEMSIZE_VIRTUAL; it.next()) {
        kfree((void*)it.pa());
        
    }

    // mark the process as free
    current -> state = P_FREE;
    kfree(current->pagetable);
    
    current->regs.reg_rax = 0;
    schedule();

    // it will never reach
    return -1;

}

void free_memory_helper (void* fail_addr, x86_64_pagetable* pagetable){
    for (uintptr_t addr = 0; addr < (uintptr_t) fail_addr; addr += PAGESIZE){
        uintptr_t pa = vmiter(pagetable,addr).pa();
        int perm = vmiter(pagetable,addr).perm();
        int x = PTE_P | PTE_U;
        if (((perm & x) == x) && (pa != (uintptr_t) nullptr) && (addr != CONSOLE_ADDR)){
                kfree((void*)pa);
        }
    }
}

void free_ptmemory_helper ( x86_64_pagetable* pagetable){
    for (ptiter it(pagetable); it.va() < MEMSIZE_VIRTUAL; it.next()) {
        kfree((void*)it.pa());
            
    }
    kfree(pagetable);


}


// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.

void exception(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: exception %d at rip %p\n",
                current->pid, regs->reg_intno, regs->reg_rip); */

    // Show the current cursor location and memory state
    // (unless this is a kernel fault).
    console_show_cursor(cursorpos);
    if (regs->reg_intno != INT_PF || (regs->reg_errcode & PTE_U)) {
        memshow();
    }

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER:
        ++ticks;
        lapicstate::get().ack();
        schedule();
        break;                  /* will not be reached */

    case INT_PF: {
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        const char* operation = regs->reg_errcode & PTE_W
                ? "write" : "read";
        const char* problem = regs->reg_errcode & PTE_P
                ? "protection problem" : "missing page";

        if (!(regs->reg_errcode & PTE_U)) {
            proc_panic(current, "Kernel page fault on %p (%s %s, rip=%p)!\n",
                       addr, operation, problem, regs->reg_rip);
        }
        error_printf(CPOS(24, 0), COLOR_ERROR,
                     "PAGE FAULT on %p (pid %d, %s %s, rip=%p)!\n",
                     addr, current->pid, operation, problem, regs->reg_rip);
        log_print_backtrace(current);
        current->state = P_FAULTED;
        break;
    }

    default:
        proc_panic(current, "Unhandled exception %d (rip=%p)!\n",
                   regs->reg_intno, regs->reg_rip);

    }


    // Return to the current process (or run something else).
    if (current->state == P_RUNNABLE) {
        run(current);
    } else {
        schedule();
    }
}


int syscall_page_alloc(uintptr_t addr);

// syscall(regs)
//    Handle a system call initiated by a `syscall` instruction.
//    The process’s register values at system call time are accessible in
//    `regs`.
//
//    If this function returns with value `V`, then the user process will
//    resume with `V` stored in `%rax` (so the system call effectively
//    returns `V`). Alternately, the kernel can exit this function by
//    calling `schedule()`, perhaps after storing the eventual system call
//    return value in `current->regs.reg_rax`.
//
//    It is only valid to return from this function if
//    `current->state == P_RUNNABLE`.
//
//    Note that hardware interrupts are disabled when the kernel is running.

uintptr_t syscall(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: syscall %d at rip %p\n",
                  current->pid, regs->reg_rax, regs->reg_rip); */

    // Show the current cursor location and memory state.
    console_show_cursor(cursorpos);
    memshow();

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_rax) {

    case SYSCALL_PANIC:
        user_panic(current);
        break; // will not be reached

    case SYSCALL_GETPID:
        return current->pid;

    case SYSCALL_YIELD:
        current->regs.reg_rax = 0;
        schedule();             // does not return

    case SYSCALL_PAGE_ALLOC:
        return syscall_page_alloc(current->regs.reg_rdi);

    case SYSCALL_FORK:
        return sys_fork();

    case SYSCALL_EXIT:
        return sys_exit();

    default:
        proc_panic(current, "Unhandled system call %ld (pid=%d, rip=%p)!\n",
                   regs->reg_rax, current->pid, regs->reg_rip);

    }

    panic("Should not get here!\n");
}


// syscall_page_alloc(addr)
//    Handles the SYSCALL_PAGE_ALLOC system call. This function
//    should implement the specification for `sys_page_alloc`
//    in `u-lib.hh` (but in the handout code, it does not).

int syscall_page_alloc(uintptr_t addr) {
    uintptr_t pa = (uintptr_t) kalloc(PAGESIZE);
    if ((void*) pa == nullptr){

        return -1;
    }
    int perm = PTE_P | PTE_W | PTE_U;
    int r = vmiter(current -> pagetable, addr).try_map(pa, perm);
    // assert (r ==0);

    if (r != 0){
        kfree((void*)pa);
        return -1;
    }
    

    // assert(physpages[addr / PAGESIZE].refcount == 0);
    // ++physpages[addr / PAGESIZE].refcount;
    
    memset((void*) pa, 0, PAGESIZE);
    return 0;
}

// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.

void schedule() {
    pid_t pid = current->pid;
    for (unsigned spins = 1; true; ++spins) {
        pid = (pid + 1) % PID_MAX;
        if (ptable[pid].state == P_RUNNABLE) {
            run(&ptable[pid]);
        }

        // If Control-C was typed, exit the virtual machine.
        check_keyboard();

        // If spinning forever, show the memviewer.
        if (spins % (1 << 12) == 0) {
            memshow();
        }
    }
}


// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.

void run(proc* p) {
    assert(p->state == P_RUNNABLE);
    current = p;

    // Check the process's current registers.
    check_process_registers(p);

    // Check the process's current pagetable.
    check_pagetable(p->pagetable);

    // This function is defined in k-exception.S. It restores the process's
    // registers then jumps back to user mode.
    exception_return(p);

    // should never get here
    while (true) {
    }
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 0;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        showing = (showing + 1) % PID_MAX;
    }

    proc* p = nullptr;
    for (int search = 0; !p && search < PID_MAX; ++search) {
        if (ptable[showing].state != P_FREE
            && ptable[showing].pagetable) {
            p = &ptable[showing];
        } else {
            showing = (showing + 1) % PID_MAX;
        }
    }

    console_memviewer(p);
    if (!p) {
        console_printf(CPOS(10, 26), 0x0F00, "   VIRTUAL ADDRESS SPACE\n"
            "                          [All processes have exited]\n"
            "\n\n\n\n\n\n\n\n\n\n\n");
    }
}
