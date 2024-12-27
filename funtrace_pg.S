#include "funtrace_buf_size.h"

	.p2align 4
	.globl	__fentry__
	.type	__fentry__, @function
	.globl	__xray_FunctionEntry
	.type	__xray_FunctionEntry, @function
	.globl	exe_xray_FunctionEntry
	.type	exe_xray_FunctionEntry, @function
__fentry__:
__xray_FunctionEntry:
exe_xray_FunctionEntry:
	.cfi_startproc

	// r10 = __builtin_return_address(0)
	movq	(%rsp), %r10
	// rdtsc clobbers rdx which might have been used for a caller's parameter - save
	pushq   %rdx

    // r11 = g_thread_trace.pos
	movq	%fs:g_thread_trace@tpoff, %r11
	// rax = __rdtsc()
	rdtsc
	salq	$32, %rdx
	orq	%rdx, %rax
    // if(!g_thread_trace.enabled) return
	cmpb	$0, %fs:8+g_thread_trace@tpoff
	je	.early_exit_from_fentry
    // pos->func = return_address
	movq	%r10, (%r11)
    // pos++
	addq	$16, %r11
	// pos->cycle = rdtsc (the pos _before_ the increment;
	// gcc generated this code - hoping this way of doing it speeds things up, didn't test)
	movq	%rax, -8(%r11)
    // cyclic buffer wraparound - clear the FUNTRACE_LOG_BUF_SIZE bit in pos
	btr	    $FUNTRACE_LOG_BUF_SIZE, %r11
    // save pos back t g_thread_trace.pos
	movq	%r11, %fs:g_thread_trace@tpoff

.early_exit_from_fentry:
	popq   %rdx
	ret

	.cfi_endproc
	.size	__fentry__, .-__fentry__
	.size   __xray_FunctionEntry, .-__xray_FunctionEntry
	.size   exe_xray_FunctionEntry, .-exe_xray_FunctionEntry


	.p2align 4
	.globl	__return__
	.type	__return__, @function
	.globl	__xray_FunctionExit
	.type	__xray_FunctionExit, @function
	.globl	exe_xray_FunctionExit
	.type	exe_xray_FunctionExit, @function
	.globl	__xray_FunctionTailExit //FIXME this needs to be its own handler
	.type	__xray_FunctionTailExit, @function
	.globl	exe_xray_FunctionTailExit //FIXME this needs to be its own handler
	.type	exe_xray_FunctionTailExit, @function
__return__:
__xray_FunctionExit:
exe_xray_FunctionExit:
__xray_FunctionTailExit: //FIXME
exe_xray_FunctionTailExit: //FIXME
	.cfi_startproc

	movq	(%rsp), %r10

	//rdtsc clobbers both of these; __return__ can't clobber rax
	//(unlike __fentry__ which can.) note that the opposite isn't true -
	//__return__ can't clobber rdx "symmetrically" to __fentry__'s clobbering
	//of rax, because a tail call can happen after the call to __return__
	//(not sure why gcc does it this way but it does) and this tail call
	//might get an argument in rdx
	pushq   %rdx
	pushq   %rax

	
	movq	%fs:g_thread_trace@tpoff, %r11
	rdtsc
	salq	$32, %rdx
	orq	%rdx, %rax
	cmpb	$0, %fs:8+g_thread_trace@tpoff
	je	.early_exit_from_return
    	//this is the main addition in __return__ to the code of __fentry__
	btsq	$FUNTRACE_RETURN_BIT, %r10
	movq	%rax, 8(%r11)
	addq	$16, %r11
	movq	%r10, -16(%r11)
	btr	    $FUNTRACE_LOG_BUF_SIZE, %r11
	movq	%r11, %fs:g_thread_trace@tpoff

.early_exit_from_return:
	popq   %rax
	popq   %rdx
	ret

	.cfi_endproc
	.size	__return__, .-__return__
	.size   __xray_FunctionExit, .-__xray_FunctionExit
	.size	exe_xray_FunctionExit, .-exe_xray_FunctionExit
