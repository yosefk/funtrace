// FIXME: hardcoded 16383
	.p2align 4
	.globl	__fentry__
	.type	__fentry__, @function
__fentry__:
	.cfi_startproc
	pushq   %rdx
	rdtsc
	salq	$32, %rdx
	orq	%rdx, %rax
	cmpb	$0, %fs:4+g_thread_trace@tpoff
	jne	.early_exit_from_fentry
	movslq	%fs:g_thread_trace@tpoff, %r10
	movq	%fs:0, %r11
	movq	%r10, %rdx
	leaq	16+g_thread_trace@tpoff(%r10,%r11), %r11
	movq	8(%rsp), %r10
	addl	$16, %edx
	andl	$16383, %edx
	movq	%rax, 8(%r11)
	movq	%r10, (%r11)
	movl	%edx, %fs:g_thread_trace@tpoff
.early_exit_from_fentry:
	popq   %rdx
	ret
	.cfi_endproc
	.size	__fentry__, .-__fentry__
	.p2align 4
	.globl	__return__
	.type	__return__, @function
__return__:
	.cfi_startproc
	pushq   %rdx
	pushq   %rax
	rdtsc
	salq	$32, %rdx
	orq	%rdx, %rax
	cmpb	$0, %fs:4+g_thread_trace@tpoff
	jne	.early_exit_from_return
	movslq	%fs:g_thread_trace@tpoff, %r10
	movq	%fs:0, %r11
	movq	%r10, %rdx
	leaq	16+g_thread_trace@tpoff(%r10,%r11), %r11
	movq	8(%rsp), %r10
	addl	$16, %edx
	btsq	$63, %r10
	andl	$16383, %edx
	movq	%rax, 8(%r11)
	movq	%r10, (%r11)
	movl	%edx, %fs:g_thread_trace@tpoff
.early_exit_from_return:
	popq   %rax
	popq   %rdx
	ret
	.cfi_endproc
	.size	__return__, .-__return__
