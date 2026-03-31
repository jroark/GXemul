/*
 *  MIPS16 test program for GXemul testmips machine.
 *
 *  Starts in MIPS32 mode, writes "M32:" to console, then uses JALX
 *  to enter MIPS16 mode which writes "M16:", then returns to MIPS32
 *  which writes "OK\n" and halts.
 *
 *  Console putchar at 0xb0000000 (KSEG1 mapping of 0x10000000).
 *  Console halt at 0xb0000010.
 *
 *  Assemble:  mipsel-linux-gnu-gcc -EB -mips32r2 -nostdlib -Ttext 0xa0000000
 *             -Wl,-e,_start -o test_mips16 test_mips16.S
 *  Run:       ./gxemul -E testmips test_mips16
 *
 *  Expected output: M32:M16:OK
 */

	.set noreorder

	.text
	.globl _start

/*
 *  MIPS32 entry point
 */
_start:
	/* Load console base address: 0xb0000000 */
	lui	$t0, 0xb000

	/* Write "M32:" */
	li	$t1, 'M'
	sw	$t1, 0($t0)
	li	$t1, '3'
	sw	$t1, 0($t0)
	li	$t1, '2'
	sw	$t1, 0($t0)
	li	$t1, ':'
	sw	$t1, 0($t0)

	/* Save console address in $s0 for MIPS16 code */
	move	$s0, $t0

	/* JALX to MIPS16 code */
	jalx	mips16_func
	nop

	/* Back in MIPS32 mode */
	lui	$t0, 0xb000

	/* Write "OK\n" */
	li	$t1, 'O'
	sw	$t1, 0($t0)
	li	$t1, 'K'
	sw	$t1, 0($t0)
	li	$t1, '\n'
	sw	$t1, 0($t0)

	/* Halt: write to offset 0x10 */
	sw	$zero, 0x10($t0)

	/* Infinite loop fallback */
1:	b	1b
	nop


/*
 *  MIPS16 function.
 *  Uses $s0 (MIPS16 reg 0) as console base address.
 *  Writes "M16:" to console, then returns via JR $ra.
 */
	.set mips16

mips16_func:
	/* $s0 = MIPS16 reg 0, has console base 0xb0000000 */
	li	$v0, 'M'
	sw	$v0, 0($s0)
	li	$v0, '1'
	sw	$v0, 0($s0)
	li	$v0, '6'
	sw	$v0, 0($s0)
	li	$v0, ':'
	sw	$v0, 0($s0)

	/* JR $ra returns to MIPS32 (RA bit 0 = 0 from JALX) */
	jr	$ra

	.set nomips16
