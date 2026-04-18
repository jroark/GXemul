/*
 *  MIPS16 branch and jump instruction tests for GXemul testmips machine.
 *
 *  Tests: B, BEQZ, BNEZ, JAL (MIPS16-to-MIPS16), JR
 *
 *  Note: JALX (MIPS16-to-MIPS32 round-trip) is tested separately in
 *  test_mips16.S. This test only exercises intra-MIPS16 control flow.
 *
 *  Expected output: ABCDEFG
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	jalx	test_mips16_branch
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


	.set mips16

test_mips16_branch:
	move	$s1, $ra

	/* --- Test A: B (unconditional branch) --- */
	b	test_a_target
test_a_fail:
	li	$v0, '?'
	sw	$v0, 0($s0)
	b	test_b
test_a_target:
	li	$v0, 65		/* 'A' */
	sw	$v0, 0($s0)

	/* --- Test B: BEQZ taken --- */
test_b:
	li	$v0, 0
	beqz	$v0, test_b_pass
	li	$v0, '?'
	sw	$v0, 0($s0)
	b	test_c
test_b_pass:
	li	$v0, 66		/* 'B' */
	sw	$v0, 0($s0)

	/* --- Test C: BEQZ not taken --- */
test_c:
	li	$v0, 1
	beqz	$v0, test_c_fail
	li	$v0, 67		/* 'C' */
	sw	$v0, 0($s0)
	b	test_d
test_c_fail:
	li	$v0, '?'
	sw	$v0, 0($s0)

	/* --- Test D: BNEZ taken --- */
test_d:
	li	$v0, 1
	bnez	$v0, test_d_pass
	li	$v0, '?'
	sw	$v0, 0($s0)
	b	test_e
test_d_pass:
	li	$v0, 68		/* 'D' */
	sw	$v0, 0($s0)

	/* --- Test E: BNEZ not taken --- */
test_e:
	li	$v0, 0
	bnez	$v0, test_e_fail
	li	$v0, 69		/* 'E' */
	sw	$v0, 0($s0)
	b	test_f
test_e_fail:
	li	$v0, '?'
	sw	$v0, 0($s0)

	/* --- Test F: JAL within MIPS16 --- */
test_f:
	jal	mips16_sub
	nop
	/* mips16_sub printed 'F', returned here */

	/* --- Test G: final check --- */
	li	$v0, 71		/* 'G' */
	sw	$v0, 0($s0)

	/* Return to MIPS32 caller */
	move	$ra, $s1
	jr	$ra


/*
 *  MIPS16 subroutine: prints 'F' and returns
 */
	.align 2
mips16_sub:
	li	$v0, 70		/* 'F' */
	sw	$v0, 0($s0)
	jr	$ra

	.set nomips16
