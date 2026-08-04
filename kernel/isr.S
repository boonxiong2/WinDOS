/* isr.S — 64-bit ISR stubs, GAS syntax */
.global isr20, isr21, isr2c, isr_default
.global syscall_entry_asm

/* syscall entry: RAX=call num, RDI/RSI/RDX=args.
   syscall clobbers RCX (ret addr) & R11 (user RFLAGS) — save them,
   call C dispatcher, restore, sysretq back to user mode. */
syscall_entry_asm:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %r11          /* user RFLAGS (sysret needs it) */
    pushq %rcx          /* user return address (sysret needs it) */
    /* preserve syscall arg regs across the C dispatcher (SysV clobbers
       caller-saved regs). RAX is the return value — left untouched. */
    pushq %rdi          /* arg1 */
    pushq %rsi          /* arg2 */
    pushq %rdx          /* arg3 */
    pushq %r10          /* arg4 */
    pushq %r8           /* arg5 */
    pushq %r9           /* arg6 */
    call syscall_dispatch
    addq $48, %rsp      /* drop saved args (6*8); RAX = return value */
    popq %rcx
    popq %r11
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    sysretq

.macro ISR_STUB num, handler
.global isr\num
isr\num:
    push %rax; push %rcx; push %rdx; push %rbx
    push %rbp; push %rsi; push %rdi
    push %r8;  push %r9;  push %r10; push %r11
    push %r12; push %r13; push %r14; push %r15
    mov %rsp, %rdi
    call \handler
    pop %r15; pop %r14; pop %r13; pop %r12
    pop %r11; pop %r10; pop %r9;  pop %r8
    pop %rdi; pop %rsi; pop %rbp; pop %rbx
    pop %rdx; pop %rcx; pop %rax
    /* frame now: [RIP, CS, RFLAGS, (RSP, SS)] — Ring3 IRQ has RSP/SS */
    /* iretq to Ring3 #GPs on QEMU (SS user-seg load e=0x20) — use sysretq
       for Ring3 returns; plain iretq for Ring0 (same-privilege) returns. */
    movq 8(%rsp), %rax
    testb $3, %al
    jz 1f
    movq 0(%rsp), %rcx      /* user RIP */
    movq 16(%rsp), %r11     /* user RFLAGS */
    movq 24(%rsp), %rsp     /* user RSP (sysret doesn't set it) */
    sysretq
1:  iretq
.endm

/* Exception stub: pushes vector number, then same as ISR_STUB.
   EXC_STUB    = no CPU error code (0-7,9,16-19): addq $8 for num
   EXC_STUB_ERR = CPU pushes error code (8,10-14,17): addq $16 for num+errcode */
.macro EXC_STUB num
.global exc\num
exc\num:
    /* DEBUG: raw serial "E<num>" */
    pushq %rax
    movw $0x3F8, %dx
    movb $'E', %al
    outb %al, %dx
    movb $'0' + (\num / 10), %al
    outb %al, %dx
    movb $'0' + (\num % 10), %al
    outb %al, %dx
    movb $'\n', %al
    outb %al, %dx
    popq %rax
    pushq $\num
    push %rax; push %rcx; push %rdx; push %rbx
    push %rbp; push %rsi; push %rdi
    push %r8;  push %r9;  push %r10; push %r11
    push %r12; push %r13; push %r14; push %r15
    mov %rsp, %rdi
    call exc_handler
    pop %r15; pop %r14; pop %r13; pop %r12
    pop %r11; pop %r10; pop %r9;  pop %r8
    pop %rdi; pop %rsi; pop %rbp; pop %rbx
    pop %rdx; pop %rcx; pop %rax
    addq $8, %rsp
    iretq
.endm
.macro EXC_STUB_ERR num
.global exc\num
exc\num:
    /* DEBUG: raw serial "E<num>" — which vector actually fired? */
    pushq %rax
    movw $0x3F8, %dx
    movb $'E', %al
    outb %al, %dx
    movb $'0' + (\num / 10), %al
    outb %al, %dx
    movb $'0' + (\num % 10), %al
    outb %al, %dx
    movb $'\n', %al
    outb %al, %dx
    popq %rax
    pushq $\num
    push %rax; push %rcx; push %rdx; push %rbx
    push %rbp; push %rsi; push %rdi
    push %r8;  push %r9;  push %r10; push %r11
    push %r12; push %r13; push %r14; push %r15
    mov %rsp, %rdi
    call exc_handler
    pop %r15; pop %r14; pop %r13; pop %r12
    pop %r11; pop %r10; pop %r9;  pop %r8
    pop %rdi; pop %rsi; pop %rbp; pop %rbx
    pop %rdx; pop %rcx; pop %rax
    addq $16, %rsp
    iretq
.endm

EXC_STUB 0
EXC_STUB 1
EXC_STUB 2
EXC_STUB 3
EXC_STUB 4
EXC_STUB 5
EXC_STUB 6
EXC_STUB 7
EXC_STUB_ERR 8
EXC_STUB 9
EXC_STUB_ERR 10
EXC_STUB_ERR 11
EXC_STUB_ERR 12
EXC_STUB_ERR 13
EXC_STUB_ERR 14
EXC_STUB 16
EXC_STUB_ERR 17
EXC_STUB 18
EXC_STUB 19

ISR_STUB 20, isr20_handler
ISR_STUB 21, isr21_handler
ISR_STUB 2c, isr2c_handler
ISR_STUB _default, isr_default_handler
