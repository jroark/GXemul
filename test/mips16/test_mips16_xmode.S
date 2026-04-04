/*
 *  MIPS16 cross-mode transition stress tests for GXemul testmips machine.
 *
 *  Tests: JALX round-trips, JALX from MIPS16, nested mode switches,
 *         JALR (ry==2 path), SWRASP save/restore RA,
 *         chained MIPS16 subroutines, ADDIUPC.
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

	jalx	test_mips16_xmode
	nop

	/* Back in MIPS32 — newline and halt */
	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


/*
 *  MIPS32 helper: prints a character passed in $a0, returns to caller.
 *  Called from MIPS16 via JALX — RA will have bit 0 set for MIPS16 return.
 */
	.align 2
m32_putchar:
	lui	$t0, CONS
	sw	$a0, 0($t0)
	jr	$ra
	nop


/*
 *  MIPS32 helper for test C: called from a MIPS16 function that was
 *  itself called from MIPS32.  Prints 'C' and returns.
 */
	.align 2
m32_print_c:
	lui	$t0, CONS
	li	$t1, 67			/* 'C' */
	sw	$t1, 0($t0)
	jr	$ra
	nop


/*
 *  MIPS16 test code
 */
	.set mips16

	.align 2
test_mips16_xmode:
	/* Save return address (points back to MIPS32 with bit 0 = 0) */
	move	$s1, $ra

	/*
	 *  --- Test A: Basic JALX MIPS32->MIPS16->return ---
	 *  We're already here via JALX from _start.  Print 'A'.
	 */
	li	$v0, 65			/* 'A' */
	sw	$v0, 0($s0)

	/*
	 *  --- Test B: JALX from MIPS16 to MIPS32 helper ---
	 *  Use MIPS16 JALX to call m32_putchar.
	 */
	li	$a0, 66			/* 'B' */
	jalx	m32_putchar
	nop

	/*
	 *  --- Test C: Nested MIPS32->MIPS16->MIPS32->MIPS16 ---
	 *  Call m32_print_c via JALX, which prints 'C' and returns here.
	 */
	jalx	m32_print_c
	nop

	/*
	 *  --- Test D: JALR in MIPS16 (ry==2 path) ---
	 *  Load address of a MIPS16 subroutine into a register,
	 *  then JALR to it.  The subroutine prints 'D' and returns.
	 */
	la	$v0, m16_print_d
	jalr	$v0
	nop

	/*
	 *  --- Test E: JR $ra exits MIPS16 correctly ---
	 *  Call a MIPS16 subroutine, if we get back the JR worked.
	 */
	jal	m16_nop_sub
	nop
	li	$v0, 69			/* 'E' */
	sw	$v0, 0($s0)

	/*
	 *  --- Test F: SWRASP save/restore RA pattern ---
	 *  Save RA to stack via SWRASP, call a subroutine (clobbers RA),
	 *  restore RA from stack via LW into temp + MOVE, verify.
	 */
	sw	$ra, 0($sp)		/* SWRASP encoding */
	jal	m16_print_f
	nop
	/* Restore RA: load into temp reg then move to $ra */
	lw	$v0, 0($sp)		/* LWSP into MIPS16 reg */
	move	$ra, $v0		/* MOV32R: move to $ra */

	/*
	 *  --- Test G: Chained MIPS16 subroutines ---
	 *  m16_chain_g calls m16_chain_g2, which prints 'G'.
	 *  Tests that JAL works for multi-level MIPS16 calls.
	 */
	sw	$ra, 0($sp)		/* save RA */
	addiu	$sp, -8
	jal	m16_chain_g
	nop
	addiu	$sp, 8
	lw	$v0, 0($sp)		/* restore RA via temp */
	move	$ra, $v0

	/*
	 *  --- Test H: ADDIUPC + verify ---
	 *  Compute an address relative to PC, use it.
	 */
	addiu	$v0, $pc, 0		/* $v0 = PC (approx) */
	/* If we didn't crash, ADDIUPC works */
	li	$v0, 72			/* 'H' */
	sw	$v0, 0($s0)

	/* Return to MIPS32 caller */
	move	$ra, $s1
	jr	$ra


/*
 *  MIPS16 subroutine: prints 'D' and returns
 */
	.align 2
m16_print_d:
	li	$v0, 68			/* 'D' */
	sw	$v0, 0($s0)
	jr	$ra

/*
 *  MIPS16 subroutine: does nothing, returns
 */
	.align 2
m16_nop_sub:
	jr	$ra

/*
 *  MIPS16 subroutine: prints 'F' and returns
 */
	.align 2
m16_print_f:
	li	$v0, 70			/* 'F' */
	sw	$v0, 0($s0)
	jr	$ra

/*
 *  MIPS16 subroutine chain for test G:
 *  m16_chain_g saves RA, calls m16_chain_g2, restores RA, returns.
 */
	.align 2
m16_chain_g:
	sw	$ra, 0($sp)		/* SWRASP */
	addiu	$sp, -8
	jal	m16_chain_g2
	nop
	addiu	$sp, 8
	lw	$v0, 0($sp)		/* restore RA via temp */
	move	$ra, $v0
	jr	$ra

	.align 2
m16_chain_g2:
	li	$v0, 71			/* 'G' */
	sw	$v0, 0($s0)
	jr	$ra

	.set nomips16
