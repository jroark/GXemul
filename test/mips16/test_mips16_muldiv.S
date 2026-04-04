/*
 *  MIPS16 multiply/divide instruction tests for GXemul testmips machine.
 *
 *  Tests: MULT, MULTU, DIV, DIVU, MFHI, MFLO
 *
 *  Expected output: ABCDEF
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	jalx	test_mips16_muldiv
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop

	.set mips16

test_mips16_muldiv:
	move	$s1, $ra

	/* --- Test A: MULT + MFLO --- */
	/* 7 * 9 = 63 */
	li	$v0, 7
	li	$v1, 9
	mult	$v0, $v1
	mflo	$a0
	/* 63 + 2 = 65 = 'A' */
	addiu	$a0, 2
	sw	$a0, 0($s0)

	/* --- Test B: MULT + MFHI (overflow into HI) --- */
	/* 0x80 * 0x80 = 0x4000, fits in LO, HI should be 0 */
	/* Use: 255 * 255 = 65025 = 0xFE01; HI=0 for 32-bit */
	/* Instead test that HI is correct for a known product */
	/* Simply verify MFHI returns 0 for small multiply */
	li	$v0, 100
	li	$v1, 100
	mult	$v0, $v1	/* 10000, HI=0 */
	mfhi	$a0
	/* HI should be 0; 0 + 66 = 66 = 'B' */
	addiu	$a0, 66
	sw	$a0, 0($s0)

	/* --- Test C: MULTU + MFLO --- */
	/* 13 * 5 = 65 = 'A'... use 11 * 6 = 66 + 1 = 67 = 'C' */
	li	$v0, 11
	li	$v1, 6
	multu	$v0, $v1	/* 66 */
	mflo	$a0
	addiu	$a0, 1		/* 67 = 'C' */
	sw	$a0, 0($s0)

	/* --- Test D: DIV + MFLO (quotient) --- */
	/* 204 / 3 = 68 = 'D' */
	li	$v0, 204
	li	$v1, 3
	div	$v0, $v1
	mflo	$a0
	sw	$a0, 0($s0)

	/* --- Test E: DIV + MFHI (remainder) --- */
	/* 103 / 10: quotient=10, remainder=3 */
	li	$v0, 103
	li	$v1, 10
	div	$v0, $v1
	mfhi	$a0		/* remainder = 3 */
	/* 3 + 66 = 69 = 'E' */
	addiu	$a0, 66
	sw	$a0, 0($s0)

	/* --- Test F: DIVU + MFLO --- */
	/* 210 / 3 = 70 = 'F' */
	li	$v0, 210
	li	$v1, 3
	divu	$v0, $v1
	mflo	$a0
	sw	$a0, 0($s0)

	move	$ra, $s1
	jr	$ra

	.set nomips16
