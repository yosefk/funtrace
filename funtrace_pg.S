#ifdef FUNTRACE_FUNCOUNT
#include "funcount_pg.S"
#else

#include "funtrace_flags.h"

        .p2align 4
        .globl  __fentry__
        .type   __fentry__, @function
        .globl  __xray_FunctionEntry
        .type   __xray_FunctionEntry, @function
        .globl  exe_xray_FunctionEntry
        .type   exe_xray_FunctionEntry, @function
__fentry__:
__xray_FunctionEntry:
exe_xray_FunctionEntry:
        .cfi_startproc
        // r11 = g_thread_trace.pos
        movq    %fs:g_thread_trace@tpoff, %r11
        // cyclic buffer wraparound - clear the FUNTRACE_LOG_BUF_SIZE bit in pos
        andq    %fs:8+g_thread_trace@tpoff, %r11
        // if(!g_thread_trace.wraparound_mask) return
        je      .early_exit_from_fentry

        // r10 = __builtin_return_address(0)
        movq    (%rsp), %r10
        // rdtsc clobbers rdx which might have been used for a caller's parameter - save
        pushq   %rdx

        // rax = __rdtsc()
        rdtsc
        salq    $32, %rdx
        orq     %rdx, %rax
        // pos->func = return_address
        movq    %r10, (%r11)
        // pos++
        addq    $16, %r11
        // pos->cycle = rdtsc (the pos _before_ the increment; gcc generated this code...)
        movq    %rax, -8(%r11)
        // save pos back to g_thread_trace.pos
        movq    %r11, %fs:g_thread_trace@tpoff

        popq   %rdx
.early_exit_from_fentry:
        ret

// XRay instrumentation (unlike __fentry__/__return__ -pg instrumentation) calls separate
// functions upon returning from a function and upon tail-calling a function instead of
// returning from its caller. you would think this lets us present the tail call correctly (instead
// of, given f which calls g which tail-calls h, misprepresent h as being called from f,
// which we end up doing under -pg instrumentation because __return__ is called by g
// before jumping to h)
//
// however, in practice, it's not clear how to make use of the tail-call vs return distinction.
// for example, you push the tail-caller to the stack and pop it when its callee returns; this
// works well if this tail-callee is instrumented or calls at least one instrumented function
// itself, but what if it doesn't - when is the tail-caller going to be "diagnosed" as having
// returned in that case?
//
// XRay itself records distinct event types, EXIT and TAIL_EXIT, and then xray-converter.cpp
// treats them exactly the same in exportAsChromeTraceEventFormat(). we simply record a single
// event type for both events
        .cfi_endproc
        .size   __fentry__, .-__fentry__
        .size   __xray_FunctionEntry, .-__xray_FunctionEntry
        .size   exe_xray_FunctionEntry, .-exe_xray_FunctionEntry


        .p2align 4
        .globl  __return__
        .type   __return__, @function
        .globl  __xray_FunctionExit
        .type   __xray_FunctionExit, @function
        .globl  exe_xray_FunctionExit
        .type   exe_xray_FunctionExit, @function
        .globl  __xray_FunctionTailExit
        .type   __xray_FunctionTailExit, @function
        .globl  exe_xray_FunctionTailExit
        .type   exe_xray_FunctionTailExit, @function
__return__:
__xray_FunctionExit:
exe_xray_FunctionExit:
__xray_FunctionTailExit:
exe_xray_FunctionTailExit:
        .cfi_startproc

        movq    %fs:g_thread_trace@tpoff, %r11
        andq    %fs:8+g_thread_trace@tpoff, %r11
        je      .early_exit_from_return

        movq    (%rsp), %r10

        //rdtsc clobbers both of these; __return__ can't clobber rax
        //(unlike __fentry__ which can.) note that the opposite isn't true -
        //__return__ can't clobber rdx "symmetrically" to __fentry__'s clobbering
        //of rax, because a tail call can happen after the call to __return__
        //(not sure why gcc does it this way but it does) and this tail call
        //might get an argument in rdx
        pushq   %rdx
        pushq   %rax
        
        rdtsc
        salq    $32, %rdx
        orq     %rdx, %rax
        //this is the main addition in __return__ to the code of __fentry__
#ifndef __clang__
        btsq    $FUNTRACE_RETURN_BIT, %r10
#else
        btsq    $FUNTRACE_RETURN_WITH_CALLER_ADDRESS_BIT, %r10 //XRay jumps to the exit handler
        //rather than calling it; you get an integer ID of the returning function in a register
        //but we don't use it (it's not that trivial to decode, and XRay itself still doesn't
        //do it for functions in shared objects as of early 2025). our return address thus
        //points into the caller of the returning function
#endif
        movq    %rax, 8(%r11)
        addq    $16, %r11
        movq    %r10, -16(%r11)
        movq    %r11, %fs:g_thread_trace@tpoff

        popq   %rax
        popq   %rdx
.early_exit_from_return:
        ret

        .cfi_endproc
        .size   __return__, .-__return__
        .size   __xray_FunctionExit, .-__xray_FunctionExit
        .size   exe_xray_FunctionExit, .-exe_xray_FunctionExit
        .size   __xray_FunctionTailExit, .-__xray_FunctionTailExit
        .size   exe_xray_FunctionTailExit, .-exe_xray_FunctionTailExit
#endif
