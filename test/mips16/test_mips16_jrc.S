/*
 *  MIPS16 compact JRC/JALRC regression test.
 *
 *  Expected output: AB
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	jalx	test_mips16_jrc
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


	.set mips16

test_mips16_jrc:
	move	$a0, $ra

	li	$v1, '?'
	la	$v0, jrc_target
	jrc	$v0
	sw	$v1, 0($s0)

jrc_target:
	li	$v1, 65
	sw	$v1, 0($s0)

	li	$s1, 0
	la	$v0, jalrc_target
	jalrc	$v0
after_jalrc:
	addiu	$s1, 1

	move	$ra, $a0
	jr	$ra

jalrc_target:
	li	$v0, 0
	cmp	$s1, $v0
	btnez	jalrc_fail

	move	$v1, $ra
	la	$v0, after_jalrc
	cmp	$v1, $v0
	btnez	jalrc_fail

	li	$v1, 66
	sw	$v1, 0($s0)
	jr	$ra

jalrc_fail:
	li	$v1, '!'
	sw	$v1, 0($s0)
	jr	$ra

	.set nomips16
