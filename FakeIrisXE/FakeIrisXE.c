//
//  FakeIrisXE.c
//  FakeIrisXE
//
//  Created by NuoFang on 2026/1/11.
//

#include <mach/mach_types.h>

kern_return_t FakeIrisXE_start(kmod_info_t * ki, void *d);
kern_return_t FakeIrisXE_stop(kmod_info_t *ki, void *d);

kern_return_t FakeIrisXE_start(kmod_info_t * ki, void *d)
{
    return KERN_SUCCESS;
}

kern_return_t FakeIrisXE_stop(kmod_info_t *ki, void *d)
{
    return KERN_SUCCESS;
}
