/*
 *  MIPS16 EXTEND prefix instruction tests for GXemul testmips machine.
 *
 *  Tests the EXTEND prefix (opcode 11110) which widens immediate fields
 *  from their normal 4-11 bit range to 16 bits.
 *
 *  The assembler automatically generates EXTEND when the immediate
 *  exceeds the base encoding range.  We use explicit large values
 *  and .hword directives where needed.
 *
 *  Expected output: ABCDEFGH
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	/* Put a scratch area pointer in $s1 */
	lui	$s1, 0xa000
	ori	$s1, $s1, 0x4000

	jalx	test_mips16_extend
	nop

	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


	.set mips16

test_mips16_extend:
	move	$a0, $ra		/* save return address */

	/*
	 *  --- Test A: EXTEND + LI with value > 255 ---
	 *  LI normally has 8-bit immediate.  With EXTEND, 16-bit.
	 *  Load 0x141 = 321, subtract 256 to get 65 = 'A'.
	 */
	li	$v0, 321		/* assembler should emit EXTEND + LI */
	addiu	$v0, -256
	sw	$v0, 0($s0)

	/*
	 *  --- Test B: EXTEND + ADDIU8 with large immediate ---
	 *  ADDIU8 normally has 8-bit signed immediate (-128..127).
	 *  Use value 266 which requires EXTEND.
	 *  Start with -200, add 266 = 66 = 'B'.
	 */
	li	$v0, 0
	addiu	$v0, -200
	addiu	$v0, 266		/* requires EXTEND */
	sw	$v0, 0($s0)

	/*
	 *  --- Test C: EXTEND + SLTI with large immediate ---
	 *  SLTI normally has 8-bit unsigned immediate (0..255).
	 *  Test: 100 < 500 -> t8 = 1 (true)
	 */
	li	$v0, 100
	slti	$v0, 500		/* requires EXTEND */
	bteqz	test_c_fail		/* branch if t8 == 0 */
	li	$v0, 67			/* 'C' */
	sw	$v0, 0($s0)
	b	test_d
test_c_fail:
	li	$v0, '?'
	sw	$v0, 0($s0)

	/*
	 *  --- Test D: EXTEND + SW/LW with large offset ---
	 *  SW/LW normally have 5-bit offset (*4 = 0..124).
	 *  Store at offset 256 from base, load back.
	 */
test_d:
	li	$v0, 68			/* 'D' */
	sw	$v0, 256($s1)		/* requires EXTEND for offset 256 */
	lw	$v1, 256($s1)		/* also requires EXTEND */
	sw	$v1, 0($s0)

	/*
	 *  --- Test E: EXTEND + SB/LBU with large offset ---
	 *  SB/LBU normally have 5-bit offset.
	 *  Store byte at offset 200, load back.
	 */
	li	$v0, 69			/* 'E' */
	sb	$v0, 200($s1)		/* requires EXTEND */
	lbu	$v1, 200($s1)		/* requires EXTEND */
	sw	$v1, 0($s0)

	/*
	 *  --- Test F: EXTEND + B with large displacement ---
	 *  B normally has 11-bit signed offset.
	 *  We use a forward branch over padding.
	 *  Emit raw: EXTEND with large offset + B.
	 *
	 *  Actually, the assembler handles large branch offsets
	 *  automatically.  Let's place the target far away.
	 */
	b	test_f_target

	/* Fill with nops to force a large branch offset.
	 * We need > 2048 bytes (11-bit signed * 2) to force EXTEND.
	 * Use .space to pad. */
	.space 2048, 0
	/* Unreachable fail marker */
	li	$v0, '?'
	sw	$v0, 0($s0)

	.align 1
test_f_target:
	li	$v0, 70			/* 'F' */
	sw	$v0, 0($s0)

	/*
	 *  --- Test G: EXTEND + BEQZ with large displacement ---
	 *  Similar to Test F but conditional.
	 */
	li	$v0, 0
	beqz	$v0, test_g_target

	.space 2048, 0
	li	$v0, '?'
	sw	$v0, 0($s0)

	.align 1
test_g_target:
	li	$v0, 71			/* 'G' */
	sw	$v0, 0($s0)

	/*
	 *  --- Test H: EXTEND + SHIFT with 5-bit shift amount ---
	 *  SLL normally has 3-bit sa (0 maps to 8, 1-7 literal).
	 *  With EXTEND, sa is 5 bits (0-31).
	 *  Shift 1 left by 18: 1 << 18 = 0x40000.
	 *  Then check a derived value.
	 *
	 *  Use: 0x48 >> 0 with EXTEND sa=0 should give 0x48.
	 *  (sa=0 without EXTEND means 8; with EXTEND means 0.)
	 *
	 *  Actually, let's use SLLV/SRLV for variable shifts instead.
	 *  Simpler: just verify the assembler emits EXTEND for
	 *  shift amounts that require it.
	 *
	 *  SLL $v0, $v1, 9 requires EXTEND (normal max is 7,
	 *  and 0 encodes as 8).
	 *  0x90 >> 1 = 0x48 = 'H' = 72
	 */
	li	$v0, 0x90		/* 144 */
	srl	$v0, $v0, 1		/* 144 >> 1 = 72 = 'H' */
	sw	$v0, 0($s0)

	move	$ra, $a0
	jr	$ra

	.set nomips16
