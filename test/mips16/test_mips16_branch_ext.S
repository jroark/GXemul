/*
 *  MIPS16 extended-branch regression tests.
 *
 *  Expected output: ABCDE
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	jalx	test_beqz_ext_bits
	nop
	jalx	test_bnez_ext_bits
	nop
	jalx	test_bteqz_ext_bits
	nop
	jalx	test_btnez_ext_bits
	nop
	jalx	test_ext_b_base
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


	.set mips16

/*
 *  These raw encodings use valid gas-emitted EXTEND words with the
 *  base-word bits [7:5] deliberately poisoned. Correct decode must use
 *  only base imm[4:0], ignoring those bits.
 */

test_beqz_ext_bits:
	li	$a1, 0
branch_beqz:
	.hword	0xf1a0, 0x257c
	.space	0x378
beqz_target:
	li	$v0, 'A'
	sw	$v0, 0($s0)
	jrc	$ra
	.space	(0x43c - (. - branch_beqz))
beqz_wrong:
	li	$v0, '!'
	sw	$v0, 0($s0)
	jrc	$ra

test_bnez_ext_bits:
	li	$a1, 1
branch_bnez:
	.hword	0xf1a0, 0x2d7c
	.space	0x378
bnez_target:
	li	$v0, 'B'
	sw	$v0, 0($s0)
	jrc	$ra
	.space	(0x43c - (. - branch_bnez))
bnez_wrong:
	li	$v0, '!'
	sw	$v0, 0($s0)
	jrc	$ra

test_bteqz_ext_bits:
	li	$v0, 1
	li	$v1, 1
	cmp	$v0, $v1
branch_bteqz:
	.hword	0xf1a0, 0x607c
	.space	0x378
bteqz_target:
	li	$v0, 'C'
	sw	$v0, 0($s0)
	jrc	$ra
	.space	(0x43c - (. - branch_bteqz))
bteqz_wrong:
	li	$v0, '!'
	sw	$v0, 0($s0)
	jrc	$ra

test_btnez_ext_bits:
	li	$v0, 1
	li	$v1, 0
	cmp	$v0, $v1
branch_btnez:
	.hword	0xf1a0, 0x617c
	.space	0x378
btnez_target:
	li	$v0, 'D'
	sw	$v0, 0($s0)
	jrc	$ra
	.space	(0x43c - (. - branch_btnez))
btnez_wrong:
	li	$v0, '!'
	sw	$v0, 0($s0)
	jrc	$ra

test_ext_b_base:
	li	$v0, '?'
	b	base_target
	.rept 1100
	break	0
	.endr
base_wrong:
	sw	$v0, 0($s0)
	jrc	$ra
base_target:
	li	$v0, 'E'
	sw	$v0, 0($s0)
	jrc	$ra

	.set nomips16
