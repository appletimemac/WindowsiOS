// Guest programs for tests/test_excl.c: position independent, copied into guest RAM, registers carry the
// addresses. Every one ends with br x29 to the test's stop address.

.text

// ---- A value another CPU touches between an exclusive load and its store ----
// CPU A: x0 = the cell, x10 = the flags, x2 (and x3) = what it tries to store, x16 (x17) = what it stores in a
// second try, with a load of its own, once B is done.
//   flags+0: A has loaded; +8: B is done; +16: A's status; +24, +32: what A's load saw; +40: the second try's
//   status; +48, +56: the cell as the first try left it.
// CPU B: x0 = the cell, x10 = the flags, x6 (x7) = another value, x12 (x13) = the cell's first value.
//   x15 = 1: change it and change it back, with exclusive stores; 2: change it and leave it, with one;
//   3: change it with a plain store; 0: leave it alone.

.macro ABA_A name, ld, st, r, pair, pl
    .p2align 2
    .globl _\name
    .globl _\name\()_end
_\name:
.if \pair
    \ld \r\()8, \r\()9, [x0]
.else
    \ld \r\()8, [x0]
    mov x9, #0
.endif
    mov x4, #1
    str x4, [x10]
1:  ldr x5, [x10, #8]
    cbz x5, 1b
.if \pair
    \st w11, \r\()2, \r\()3, [x0]
.else
    \st w11, \r\()2, [x0]
.endif
    str x11, [x10, #16]
    stp x8, x9, [x10, #24]
    mov x21, #0
.if \pair
    \pl \r\()20, \r\()21, [x0]
.else
    \pl \r\()20, [x0]
.endif
    stp x20, x21, [x10, #48]
.if \pair
    \ld \r\()8, \r\()9, [x0]
    \st w12, \r\()16, \r\()17, [x0]
.else
    \ld \r\()8, [x0]
    \st w12, \r\()16, [x0]
.endif
    str x12, [x10, #40]
    br x29
_\name\()_end:
.endm

.macro ABA_B name, ld, st, r, pair, ps
    .p2align 2
    .globl _\name
    .globl _\name\()_end
_\name:
1:  ldr x5, [x10]
    cbz x5, 1b
    cbz x15, 4f
    cmp x15, #3
    b.eq 5f
.if \pair
2:  \ld \r\()8, \r\()9, [x0]
    \st w11, \r\()6, \r\()7, [x0]
.else
2:  \ld \r\()8, [x0]
    \st w11, \r\()6, [x0]
.endif
    cbnz w11, 2b
    cmp x15, #2
    b.eq 4f
.if \pair
3:  \ld \r\()8, \r\()9, [x0]
    \st w11, \r\()12, \r\()13, [x0]
.else
3:  \ld \r\()8, [x0]
    \st w11, \r\()12, [x0]
.endif
    cbnz w11, 3b
    b 4f
.if \pair
5:  \ps \r\()6, \r\()7, [x0]
.else
5:  \ps \r\()6, [x0]
.endif
4:  mov x4, #1
    str x4, [x10, #8]
    br x29
_\name\()_end:
.endm

ABA_A excl_a_p64, ldxp, stxp, x, 1, ldp
ABA_B excl_b_p64, ldxp, stxp, x, 1, stp
ABA_A excl_a_p32, ldxp, stxp, w, 1, ldp
ABA_B excl_b_p32, ldxp, stxp, w, 1, stp
ABA_A excl_a_x, ldxr, stxr, x, 0, ldr
ABA_B excl_b_x, ldxr, stxr, x, 0, str
ABA_A excl_a_w, ldaxr, stlxr, w, 0, ldr
ABA_B excl_b_w, ldxr, stxr, w, 0, str
ABA_A excl_a_h, ldxrh, stxrh, w, 0, ldrh
ABA_B excl_b_h, ldxrh, stxrh, w, 0, strh
ABA_A excl_a_b, ldxrb, stxrb, w, 0, ldrb
ABA_B excl_b_b, ldaxrb, stlxrb, w, 0, strb

// ---- One CPU, the forms and registers a store can have ----
// x0 = the cell, x10 = results (8 bytes each). Each step loads, stores and notes the status.
    .p2align 2
    .globl _excl_forms
    .globl _excl_forms_end
_excl_forms:
    ldxp x8, x9, [x0]               // 0: the pair from x1, x2 (x1 is one of the registers the store borrows)
    mov x1, #0x1111
    mov x2, #0x2222
    stxp w11, x1, x2, [x0]
    str x11, [x10]
    ldxp x8, x9, [x0]               // 1: both zero registers (how Windows empties a list)
    stxp w12, xzr, xzr, [x0]
    str x12, [x10, #8]
    stp x8, x9, [x10, #16]          // 2, 3: what that load saw
    mov x6, #0x6666                 // 4: from registers the translator keeps in memory
    mov x7, #0x7777
    ldxp x16, x17, [x0]
    stxp w15, x6, x7, [x0]
    str x15, [x10, #32]
    stp x16, x17, [x10, #88]        // 11, 12: that load saw the two zeros
    ldxp x8, x9, [x0]               // 5: a store with no load before it fails (CLREX in between)
    clrex
    stxp w11, x1, x2, [x0]
    str x11, [x10, #40]
    ldxp x8, x9, [x0]               // 6: a store of another size than the load fails
    stxp w11, w1, w2, [x0]
    str x11, [x10, #48]
    add x4, x0, #16                 // 7: a single, to the second cell
    ldxr x8, [x4]
    mov x3, #0x3333
    stxr w11, x3, [x4]
    str x11, [x10, #56]
    ldxr x8, [x4]                   // 8: a store to another address than the load fails
    stxr w11, x3, [x0]
    str x11, [x10, #64]
    add x5, x0, #17                 // 9: one byte in the middle of the second cell
    ldxrb w8, [x5]
    mov w3, #0xab
    stxrb w11, w3, [x5]
    str x11, [x10, #72]
    ldr x8, [x4]
    str x8, [x10, #120]             // 15: the second cell with that byte in it
    ldxr x8, [x4]                   // 10: two stores after one load: the second fails
    stxr w11, x3, [x4]
    stxr w12, x3, [x4]
    str x12, [x10, #80]
    str x11, [x10, #104]            // 13: and the first did not
    ldr x8, [x4]
    str x8, [x10, #112]             // 14: what it wrote
    ldxr x8, [x4]                   // the status thrown away (this form is done in C)
    mov x3, #0x4444
    stxr wzr, x3, [x4]
    ldxp w8, w9, [x4]               // 16: a pair's store after a single's load of the same 8 bytes fails (a pair's
    ldxr x8, [x4]                   // load before it, so that everything but the form says the store may go)
    stxp w11, w1, w2, [x4]
    str x11, [x10, #128]
    ldxp w8, w9, [x4]               // 17: and a single's store after a pair's load
    stxr w11, x1, [x4]
    str x11, [x10, #136]
    br x29
_excl_forms_end:

// ---- Windows' interlocked list, as its kernel pops and pushes (the two routines are copied from a dump) ----
// x0 = the list's head {depth, first entry}, x20 = rounds to do, x21 = this CPU's mark (not 0), x22 = the poison
// entry, x23 = how long this CPU dawdles between its load and its store, x24 = its results {rounds done, what went
// wrong, the entry}, x25 = the flag that stops everyone.
// An entry is {next, owner}. A CPU that holds an entry writes its mark into owner and, as the owner of a block
// would, its own data over next (the poison entry's address, so a list that takes that for a link is caught and
// not followed into nowhere).
    .p2align 2
    .globl _excl_slist
    .globl _excl_slist_end
_excl_slist:
0:  ldr x4, [x25]
    cbnz x4, 9f
1:  ldxp x8, x9, [x0]               // pop
    cbnz x9, 10f
    ldr x4, [x25]                   // empty (with eight entries and four CPUs, only once the list is broken)
    cbnz x4, 9f
    b 1b
10: ldr x10, [x9]
    mov x5, x23
2:  cbz x5, 3f
    sub x5, x5, #1
    b 2b
3:  sub x8, x8, #1
    stxp w11, x8, x10, [x0]
    cbnz w11, 1b
    cmp x9, x22
    b.eq 6f
    ldr x4, [x9, #8]
    cbnz x4, 7f
    str x21, [x9, #8]
    str x22, [x9]
    ldr x4, [x9, #8]
    cmp x4, x21
    b.ne 8f
    str xzr, [x9, #8]
4:  ldr x12, [x0, #8]               // push
    str x12, [x9]
    dmb ish
    ldxp x13, x14, [x0]
    cmp x12, x14
    b.ne 4b
    add x13, x13, #1
    stxp w11, x13, x9, [x0]
    cbnz w11, 4b
    ldr x4, [x24]
    add x4, x4, #1
    str x4, [x24]
    subs x20, x20, #1
    b.ne 0b
9:  br x29
6:  mov x4, #1                      // the list handed out an owner's data as an entry
    b 5f
7:  mov x4, #2                      // the list handed out an entry someone holds
    b 5f
8:  mov x4, #3                      // someone took the entry this CPU holds
5:  str x4, [x24, #8]
    str x9, [x24, #16]
    mov x5, #1
    str x5, [x25]
    br x29
_excl_slist_end:
