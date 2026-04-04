/*
 *  MIPS16 exception handling tests for GXemul testmips machine.
 *
 *  Tests: BREAK exception from MIPS16, EPC bit 0 preservation,
 *         ERET return to MIPS16, register preservation, BTNEZ.
 *
 *  The test installs a general exception handler at 0x80000180 that:
 *   - Checks EPC bit 0 (MIPS16 mode indicator)
 *   - Advances EPC past the BREAK instruction (2 bytes)
 *   - Does ERET to return to MIPS16 mode
 *
 *  Expected output: ABCD
 */

	.set noreorder
	.text
	.globl _start

#define CONS	0xb000

_start:
	lui	$s0, CONS

	/* Set up stack */
	lui	$sp, 0xa000
	ori	$sp, $sp, 0x8000

	/*
	 *  Install exception handler at 0x80000180.
	 *  We copy the handler code there at runtime.
	 */
	la	$t0, exception_handler
	li	$t1, 0x80000180
	/* Copy 32 words (128 bytes) of handler code */
	li	$t2, 32
copy_loop:
	lw	$t3, 0($t0)
	sw	$t3, 0($t1)
	addiu	$t0, 4
	addiu	$t1, 4
	addiu	$t2, -1
	bnez	$t2, copy_loop
	nop

	/* Set up CP0 Status: enable BEV=0 so exceptions go to 0x80000180 */
	mfc0	$t0, $12		/* Status */
	li	$t1, 0xFFBFFFFF		/* clear BEV (bit 22) */
	and	$t0, $t0, $t1
	mtc0	$t0, $12
	nop
	nop

	/*
	 *  Use $s2 as a "test phase" register:
	 *    0 = Test A/B: first BREAK (check EPC bit 0 + ERET return)
	 *    1 = Test C: second BREAK (check register preservation)
	 */
	li	$s2, 0

	/* $s0 = console base, preserved across mode switches */
	jalx	test_mips16_except
	nop

	/* Back in MIPS32 — newline and halt */
	lui	$t0, CONS
	li	$t1, '\n'
	sw	$t1, 0($t0)
	sw	$zero, 0x10($t0)
1:	b	1b
	nop


/*
 *  Exception handler (copied to 0x80000180 at runtime).
 *  Runs in MIPS32 mode (exceptions always clear MIPS16 mode).
 *
 *  For test A: check that EPC bit 0 == 1, print 'A'
 *  For test C: just advance EPC and return (register check is in MIPS16)
 */
	.align 4
exception_handler:
	/* Read EPC */
	mfc0	$k0, $14		/* EPC */
	nop

	/* Check test phase in $s2 */
	bnez	$s2, exc_phase1
	nop

	/* Phase 0: Test A — verify EPC bit 0 is set (was in MIPS16) */
	andi	$k1, $k0, 1
	beqz	$k1, exc_fail
	nop

	/* Print 'A' */
	lui	$k1, CONS
	li	$at, 65			/* 'A' */
	sw	$at, 0($k1)

	/* Advance EPC past the 2-byte BREAK instruction.
	 * EPC has bit 0 set for MIPS16, so actual PC = EPC & ~1.
	 * New EPC = (EPC & ~1) + 2, with bit 0 re-set for MIPS16. */
	ori	$k0, $k0, 1		/* ensure bit 0 stays set */
	addiu	$k0, $k0, 2		/* advance by 2 (BREAK is 16-bit) */
	mtc0	$k0, $14		/* write back EPC */
	nop

	/* Set phase to 1 for next exception */
	li	$s2, 1

	eret

exc_phase1:
	/* Phase 1: Test C — just advance EPC and return */
	/* Print nothing here; MIPS16 code will verify registers */
	ori	$k0, $k0, 1
	addiu	$k0, $k0, 2
	mtc0	$k0, $14
	nop
	eret

exc_fail:
	/* Print '?' for failure */
	lui	$k1, CONS
	li	$at, '?'
	sw	$at, 0($k1)
	/* Still advance past BREAK */
	addiu	$k0, $k0, 2
	mtc0	$k0, $14
	nop
	eret


/*
 *  MIPS16 test code
 */
	.set mips16

test_mips16_except:
	move	$s1, $ra

	/*
	 *  --- Test A + B: BREAK triggers exception, ERET returns ---
	 *
	 *  Test A: The exception handler (MIPS32) checks EPC bit 0
	 *          and prints 'A' if it's set.
	 *  Test B: After ERET, we resume here in MIPS16 mode and
	 *          print 'B' to confirm successful return.
	 */
	break	0

	/* If we get here, ERET worked — print 'B' */
	li	$v0, 66			/* 'B' */
	sw	$v0, 0($s0)

	/*
	 *  --- Test C: Register preservation across exception ---
	 *  Load known values into registers, take BREAK, verify after.
	 */
	li	$a0, 42
	li	$a1, 99
	break	0

	/* Verify $a0 and $a1 survived the exception */
	/* $a0 should still be 42 */
	li	$v0, 42
	cmp	$a0, $v0		/* t8 = a0 ^ 42 */
	btnez	test_c_fail
	/* $a1 should still be 99 */
	li	$v0, 99
	cmp	$a1, $v0		/* t8 = a1 ^ 99 */
	btnez	test_c_fail
	li	$v0, 67			/* 'C' */
	sw	$v0, 0($s0)
	b	test_d

test_c_fail:
	li	$v0, '?'
	sw	$v0, 0($s0)

	/*
	 *  --- Test D: BTNEZ (branch if T8 != 0) ---
	 */
test_d:
	li	$v0, 10
	li	$v1, 20
	cmp	$v0, $v1		/* t8 = 10 ^ 20 = 30, nonzero */
	btnez	test_d_pass
	/* Should not reach here */
	li	$v0, '?'
	sw	$v0, 0($s0)
	b	test_done

test_d_pass:
	li	$v0, 68			/* 'D' */
	sw	$v0, 0($s0)

test_done:
	move	$ra, $s1
	jr	$ra

	.set nomips16
