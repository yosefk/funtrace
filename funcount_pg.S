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

        //save registers where arguments might have been passed.
        //(we only want to save __cyg_profile_func_enter - we don't optimize
        //funcount as much as funtrace; our goal is just to count the calls
        //that would have been instrumented by funtrace in -pg/XRay modes)
        sub    $0x38,%rsp
        mov    %rax,(%rsp)
        mov    %rcx,0x8(%rsp)
        mov    %rdx,0x10(%rsp)
        mov    %rsi,0x18(%rsp)                     
        mov    %rdi,0x20(%rsp)
        mov    %r8,0x28(%rsp)
        mov    %r9,0x30(%rsp)
        mov    0x38(%rsp),%rdi
        call   __cyg_profile_func_enter
        mov    0x30(%rsp),%r9
        mov    0x28(%rsp),%r8
        mov    0x20(%rsp),%rdi
        mov    0x18(%rsp),%rsi
        mov    0x10(%rsp),%rdx
        mov    0x8(%rsp),%rcx
        mov    (%rsp),%rax
        add    $0x38,%rsp

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

        // funcount.cpp doesn't use __cyg_profile_func_exit
        ret

        .cfi_endproc
        .size   __return__, .-__return__
        .size   __xray_FunctionExit, .-__xray_FunctionExit
        .size   exe_xray_FunctionExit, .-exe_xray_FunctionExit
        .size   __xray_FunctionTailExit, .-__xray_FunctionTailExit
        .size   exe_xray_FunctionTailExit, .-exe_xray_FunctionTailExit
