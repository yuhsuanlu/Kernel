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
#include "errno.h"

#include "util/debug.h"
#include "util/string.h"

#include "proc/proc.h"
#include "proc/kthread.h"

#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/page.h"
#include "mm/pframe.h"
#include "mm/mmobj.h"
#include "mm/pagetable.h"
#include "mm/tlb.h"

#include "fs/file.h"
#include "fs/vnode.h"

#include "vm/shadow.h"
#include "vm/vmmap.h"

#include "api/exec.h"

#include "main/interrupt.h"

/* Pushes the appropriate things onto the kernel stack of a newly forked thread
 * so that it can begin execution in userland_entry.
 * regs: registers the new thread should have on execution
 * kstack: location of the new thread's kernel stack
 * Returns the new stack pointer on success. */
static uint32_t
fork_setup_stack(const regs_t *regs, void *kstack)
{
        /* Pointer argument and dummy return address, and userland dummy return
         * address */
        uint32_t esp = ((uint32_t) kstack) + DEFAULT_STACK_SIZE - (sizeof(regs_t) + 12);
        *(void **)(esp + 4) = (void *)(esp + 8); /* Set the argument to point to location of struct on stack */
        memcpy((void *)(esp + 8), regs, sizeof(regs_t)); /* Copy over struct */
        return esp;
}


/*
 * The implementation of fork(2). Once this works,
 * you're practically home free. This is what the
 * entirety of Weenix has been leading up to.
 * Go forth and conquer.
 */
int
do_fork(struct regs *regs)
{
	KASSERT(regs != NULL);
	KASSERT(curproc != NULL);
	KASSERT(curproc->p_state == PROC_RUNNING);
	dbg(DBG_PRINT, "(GRADING3A 7.a)\n");
	dbg(DBG_PRINT, "(GRADING3B)\n");

	// create new proc
	proc_t *newproc = NULL;
	newproc = proc_create("newForkedProc");
	if(!newproc) {
		curthr->kt_errno = ENOMEM;
		return -ENOMEM;
	}

	// create newthr and link with newproc
        kthread_t *newthr = kthread_clone(curthr);
	if(!newthr) {
		pt_destroy_pagedir(newproc->p_pagedir);
                list_remove(&curproc->p_list_link);
                list_remove(&curproc->p_child_link);
                vput(curproc->p_cwd);
		curthr->kt_errno = ENOMEM;
		return -ENOMEM;
	}
	list_insert_tail(&newproc->p_threads, &newthr->kt_plink);
        newthr->kt_proc = newproc;

	// setup newproc heap
	newproc->p_brk = curproc->p_brk;
        newproc->p_start_brk = curproc-> p_start_brk;

	// create newproc vmmap and vmarea
	newproc->p_vmmap = vmmap_clone(curproc->p_vmmap);
	if(!newproc->p_vmmap) {
		pt_destroy_pagedir(newproc->p_pagedir);
		list_remove(&curproc->p_list_link);
		list_remove(&curproc->p_child_link);
		vput(curproc->p_cwd);
		curthr->kt_errno = ENOMEM;
		return -ENOMEM;
	}

	// setup vmarea mmobjs
	newproc->p_vmmap->vmm_proc = newproc;
	vmarea_t *curr_c_vma = NULL;
	list_iterate_begin(&newproc->p_vmmap->vmm_list, curr_c_vma, vmarea_t, vma_plink) {
		vmarea_t *curr_p_vma = vmmap_lookup(curproc->p_vmmap, curr_c_vma->vma_start);
		if((curr_c_vma->vma_flags & MAP_TYPE) == MAP_PRIVATE) { // private:  create new shadow mmobjs
			dbg(DBG_PRINT, "(GRADING3B)\n");
			mmobj_t *new_p_shadow = shadow_create();
			KASSERT(new_p_shadow);
			new_p_shadow->mmo_shadowed = curr_p_vma->vma_obj;
			new_p_shadow->mmo_un.mmo_bottom_obj = mmobj_bottom_obj(curr_p_vma->vma_obj);

			mmobj_t *new_c_shadow = shadow_create();
			KASSERT(new_c_shadow);
			new_c_shadow->mmo_shadowed = curr_p_vma->vma_obj;
                        new_c_shadow->mmo_un.mmo_bottom_obj = mmobj_bottom_obj(curr_p_vma->vma_obj);
			
			curr_p_vma->vma_obj->mmo_ops->ref(curr_p_vma->vma_obj); // ref count +1
			curr_p_vma->vma_obj = new_p_shadow;
			curr_c_vma->vma_obj = new_c_shadow;
		} else { // shared: update curr mmobj
			dbg(DBG_PRINT, "(GRADING3B)\n");
			curr_c_vma->vma_obj = curr_p_vma->vma_obj;
			curr_c_vma->vma_obj->mmo_ops->ref(curr_c_vma->vma_obj); // ref count +1
		}
		list_insert_tail(mmobj_bottom_vmas(curr_p_vma->vma_obj), &curr_c_vma->vma_olink);
	} list_iterate_end();

	KASSERT(newproc->p_state == PROC_RUNNING);
	KASSERT(newproc->p_pagedir != NULL);
	KASSERT(newthr->kt_kstack != NULL);
    dbg(DBG_PRINT, "(GRADING3A 7.a)\n");


	// setup newproc p_files
	int f;
        for(f = 0; f < NFILES; ++f) {
                newproc->p_files[f] = curproc->p_files[f];
                if(newproc->p_files[f]) {
                        fref(newproc->p_files[f]); // ref count +1
                }
	}

	// setup newthr kt_ctx
	regs->r_eax = 0; // return 0 to child proc
	(newthr->kt_ctx).c_eip = (uint32_t) userland_entry;
	(newthr->kt_ctx).c_esp = fork_setup_stack(regs, newthr->kt_kstack);
	(newthr->kt_ctx).c_pdptr = newproc->p_pagedir;
	(newthr->kt_ctx).c_kstack = (uintptr_t) newthr->kt_kstack;
	(newthr->kt_ctx).c_kstacksz = DEFAULT_STACK_SIZE;

	// update pagetable and tlb
	pt_unmap_range(curproc->p_pagedir, USER_MEM_LOW, USER_MEM_HIGH);
	tlb_flush_all();

	// put into runq
	sched_make_runnable(newthr);

	// return child pid to parent proc
	regs->r_eax = newproc->p_pid;
	dbg(DBG_PRINT, "(GRADING3B)\n");

        return newproc->p_pid;
}
