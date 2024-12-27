// with gcc's __fentry__ and __return__, we define them once in the executable
// and then shared objects loaded by that executable get their references to
// both of these functions resolved to the ones in the executable.
//
// it doesn't work this way with LLVM XRay, because each shared object comes
// with its own definition of __xray_Function* and there's no trivial way to make
// the XRay runtime code linked into the .so reference the global functions instead;
// in particular, the runtime code patching functions to call __xray_Function*
// (instead of the nops put there by the compiler) might complain that the distance
// between the patched code and the jump target (the executable's __xray_Function*
// implementation) is too large to fit in the encoding of the instruction used
// for the patching (I got to the point where it did complain in testing...)
//
// so instead we replace the LLVM XRay runtime's __xray_Function* implementations
// with jumps to the executable's XRay functions; the executable exports
// exe_xray_Function* implementations for the code below to call.
//
// this code is generated from C code calling one function from another;
// we implement this in assembly since the C code might clobber registers
// which it shouldn't (which doesn't happen under -O3 but we don't want to
// take chances on this - these are not normal function calls that can rely
// on the calling convention)
	.text
	.hidden	__xray_FunctionEntry
	.globl	__xray_FunctionEntry
	.p2align	4
	.type	__xray_FunctionEntry,@function
__xray_FunctionEntry:
	.cfi_startproc
	jmp	exe_xray_FunctionEntry@PLT
	.cfi_endproc
	.size	__xray_FunctionEntry, .-__xray_FunctionEntry

	.hidden	__xray_FunctionExit
	.globl	__xray_FunctionExit
	.p2align	4
	.type	__xray_FunctionExit,@function
__xray_FunctionExit:
	.cfi_startproc
	jmp	exe_xray_FunctionExit@PLT
	.cfi_endproc
	.size	__xray_FunctionExit, .-__xray_FunctionExit

	.hidden	__xray_FunctionTailExit
	.globl	__xray_FunctionTailExit
	.p2align	4
	.type	__xray_FunctionTailExit,@function
__xray_FunctionTailExit:
	.cfi_startproc
	jmp	exe_xray_FunctionTailExit@PLT
	.cfi_endproc
	.size	__xray_FunctionTailExit, .-__xray_FunctionTailExit


