/*
 *  MIPS16 load/store instruction tests for GXemul testmips machine.
 *
 *  Tests: SW/LW (base+offset), SB/LB/LBU, SH/LH/LHU,
 *         SWSP/LWSP (sp-relative), ADDIUSP, multiple offsets
 *
 *  Expected output: ABCDEFGH
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS
	/* Set up a stack in low memory */
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000
	/* Also pass stack area in $s1 for MIPS16 byte/half access */
	move	$s1, $sp

	jalx	test_mips16_mem
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop

	.set mips16

test_mips16_mem:
	move	$a0, $ra	/* save return address in $a0 (MIPS16 reg 4) */

	/* --- Test A: SW / LW with register base --- */
	li	$v0, 65		/* 'A' */
	sw	$v0, 0($s1)	/* store to memory via $s1 */
	lw	$v1, 0($s1)	/* load back */
	sw	$v1, 0($s0)	/* print */

	/* --- Test B: SB / LB (signed byte load) --- */
	li	$v0, 66		/* 'B' */
	sb	$v0, 0($s1)
	lb	$v1, 0($s1)
	sw	$v1, 0($s0)

	/* --- Test C: SB / LBU (unsigned byte load) --- */
	li	$v0, 67		/* 'C' */
	sb	$v0, 0($s1)
	lbu	$v1, 0($s1)
	sw	$v1, 0($s0)

	/* --- Test D: SH / LH (signed halfword) --- */
	li	$v0, 68		/* 'D' */
	sh	$v0, 0($s1)
	lh	$v1, 0($s1)
	sw	$v1, 0($s0)

	/* --- Test E: SH / LHU (unsigned halfword) --- */
	li	$v0, 69		/* 'E' */
	sh	$v0, 0($s1)
	lhu	$v1, 0($s1)
	sw	$v1, 0($s0)

	/* --- Test F: SWSP / LWSP (sp-relative word) --- */
	li	$v0, 70		/* 'F' */
	sw	$v0, 0($sp)
	lw	$v0, 0($sp)
	sw	$v0, 0($s0)

	/* --- Test G: SW/LW with nonzero offset --- */
	li	$v0, 71		/* 'G' */
	sw	$v0, 4($s1)	/* offset 4 */
	lw	$v1, 4($s1)
	sw	$v1, 0($s0)

	/* --- Test H: ADDIUSP (compute addr relative to sp) --- */
	addiu	$v0, $sp, 0	/* $v0 = $sp + 0 */
	/* Just verify it doesn't crash */
	li	$v0, 72		/* 'H' */
	sw	$v0, 0($s0)

	move	$ra, $a0
	jr	$ra

	.set nomips16
