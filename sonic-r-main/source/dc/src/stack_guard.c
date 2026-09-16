/**
 * stack_guard.c — stack-smashing report handler.
 *
 * Newlib supplies a default __stack_chk_fail that aborts without saying
 * anything useful. This overrides it with one that names the function whose
 * frame was overwritten, which is the only part we actually care about.
 *
 * Requires -fstack-protector (any level) in KOS_CFLAGS. kos-cc passes
 * KOS_CFLAGS ahead of the game's own CFLAGS, so game code is instrumented
 * along with KOS.
 *
 * Reading the output: PR is the return address of the function that tripped
 * the canary — it points at the CALLER, so the smashed frame belongs to
 * whatever function that address falls inside. Resolve it with
 *
 *     $KOS_ADDR2LINE -f -C -i -e build/sonicr.elf <PR>
 *
 * The backtrace beneath it only prints usefully when frame pointers survive
 * to the final command line. KOS_CFLAGS sets -fno-omit-frame-pointer, but
 * the game's CFLAGS append -O3 afterwards and -O3 implies the opposite, so
 * the game half needs -fno-omit-frame-pointer repeated in dc/Makefile for
 * these frames to resolve. Without it this still prints, but the frames
 * above __stack_chk_fail will be missing or wrong.
 */

#include <kos.h>
#include <arch/arch.h>
#include <arch/stack.h>
#include <stdio.h>
#include <stdlib.h>

/* __used is mandatory, not decoration: KOS builds with -flto=auto, and LTO
 * will drop an override nothing appears to reference — silently handing the
 * job back to Newlib's default handler. The KOS example calls this out. */
__used void __stack_chk_fail(void)
{
    uintptr_t pr = arch_get_ret_addr();

    printf("\n*** STACK SMASHED ***\n");
    printf("PR=0x%08lx  (resolve with: $KOS_ADDR2LINE -f -C -i -e "
           "build/sonicr.elf 0x%08lx)\n",
           (unsigned long)pr, (unsigned long)pr);

    /* Leave off this frame; the caller is the interesting one. */
    arch_stk_trace(1);

    /* Do not return and do not exit cleanly. The canary means memory is
     * already wrong, so anything further runs on a corrupt stack — stop
     * here while the report above is still the last thing on screen. */
    abort();
}
