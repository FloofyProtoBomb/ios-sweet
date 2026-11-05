/*
 * First userspace program for SWEET bring-up: mockfs serves the RAM disk
 * itself as /sbin/launchd, so this static binary is PID 1. It proves that
 * exec, the user/kernel boundary, devfs and the console all work, then
 * stays alive (init exiting panics the kernel).
 *
 * tools/mkinit.py wraps the assembled __text into a static Mach-O, so this
 * must stay position independent with no relocations and no writable data:
 * every reference is adr (PC-relative), the PID digit lives on the stack.
 *
 * Raw XNU syscalls: number in x16, svc #0x80, carry set on error.
 */
#define SYS_write	4
#define SYS_open	5
#define SYS_getpid	20
#define SYS_select	93
#define O_RDWR		2
#ifndef INIT_BOARD
#define INIT_BOARD "SWEET"
#endif

	.text
	.globl	_start
	.p2align 2
_start:
	/*
	 * The kernel enters a static binary with sp at argc, aligned only to
	 * what the argument/apple strings happened to need. Using a misaligned
	 * sp as a base register raises an SP alignment fault (SIGBUS) on real
	 * hardware (QEMU does not check), so align it like crt0/dyld do.
	 */
	mov	x9, sp
	and	x9, x9, #~15
	mov	sp, x9

	adr	x0, path
	mov	x1, #O_RDWR
	mov	x2, #0
	mov	x16, #SYS_open
	svc	#0x80
	b.cs	hang			/* no console: nothing to report to */
	mov	x19, x0
#ifdef DRE_CHECK_NATIVE_UFS
	mov	w28, #0
#endif
#ifdef DRE_CHECK_UI_SERVICES
	mov	w27, #0
#endif

	adr	x1, hello
	adr	x2, hello_tail
	sub	x2, x2, x1
	bl	write_console

	mov	x16, #SYS_getpid
	svc	#0x80
	add	w0, w0, #'0'		/* PID 1 expected: one digit */
	sub	sp, sp, #16
	strb	w0, [sp]
	mov	x1, sp
	mov	x2, #1
	bl	write_console
	add	sp, sp, #16

	adr	x1, hello_tail
	adr	x2, alive
	sub	x2, x2, x1
	bl	write_console

#ifdef DRE_CHECK_IDENTIFIERS
	bl	Ldre_check_ids
#endif
tick:
#if defined(DRE_EXPECT_CPUS) && !defined(DRE_BOARD_TESTS)
	bl	Ldre_check_cpus
#endif
#if defined(DRE_CHECK_UI_SERVICES) && !defined(DRE_BOARD_TESTS)
	bl	Ldre_check_ui
#endif
#ifdef DRE_BOARD_TESTS
	bl	Ldre_print_report
#endif
#if defined(DRE_CHECK_NATIVE_UFS) && !defined(DRE_BOARD_TESTS)
	bl	Ldre_check_native
#endif
#ifdef DRE_CHECK_NATIVE_UFS
	bl	Lnative_fault_status
#endif
#if defined(DRE_EXPECT_RAM_MIB) && !defined(DRE_BOARD_TESTS)
	bl	Ldre_check_memory
#endif
	/* select(0, NULL, NULL, NULL, &5s) as a portable sleep */
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	adr	x4, five_sec
	mov	x16, #SYS_select
	svc	#0x80

	adr	x1, alive
	adr	x2, alive_end
	sub	x2, x2, x1
	bl	write_console
	b	tick

/* write(x19, x1, x2) */
write_console:
	mov	x0, x19
	mov	x16, #SYS_write
	svc	#0x80
	ret

hang:
	b	hang

	.p2align 3
five_sec:
	.quad	5			/* tv_sec */
	.quad	0			/* tv_usec (int32 + padding) */
path:
	.asciz	"/dev/console"
hello:
	.ascii	"\n*** "
	.ascii	INIT_BOARD
	.ascii	" userspace: init running as PID "
hello_tail:
	.ascii	" ***\n"
alive:
	.ascii	INIT_BOARD
	.ascii	" init: still alive\n"
alive_end:
#ifdef DRE_CHECK_IDENTIFIERS
#include "../devices/dre/identity-check.inc"
#endif
#ifdef DRE_EXPECT_CPUS
#include "../devices/dre/cpu-check.inc"
#endif
#ifdef DRE_CHECK_UI_SERVICES
#include "../devices/dre/ui-check.inc"
#endif
#ifdef DRE_CHECK_NATIVE_UFS
#include "../devices/dre/native-check.inc"

/* Print the three typed snapshot rows if this kernel has a captured fault.
 * Old kernels and successful storage probes have no rows to report. */
	.p2align 2
Lnative_fault_status:
	stp	x29, x30, [sp, #-16]!
	sub	sp, sp, #272
	mov	x8, #256
	str	x8, [sp]
	adr	x0, Lnative_fault_key
	mov	x1, #(Lnative_fault_key_end - Lnative_fault_key)
	add	x2, sp, #16
	mov	x3, sp
	mov	x4, #0
	mov	x5, #0
	mov	x16, #274		/* sysctlbyname, read only */
	svc	#0x80
	b.cs	Lnative_fault_done
	ldr	x2, [sp]
	cmp	x2, #2
	b.lo	Lnative_fault_done
	cmp	x2, #256
	b.hi	Lnative_fault_done
	sub	x2, x2, #1		/* Exclude the required terminator. */
	add	x1, sp, #16
	ldrb	w8, [x1, x2]
	cbnz	w8, Lnative_fault_done
	adr	x13, Lnative_fault_prefix
	mov	x9, #0
	mov	w10, #'1'
Lnative_fault_row:
	sub	x8, x2, x9
	cmp	x8, #12		/* Prefix, digit, '=', data byte, newline. */
	b.lo	Lnative_fault_done
	mov	x12, #0
Lnative_fault_check_prefix:
	ldrb	w8, [x1, x9]
	ldrb	w11, [x13, x12]
	cmp	w8, w11
	b.ne	Lnative_fault_done
	add	x9, x9, #1
	add	x12, x12, #1
	cmp	x12, #8
	b.lo	Lnative_fault_check_prefix
	ldrb	w8, [x1, x9]
	cmp	w8, w10
	b.ne	Lnative_fault_done
	add	x9, x9, #1
	ldrb	w8, [x1, x9]
	cmp	w8, #'='
	b.ne	Lnative_fault_done
	add	x9, x9, #1
	mov	x11, x9
Lnative_fault_body:
	ldrb	w8, [x1, x9]
	cmp	w8, #10
	b.eq	Lnative_fault_end_row
	cmp	w8, #32
	b.lo	Lnative_fault_done
	cmp	w8, #126
	b.hi	Lnative_fault_done
	add	x9, x9, #1
	cmp	x9, x2
	b.lo	Lnative_fault_body
	b	Lnative_fault_done
Lnative_fault_end_row:
	cmp	x9, x11
	b.eq	Lnative_fault_done
	add	x9, x9, #1
	add	w10, w10, #1
	cmp	w10, #'4'
	b.lo	Lnative_fault_row
	cmp	x9, x2
	b.ne	Lnative_fault_done
	bl	write_console
Lnative_fault_done:
	add	sp, sp, #272
	ldp	x29, x30, [sp], #16
	ret
Lnative_fault_key: .ascii "kern.dre_ufs_fault"
Lnative_fault_key_end:
Lnative_fault_prefix: .ascii "NATIVE_F"
#endif
#ifdef DRE_EXPECT_RAM_MIB
#include "../devices/dre/memory-check.inc"
#endif
