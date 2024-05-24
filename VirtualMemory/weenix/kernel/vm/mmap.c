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

#include "globals.h"
#include "errno.h"
#include "types.h"

#include "mm/mm.h"
#include "mm/tlb.h"
#include "mm/mman.h"
#include "mm/page.h"

#include "proc/proc.h"

#include "util/string.h"
#include "util/debug.h"

#include "fs/vnode.h"
#include "fs/vfs.h"
#include "fs/file.h"

#include "vm/vmmap.h"
#include "vm/mmap.h"


static int is_valid_fd(int fd){
        return (fd >= 0 && fd < NFILES);
}

/*
 * This function implements the mmap(2) syscall, but only
 * supports the MAP_SHARED, MAP_PRIVATE, MAP_FIXED, and
 * MAP_ANON flags.
 *
 * Add a mapping to the current process's address space.
 * You need to do some error checking; see the ERRORS section
 * of the manpage for the problems you should anticipate.
 * After error checking most of the work of this function is
 * done by vmmap_map(), but remember to clear the TLB.
 */
int
do_mmap(void *addr, size_t len, int prot, int flags,
        int fd, off_t off, void **ret)
{
        if (len == 0){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        if (!((flags & MAP_TYPE) == MAP_SHARED || (flags & MAP_TYPE) == MAP_PRIVATE)){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        if (!PAGE_ALIGNED(off)){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        if (!(flags & MAP_ANON) && (flags & MAP_FIXED) && !PAGE_ALIGNED(addr)){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        // addr: pointer, convert to integer; addr: starts point; USER_MEM_LOW: lowest address
        if (addr != NULL &&  USER_MEM_LOW > (size_t)addr){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        // USER_MEM_HIGH: highest address
        if (len > USER_MEM_HIGH){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        if (addr != NULL && len + (size_t)addr > USER_MEM_HIGH){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        // flags: 32 bit; MAP_FIXED:4 -> 00000000000000000000000000000100
        if (addr == 0 && (flags & MAP_FIXED)){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }

        vnode_t *vnode = NULL; // set as NULL first
        // if is anonymous obj -> remain as NULL: not related to file object

        // MAP_FIXED:4 -> 00000000000000000000000000001000
        // if not from anonymous obj -> check file object
        if (!(flags & MAP_ANON)){

                if ( !is_valid_fd(fd) || curproc->p_files[fd] == NULL) {
                        dbg(DBG_PRINT, "(GRADING3D)\n");
                        return -EBADF;
                }

                file_t *f = curproc->p_files[fd];
                vnode = f->f_vnode;
                if ((flags & MAP_SHARED) && (prot & PROT_WRITE)) {
                        if ( !((f->f_mode & FMODE_READ) && (f->f_mode & FMODE_WRITE) )) {
                                dbg(DBG_PRINT, "(GRADING3D)\n");
                                return -EACCES;
                        } 
                }
                if ( flags & MAP_PRIVATE ) {
                        if( !(f->f_mode & FMODE_READ) ) {
                                dbg(DBG_PRINT, "(GRADING3D)\n");
                                return -EACCES;
                        }
                }
        } 

        vmarea_t *vm_area;
        int ret_code = vmmap_map(curproc->p_vmmap, vnode, ADDR_TO_PN(addr),
                (uint32_t)PAGE_ALIGN_UP(len) / PAGE_SIZE, prot, flags, off, VMMAP_DIR_HILO, &vm_area);

        if (ret != NULL && ret_code >= 0){
                *ret = PN_TO_ADDR(vm_area->vma_start);
                pt_unmap_range(curproc->p_pagedir, (uintptr_t) PN_TO_ADDR(vm_area->vma_start), (uintptr_t) PN_TO_ADDR(vm_area->vma_start) + (uintptr_t) PAGE_ALIGN_UP(len));        
                tlb_flush_range((uintptr_t) PN_TO_ADDR(vm_area->vma_start), (uint32_t)PAGE_ALIGN_UP(len) / PAGE_SIZE);
        
                KASSERT(NULL != curproc->p_pagedir); /* page table must be valid after a memory segment is mapped into the address space */
                dbg(DBG_PRINT, "(GRADING3A 2.a)\n");
        }
        dbg(DBG_PRINT, "(GRADING3B)\n");
        return ret_code;
}


/*
 * This function implements the munmap(2) syscall.
 *
 * As with do_mmap() it should perform the required error checking,
 * before calling upon vmmap_remove() to do most of the work.
 * Remember to clear the TLB.
 */
int
do_munmap(void *addr, size_t len)
{
        
        if (len == 0){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        if (!PAGE_ALIGNED(addr)){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL; 
        }
        if ((uintptr_t)addr < USER_MEM_LOW){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        if (USER_MEM_HIGH - (uint32_t)addr < len){
                dbg(DBG_PRINT, "(GRADING3D)\n");
                return -EINVAL;
        }
        
        int ret = vmmap_remove(curproc->p_vmmap, ADDR_TO_PN(addr), (uint32_t)PAGE_ALIGN_UP(len) / PAGE_SIZE);
        dbg(DBG_PRINT, "(GRADING3B)\n");
        return ret;
}

