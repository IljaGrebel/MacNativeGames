/* Stand-in for <mach/mach.h>: the types the upstream runtime names. The calls that read other threads' state
   (diagnostics only) fail, so those diagnostics report nothing on this port. */
#pragma once
#include <stdint.h>
typedef int kern_return_t;
typedef unsigned mach_port_t;
typedef unsigned mach_msg_type_number_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef int vm_prot_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
static inline mach_port_t mach_task_self(void) { return 0; }
/* thread suspension and register reads (thread diagnostics): not available, the diagnostics skip the thread */
typedef int thread_state_flavor_t;
typedef unsigned *thread_state_t;
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(uint32_t)))
#define arm_thread_state64_get_pc(s) ((s).__pc)
#define arm_thread_state64_get_lr(s) ((s).__lr)
#define arm_thread_state64_get_fp(s) ((s).__fp)
#define arm_thread_state64_get_sp(s) ((s).__sp)
static inline kern_return_t thread_suspend(mach_port_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(mach_port_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(mach_port_t t, int f, thread_state_t s, mach_msg_type_number_t *c) {
    (void)t; (void)f; (void)s; (void)c; return KERN_FAILURE;
}

/* virtual memory: implemented in compat_vm.c over guest_mem's reserve/query (VirtualAlloc / mmap) */
typedef unsigned vm_map_t;
typedef int boolean_t;
typedef uint64_t mach_vm_offset_t, memory_object_offset_t, vm_address_t, vm_size_t, vm_offset_t;
typedef unsigned mem_entry_name_port_t, natural_t;
typedef int vm_inherit_t, vm_region_flavor_t;
typedef int *vm_region_info_t, *vm_region_recurse_info_t;
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_OVERWRITE 0x4000
#define MEMORY_OBJECT_NULL 0
#define MACH_PORT_NULL 0
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7
#define VM_INHERIT_DEFAULT 1
#define VM_INHERIT_NONE 2
#define KERN_NO_SPACE 3
#define KERN_INVALID_ADDRESS 1
typedef struct { vm_prot_t protection, max_protection; vm_inherit_t inheritance; boolean_t shared, reserved;
                 memory_object_offset_t offset; int behavior; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))
typedef struct { vm_prot_t protection, max_protection; unsigned pages_resident, pages_shared_now_private,
                 pages_swapped_out, pages_dirtied, ref_count; } vm_region_submap_info_data_64_t;
#define VM_REGION_SUBMAP_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_submap_info_data_64_t) / sizeof(int)))
extern uint64_t compat_vm_page_size;
#define vm_page_size compat_vm_page_size
kern_return_t mach_vm_map(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, mach_vm_offset_t mask, int flags,
                          mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy, vm_prot_t cur,
                          vm_prot_t max, vm_inherit_t inh);
kern_return_t mach_vm_allocate(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t s, int flags);
kern_return_t mach_vm_deallocate(vm_map_t t, mach_vm_address_t a, mach_vm_size_t s);
kern_return_t mach_vm_region(vm_map_t t, mach_vm_address_t *a, mach_vm_size_t *s, vm_region_flavor_t flavor,
                             vm_region_info_t info, mach_msg_type_number_t *cnt, mach_port_t *obj);
kern_return_t vm_region_recurse_64(vm_map_t t, vm_address_t *a, vm_size_t *s, natural_t *depth,
                                   vm_region_recurse_info_t info, mach_msg_type_number_t *cnt);
static inline kern_return_t mach_port_deallocate(vm_map_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }

/* guarded reads of possibly unmapped memory (diagnostics): compat_readable checks the range first */
int compat_readable(uint64_t address, uint64_t size);
static inline kern_return_t vm_read_overwrite(vm_map_t t, vm_address_t a, vm_size_t s, vm_address_t data, vm_size_t *out) {
    (void)t;
    if (!compat_readable(a, s)) return KERN_INVALID_ADDRESS;
    for (vm_size_t i = 0; i < s; i++) ((volatile unsigned char *)(uintptr_t)data)[i] = ((const volatile unsigned char *)(uintptr_t)a)[i];
    *out = s;
    return KERN_SUCCESS;
}
