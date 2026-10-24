/* Stand-in for <mach/mach_vm.h>: reading memory through the task port (used by the thread diagnostics to walk guest
   frame chains safely) copies with a plain memcpy guarded by the caller's checks; the other calls fail. */
#pragma once
#include <string.h>
#include <mach/mach.h>
static inline kern_return_t mach_vm_read_overwrite(mach_port_t t, mach_vm_address_t a, mach_vm_size_t s,
                                                   mach_vm_address_t data, mach_vm_size_t *out) {
    (void)t;
    if (!compat_readable(a, s)) return KERN_FAILURE;
    memcpy((void *)(uintptr_t)data, (const void *)(uintptr_t)a, (size_t)s);
    *out = s;
    return KERN_SUCCESS;
}
