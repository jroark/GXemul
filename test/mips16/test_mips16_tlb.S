/*
 *  MIPS16 TLB refill / ERET regression test for GXemul testmips.
 *
 *  Expected output: ABCD
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	b	safe_start
	nop

	/*
	 *  Keep all executable test code above the copied vector ranges.
	 *  The refill handler is copied to PA 0x00000000-0x0000007f and the
	 *  general handler to PA 0x00000180-0x000001ff, which alias this
	 *  program's initial kseg1 text page.
	 */
	.align 4
	.space	0x200

safe_start:
	lui	$s0, CONS
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	/* Scratch page in uncached kseg1, mirrored via the test TLB entry. */
	lui	$s1, 0xa000
	ori	$s1, $s1, 0x4000
	li	$t0, 66			/* 'B' */
	sw	$t0, 0($s1)

	/* Install the same handler at the refill and general vectors. */
	la	$t0, tlb_handler
	li	$t1, 0x80000000
	li	$t2, 32
	jal	copy_words
	nop

	la	$t0, tlb_handler
	li	$t1, 0x80000180
	li	$t2, 32
	jal	copy_words
	nop

	/* Route exceptions to kseg0 vectors. */
	mfc0	$t0, $12
	li	$t1, 0xffbfffff
	and	$t0, $t0, $t1
	mtc0	$t0, $12
	nop
	nop

	jalx	test_mips16_tlb
	nop

	lui	$t0, CONS
	li	$t1, 68			/* 'D' */
	sw	$t1, 0($t0)
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop

copy_words:
	lw	$t3, 0($t0)
	sw	$t3, 0($t1)
	addiu	$t0, 4
	addiu	$t1, 4
	addiu	$t2, -1
	bnez	$t2, copy_words
	nop
	jr	$ra
	nop


	.align 4
tlb_handler:
	/* Print 'A' when the refill handler runs. */
	lui	$k1, CONS
	li	$t0, 65			/* 'A' */
	sw	$t0, 0($k1)

	/* Map user VA 0x00004000 -> PA 0x00004000 as an even 4KB page. */
	li	$k0, 0x00004000
	mtc0	$k0, $10		/* EntryHi */
	li	$k0, 0x00000107
	mtc0	$k0, $2			/* EntryLo0 */
	mtc0	$zero, $3		/* EntryLo1 */
	mtc0	$zero, $5		/* PageMask = 4KB pages */
	mtc0	$zero, $0		/* Index */
	nop
	nop
	tlbwi
	nop
	eret


	.set mips16

test_mips16_tlb:
	move	$a1, $ra

	/* User-space VA that will fault, refill, then succeed on retry. */
	li	$v0, 0x4000
	lw	$v1, 0($v0)
	sw	$v1, 0($s0)

	li	$v1, 67			/* 'C' */
	sw	$v1, 4($v0)
	lw	$a0, 4($v0)
	sw	$a0, 0($s0)

	move	$ra, $a1
	jr	$ra

	.set nomips16
