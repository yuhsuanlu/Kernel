/******************************************************************************/
/* Important Spring 2023 CSCI 402 usage information:                          */
/*                                                                            */
/* This fils is part of CSCI 402 kernel programming assignments at USC.       */
/*         53616c7465645f5fd1e93dbf35cbffa3aef28f8c01d8cf2ffc51ef62b26a       */
/*         f9bda5a68e5ed8c972b17bab0f42e24b19daa7bd408305b1f7bd6c7208c1       */
/*         0e36230e913039b3046dd5fd0ba706a624d33dbaa4d6aab02c82fe09f561       */
/*         01b0fd977b0051f0b0ce0c69f7db857b1b5e007be2db6d42894bf93de848       */
/*         806d9152bd5715e9                                                   */
/* Please understand that you are NOT permitted to distribute or publically   */
/*         display a copy of this file (or ANY PART of it) for any reason.    */
/* If anyone (including your prospective employer) asks you to post the code, */
/*         you must inform them that you do NOT have permissions to do so.    */
/* You are also NOT permitted to remove or alter this comment block.          */
/* If this comment block is removed or altered in a submitted file, 20 points */
/*         will be deducted.                                                  */
/******************************************************************************/

#include "types.h"
#include "globals.h"
#include "kernel.h"
#include "errno.h"

#include "util/gdb.h"
#include "util/init.h"
#include "util/debug.h"
#include "util/string.h"
#include "util/printf.h"

#include "mm/mm.h"
#include "mm/page.h"
#include "mm/pagetable.h"
#include "mm/pframe.h"

#include "vm/vmmap.h"
#include "vm/shadowd.h"
#include "vm/shadow.h"
#include "vm/anon.h"

#include "main/acpi.h"
#include "main/apic.h"
#include "main/interrupt.h"
#include "main/gdt.h"

#include "proc/sched.h"
#include "proc/proc.h"
#include "proc/kthread.h"

#include "drivers/dev.h"
#include "drivers/blockdev.h"
#include "drivers/disk/ata.h"
#include "drivers/tty/virtterm.h"
#include "drivers/pci.h"

#include "api/exec.h"
#include "api/syscall.h"

#include "fs/vfs.h"
#include "fs/vnode.h"
#include "fs/vfs_syscall.h"
#include "fs/fcntl.h"
#include "fs/stat.h"

#include "test/kshell/kshell.h"
#include "test/s5fs_test.h"

GDB_DEFINE_HOOK(boot)
GDB_DEFINE_HOOK(initialized)
GDB_DEFINE_HOOK(shutdown)

static void *bootstrap(int arg1, void *arg2);
static void *idleproc_run(int arg1, void *arg2);
static kthread_t *initproc_create(void);
static void *initproc_run(int arg1, void *arg2);
static void hard_shutdown(void);

static context_t bootstrap_context;
extern int gdb_wait;
extern void *sunghan_test(int, void *);
extern void *sunghan_deadlock_test(int, void *);
extern void *faber_thread_test(int, void *);

extern void *vfstest_main(int, void *);
extern int faber_fs_thread_test(kshell_t *, int, char **);
extern int faber_directory_test(kshell_t *, int, char **);

/**
 * This is the first real C function ever called. It performs a lot of
 * hardware-specific initialization, then creates a pseudo-context to
 * execute the bootstrap function in.
 */
void kmain()
{
        GDB_CALL_HOOK(boot);

        dbg_init();
        dbgq(DBG_CORE, "Kernel binary:\n");
        dbgq(DBG_CORE, "  text: 0x%p-0x%p\n", &kernel_start_text, &kernel_end_text);
        dbgq(DBG_CORE, "  data: 0x%p-0x%p\n", &kernel_start_data, &kernel_end_data);
        dbgq(DBG_CORE, "  bss:  0x%p-0x%p\n", &kernel_start_bss, &kernel_end_bss);

        page_init();

        pt_init();
        slab_init();
        pframe_init();

        acpi_init();
        apic_init();
        pci_init();
        intr_init();

        gdt_init();

        /* initialize slab allocators */
#ifdef __VM__
        anon_init();
        shadow_init();
#endif
        vmmap_init();
        proc_init();
        kthread_init();

#ifdef __DRIVERS__
        bytedev_init();
        blockdev_init();
#endif

        void *bstack = page_alloc();
        pagedir_t *bpdir = pt_get();
        KASSERT(NULL != bstack && "Ran out of memory while booting.");
        /* This little loop gives gdb a place to synch up with weenix.  In the
         * past the weenix command started qemu was started with -S which
         * allowed gdb to connect and start before the boot loader ran, but
         * since then a bug has appeared where breakpoints fail if gdb connects
         * before the boot loader runs.  See
         *
         * https://bugs.launchpad.net/qemu/+bug/526653
         *
         * This loop (along with an additional command in init.gdb setting
         * gdb_wait to 0) sticks weenix at a known place so gdb can join a
         * running weenix, set gdb_wait to zero  and catch the breakpoint in
         * bootstrap below.  See Config.mk for how to set GDBWAIT correctly.
         *
         * DANGER: if GDBWAIT != 0, and gdb is not running, this loop will never
         * exit and weenix will not run.  Make SURE the GDBWAIT is set the way
         * you expect.
         */
        while (gdb_wait)
                ;
        context_setup(&bootstrap_context, bootstrap, 0, NULL, bstack, PAGE_SIZE, bpdir);
        context_make_active(&bootstrap_context);

        panic("\nReturned to kmain()!!!\n");
}

/**
 * Clears all interrupts and halts, meaning that we will never run
 * again.
 */
static void
hard_shutdown()
{
#ifdef __DRIVERS__
        vt_print_shutdown();
#endif
        __asm__ volatile("cli; hlt");
}

/**
 * This function is called from kmain, however it is not running in a
 * thread context yet. It should create the idle process which will
 * start executing idleproc_run() in a real thread context.  To start
 * executing in the new process's context call context_make_active(),
 * passing in the appropriate context. This function should _NOT_
 * return.
 *
 * Note: Don't forget to set curproc and curthr appropriately.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *
bootstrap(int arg1, void *arg2)
{
        /* If the next line is removed/altered in your submission, 20 points will be deducted. */
        dbgq(DBG_TEST, "SIGNATURE: 53616c7465645f5fd698072b750d0414f47a28cd37b78b0c6c3e04a898842e32219ff78efadefe125e25f9988e9cd8c7\n");
        /* necessary to finalize page table information */
        pt_template_init();

        proc_t *proc_0 = proc_create("idle");
        kthread_t *thread_0 = kthread_create(proc_0, idleproc_run, arg1, arg2);
        KASSERT(proc_0 && thread_0 && "Cannot create thread or process");
        curproc = proc_0;
        curthr = thread_0;

        KASSERT(NULL != curproc);
        KASSERT(PID_IDLE == curproc->p_pid);
        KASSERT(NULL != curthr);
        dbg(DBG_PRINT, "(GRADING1A 1.a)\n");

        context_make_active(&thread_0->kt_ctx);

        panic("weenix returned to bootstrap()!!! BAD!!!\n");
        return NULL;
}

/**
 * Once we're inside of idleproc_run(), we are executing in the context of the
 * first process-- a real context, so we can finally begin running
 * meaningful code.
 *
 * This is the body of process 0. It should initialize all that we didn't
 * already initialize in kmain(), launch the init process (initproc_run),
 * wait for the init process to exit, then halt the machine.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *
idleproc_run(int arg1, void *arg2)
{
        int status;
        pid_t child;

        /* create init proc */
        kthread_t *initthr = initproc_create();
        init_call_all();
        GDB_CALL_HOOK(initialized);

        /* Create other kernel threads (in order) */

#ifdef __VFS__
        /* Once you have VFS remember to set the current working directory
         * of the idle and init processes */
        curproc->p_cwd = vget(vfs_root_vn->vn_fs, vfs_root_vn->vn_vno);
        initthr->kt_proc->p_cwd = vget(vfs_root_vn->vn_fs, vfs_root_vn->vn_vno);

        /* Here you need to make the null, zero, and tty devices using mknod */
        /* You can't do this until you have VFS, check the include/drivers/dev.h
         * file for macros with the device ID's you will need to pass to mknod */
        do_mkdir("/dev");

        do_mknod("/dev/null", S_IFCHR, MKDEVID(1, 0));
        do_mknod("/dev/zero", S_IFCHR, MKDEVID(1, 1));
        do_mknod("/dev/tty0", S_IFCHR, MKDEVID(2, 0));

	dbg(DBG_PRINT, "(GRADING2A)\n");

#endif

        /* Finally, enable interrupts (we want to make sure interrupts
         * are enabled AFTER all drivers are initialized) */
        intr_enable();

        /* Run initproc */
        sched_make_runnable(initthr);
        /* Now wait for it */
        child = do_waitpid(-1, 0, &status);
        KASSERT(PID_INIT == child);

#ifdef __MTP__
        kthread_reapd_shutdown();
#endif

#ifdef __SHADOWD__
        /* wait for shadowd to shutdown */
        shadowd_shutdown();
#endif

#ifdef __VFS__
        /* Shutdown the vfs: */
        dbg_print("weenix: vfs shutdown...\n");
        vput(curproc->p_cwd);
        if (vfs_shutdown())
                panic("vfs shutdown FAILED!!\n");

#endif

                /* Shutdown the pframe system */
#ifdef __S5FS__
        pframe_shutdown();
#endif

        dbg_print("\nweenix: halted cleanly!\n");
        GDB_CALL_HOOK(shutdown);
        hard_shutdown();
        return NULL;
}

/**
 * This function, called by the idle process (within 'idleproc_run'), creates the
 * process commonly refered to as the "init" process, which should have PID 1.
 *
 * The init process should contain a thread which begins execution in
 * initproc_run().
 *
 * @return a pointer to a newly created thread which will execute
 * initproc_run when it begins executing
 */
static kthread_t *
initproc_create(void)
{
        proc_t *p = proc_create("proc1");
        kthread_t *thr = kthread_create(p, initproc_run, NULL, NULL);

        KASSERT(NULL != p);
        KASSERT(PID_INIT == p->p_pid);
        KASSERT(NULL != thr);
        dbg(DBG_PRINT, "(GRADING1A 1.b)\n");

        return thr;
}

#ifdef __DRIVERS__

int do_faber_thread_test()
{
        int rv;
        proc_t *p = proc_create("faber");
        kthread_t *kt = kthread_create(p, faber_thread_test, 0, NULL);
        sched_make_runnable(kt);
        do_waitpid(p->p_pid, 0, &rv);
        return 0;
}

int do_sunghan_test()
{
        int rv;
        proc_t *p = proc_create("sunghan");
        kthread_t *kt = kthread_create(p, sunghan_test, 0, NULL);
        sched_make_runnable(kt);
        do_waitpid(p->p_pid, 0, &rv);
        return 0;
}

int do_deadlock_test()
{
        int rv;
        proc_t *p = proc_create("deadlock");
        kthread_t *kt = kthread_create(p, sunghan_deadlock_test, 0, NULL);
        sched_make_runnable(kt);
        do_waitpid(p->p_pid, 0, &rv);
        return 0;
}

#endif /* __DRIVERS__ */

#ifdef __VFS__
int do_vfstest()
{
        int rv;
        proc_t *p = proc_create("vfstest");
        kthread_t *kt = kthread_create(p, vfstest_main, 1, NULL);
        sched_make_runnable(kt);
        do_waitpid(p->p_pid, 0, &rv);
        return 0;
}

#endif

/**
 * The init thread's function changes depending on how far along your Weenix is
 * developed. Before VM/FI, you'll probably just want to have this run whatever
 * tests you've written (possibly in a new process). After VM/FI, you'll just
 * exec "/sbin/init".
 *
 * Both arguments are unused.
 *
 * @param arg1 the first argument (unused)
 * @param arg2 the second argument (unused)
 */
static void *
initproc_run(int arg1, void *arg2)
{

#ifdef __DRIVERS__

        kshell_add_command("faber", do_faber_thread_test, "invoke faber_thread_test()");
        kshell_add_command("sunghan", do_sunghan_test, "invoke sunghan_test()");
        kshell_add_command("deadlock", do_deadlock_test, "invoke sunghan_deadlock_test()");

#ifdef __VFS__
        kshell_add_command("vfstest", do_vfstest, "invoke vfstest_main()");
        kshell_add_command("thread_test", &faber_fs_thread_test, "invoke faber_fs_thread_test()");
        kshell_add_command("directory_test", &faber_directory_test, "invoke faber_directory_test()");

#endif

        kshell_t *kshell = kshell_create(0);
        if (NULL == kshell)
                panic("init: Couldn't create kernel shell\n");
        while (kshell_execute_next(kshell))
                ;
        kshell_destroy(kshell);

#endif /* __DRIVERS__ */

        dbg(DBG_PRINT, "(GRADING1C)\n");

        return NULL;
}
