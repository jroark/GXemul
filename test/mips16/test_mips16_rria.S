/*
 *  MIPS16 RRIA / ADDIU3 EXTEND regression test.
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
	li	$v0, 'A' - 4660
	jalx	test_rria_4660
	nop

	li	$v0, 'B' - 291
	jalx	test_rria_291
	nop

	li	$v0, 'C' + 16
	jalx	test_rria_neg16
	nop

	li	$v0, 'D' + 200
	jalx	test_rria_neg200
	nop

	li	$v0, 'E' - 16383
	jalx	test_rria_pos_edge
	nop

	li	$v0, 'F' + 16384
	jalx	test_rria_neg_edge
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


	.set mips16

/*
 *  gas-emitted RRIA cases chosen to cover:
 *    - non-zero EXTEND[3:0]
 *    - non-zero EXTEND[10:4]
 *    - positive values above 1023
 *    - negative values and sign edges
 */
test_rria_4660:
	addiu	$v1, $v0, 4660
	sw	$v1, 0($s0)
	jrc	$ra

test_rria_291:
	addiu	$v1, $v0, 291
	sw	$v1, 0($s0)
	jrc	$ra

test_rria_neg16:
	addiu	$v1, $v0, -16
	sw	$v1, 0($s0)
	jrc	$ra

test_rria_neg200:
	addiu	$v1, $v0, -200
	sw	$v1, 0($s0)
	jrc	$ra

test_rria_pos_edge:
	addiu	$v1, $v0, 16383
	sw	$v1, 0($s0)
	jrc	$ra

test_rria_neg_edge:
	addiu	$v1, $v0, -16384
	sw	$v1, 0($s0)

	jrc	$ra

	.set nomips16
