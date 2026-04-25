/*
 *  Copyright (C) 2004-2009  Anders Gavare.  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 *  OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 *  SUCH DAMAGE.
 *
 *
 *  COMMENT: VR41xx (VR4122 and VR4131) misc functions
 *
 *  This is just a big hack.
 *
 *  TODO: Implement more functionality some day.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "cop0.h"
#include "cpu.h"
#include "device.h"
#include "devices.h"
#include "interrupt.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "timer.h"

#include "thirdparty/bcureg.h"
#include "thirdparty/vripreg.h"
#include "thirdparty/vrkiureg.h"
#include "thirdparty/vr_rtcreg.h"

#include "hw/rtc.h"
#include "hw/cmu.h"
#include "hw/pmu.h"

#include "be300_probe.h"

#include "cpu_mips.h"	/* mips_cpu_cold_reset() */


/*  #define debug fatal  */

#define	DEV_VR41XX_TICKSHIFT		14

#define	DEV_VR41XX_LENGTH		0x800	/*  up to (not including) SIU at 0x800  */
struct vr41xx_data {
	struct interrupt cpu_irq;		/*  Connected to MIPS irq 2  */
	int		cpumodel;		/*  Model nr, e.g. 4121  */

	/*  KIU:  */
	int		kiu_console_handle;
	uint32_t	kiu_offset;
	struct interrupt kiu_irq;
	int		kiu_int_assert;
	int		old_kiu_int_assert;

	int		d0, d1, d2, d3, d4, d5;
	int		dont_clear_next;
	int		escape_state;

	/*  Timer:  */
	int		pending_timer_interrupts;
	int		rtcl1_irq_asserted;
	struct interrupt timer_irq;
	struct interrupt rtcl1_irq;
	uint32_t	rtcl1_divisor;
	uint64_t	rtcl1_cycle_accum;
	uint64_t	rtcl1_period_cycles;
	uint64_t	etime_cycle_accum;
	uint64_t	etime_period_cycles;
	uint32_t	last_count_sample;	/* for DEVICE_TICK cycle delta */

	/*  See icureg.h in NetBSD for more info.  */
	uint16_t	sysint1;
	uint16_t	msysint1;
	uint16_t	giuint;
	uint16_t	giumask;
	uint16_t	sysint2;
	uint16_t	msysint2;
	uint16_t	giu_regs[16];
	struct interrupt giu_irq;

	/*  VR4131 RTC (elapsed time counter with read-assist):  */
	rtc_state_t	rtc;
	cmu_state_t	cmu;
	pmu_state_t	pmu;
	uint8_t		bcu_regs[0x20];
	uint8_t		dmaau_regs[0x20];
	uint8_t		dcu_regs[0x20];
	uint8_t		sdramu_regs[0x10];
};

/* Public: the DSIU (VR4131 Debug SIU) ns16550 console handle, captured
 * when the device is registered so the BE-300 machine layer can push
 * keystrokes into it directly. -1 until set. */
int dev_vr41xx_dsiu_console_handle = -1;


static uint64_t vr41xx_latch_read(const uint8_t *regs, uint32_t off,
	unsigned len)
{
	uint64_t value = 0;
	unsigned i;

	for (i = 0; i < len; i++)
		value |= (uint64_t)regs[off + i] << (i * 8);

	return value;
}

static void vr41xx_latch_write(uint8_t *regs, uint32_t off, unsigned len,
	uint64_t value)
{
	unsigned i;

	for (i = 0; i < len; i++)
		regs[off + i] = (uint8_t)(value >> (i * 8));
}

static void vr41xx_seed_latch32(uint8_t *regs, uint32_t off, uint32_t value)
{
	regs[off + 0] = (uint8_t)(value & 0xff);
	regs[off + 1] = (uint8_t)((value >> 8) & 0xff);
	regs[off + 2] = (uint8_t)((value >> 16) & 0xff);
	regs[off + 3] = (uint8_t)((value >> 24) & 0xff);
}

/*
 *  vr41xx_vrip_interrupt_assert():
 *  vr41xx_vrip_interrupt_deassert():
 */
void vr41xx_vrip_interrupt_assert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	if (line < 16)
		d->sysint1 |= (1 << line);
	else
		d->sysint2 |= (1 << (line-16));
	if ((d->sysint1 & d->msysint1) | (d->sysint2 & d->msysint2))
                INTERRUPT_ASSERT(d->cpu_irq);
}
void vr41xx_vrip_interrupt_deassert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	if (line < 16)
		d->sysint1 &= ~(1 << line);
	else
		d->sysint2 &= ~(1 << (line-16));
	if (!(d->sysint1 & d->msysint1) && !(d->sysint2 & d->msysint2))
                INTERRUPT_DEASSERT(d->cpu_irq);
}

/*
 *  Module-scope pointer to the VR41xx instance, used by the CPU-side
 *  edge-triggered interrupt callback. BE-300 has exactly one VR41xx.
 */
static struct vr41xx_data *g_vr41xx_for_edge_cb = NULL;

/*
 *  vr41xx_on_interrupt_delivered():
 *
 *  Called by mips_cpu_exception() when the CPU takes an EXCEPTION_INT
 *  whose CAUSE.IP bits include any of ip_edge_triggered_mask. This lets
 *  the VRIP aggregator resync its internal latches now that the CPU has
 *  delivered the edge.
 *
 *  IP2 is the shared VRIP CPU line for SYSINT1/SYSINT2 sources. RTCL1 is
 *  edge-triggered and consumed on delivery (VR4131 UM sections 11.2.1
 *  and 13.2.9). Other enabled level sources such as GIU/touch must keep
 *  IP2 asserted after the CPU-side edge latch is cleared.
 */
void vr41xx_on_interrupt_delivered(struct cpu *cpu, uint32_t cleared_ip_mask)
{
	struct vr41xx_data *d = g_vr41xx_for_edge_cb;
	(void)cpu;
	if (d == NULL)
		return;
	/*  CAUSE bit 10 == IP2 (STATUS_IM_SHIFT + 2). If that's in the
	    cleared mask, consume the RTCL1 edge.  */
	if ((cleared_ip_mask & (1u << 10)) != 0 && d->rtcl1_irq_asserted) {
		d->sysint1 &= ~(1u << 2);
		d->rtc.rtcint &= ~RTCINT_RTCLONG1_INT;
		d->rtcl1_irq_asserted = 0;
	}
	if ((cleared_ip_mask & (1u << 10)) != 0) {
		if ((d->sysint1 & d->msysint1) ||
		    (d->sysint2 & d->msysint2))
			INTERRUPT_ASSERT(d->cpu_irq);
		else
			INTERRUPT_DEASSERT(d->cpu_irq);
	}
}


/*
 *  vr41xx_giu_interrupt_assert():
 *  vr41xx_giu_interrupt_deassert():
 */
void vr41xx_giu_interrupt_assert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	d->giuint |= (1 << line);
	if (line < 16)
		d->giu_regs[4] |= (1 << line);
	else
		d->giu_regs[5] |= (1 << (line - 16));
	if (d->giuint & d->giumask)
                INTERRUPT_ASSERT(d->giu_irq);
}
void vr41xx_giu_interrupt_deassert(struct interrupt *interrupt)
{
	struct vr41xx_data *d = (struct vr41xx_data *) interrupt->extra;
	int line = interrupt->line;
	d->giuint &= ~(1 << line);
	if (!(d->giuint & d->giumask))
                INTERRUPT_DEASSERT(d->giu_irq);
}


static void recalc_kiu_int_assert(struct cpu *cpu, struct vr41xx_data *d)
{
	if (d->kiu_int_assert != d->old_kiu_int_assert) {
		d->old_kiu_int_assert = d->kiu_int_assert;
		if (d->kiu_int_assert != 0)
			INTERRUPT_ASSERT(d->kiu_irq);
		else
			INTERRUPT_DEASSERT(d->kiu_irq);
	}
}


/*
 *  vr41xx_keytick():
 */
static void vr41xx_keytick(struct cpu *cpu, struct vr41xx_data *d)
{
	int keychange = 0;

	/*
	 *  Keyboard input:
	 *
	 *  Hardcoded for MobilePro. (See NetBSD's hpckbdkeymap.h for
	 *  info on other keyboard layouts. mobilepro780_keytrans is the
	 *  one used here.)
	 *
	 *  TODO: Make this work with "any" keyboard layout.
	 */

	if (d->d0 != 0 || d->d1 != 0 || d->d2 != 0 ||
	    d->d3 != 0 || d->d4 != 0 || d->d5 != 0)
		keychange = 1;

	/*  Release all keys:  */
	if (!d->dont_clear_next) {
		d->d0 = d->d1 = d->d2 = d->d3 = d->d4 = d->d5 = 0;
	} else
		d->dont_clear_next = 0;

	if (console_charavail(d->kiu_console_handle)) {
		char ch = console_readchar(d->kiu_console_handle);

		if (d->escape_state > 0) {
			switch (d->escape_state) {
			case 1:	/*  expecting a [  */
				d->escape_state = 0;
				if (ch == '[')
					d->escape_state = 2;
				break;
			case 2:	/*  cursor keys etc:  */
				/*  Ugly hack for Mobilepro770:  */
				if (cpu->machine->machine_subtype ==
				    MACHINE_HPCMIPS_NEC_MOBILEPRO_770) {
					switch (ch) {
					case 'A': d->d0 = 0x2000; break;
					case 'B': d->d0 = 0x20; break;
					case 'C': d->d0 = 0x1000; break;
					case 'D': d->d0 = 0x10; break;
					default:  fatal("[ vr41xx kiu: unimpl"
					    "emented escape 0x%02 ]\n", ch);
					}
				} else {
					switch (ch) {
					case 'A': d->d0 = 0x1000; break;
					case 'B': d->d0 = 0x2000; break;
					case 'C': d->d0 = 0x20; break;
					case 'D': d->d0 = 0x10; break;
					default:  fatal("[ vr41xx kiu: unimpl"
					    "emented escape 0x%02 ]\n", ch);
					}
				}
				d->escape_state = 0;
			}
		} else switch (ch) {
		case '+':	console_makeavail(d->kiu_console_handle, '=');
				d->d5 = 0x800; break;
		case '_':	console_makeavail(d->kiu_console_handle, '-');
				d->d5 = 0x800; break;
		case '<':	console_makeavail(d->kiu_console_handle, ',');
				d->d5 = 0x800; break;
		case '>':	console_makeavail(d->kiu_console_handle, '.');
				d->d5 = 0x800; break;
		case '{':	console_makeavail(d->kiu_console_handle, '[');
				d->d5 = 0x800; break;
		case '}':	console_makeavail(d->kiu_console_handle, ']');
				d->d5 = 0x800; break;
		case ':':	console_makeavail(d->kiu_console_handle, ';');
				d->d5 = 0x800; break;
		case '"':	console_makeavail(d->kiu_console_handle, '\'');
				d->d5 = 0x800; break;
		case '|':	console_makeavail(d->kiu_console_handle, '\\');
				d->d5 = 0x800; break;
		case '?':	console_makeavail(d->kiu_console_handle, '/');
				d->d5 = 0x800; break;

		case '!':	console_makeavail(d->kiu_console_handle, '1');
				d->d5 = 0x800; break;
		case '@':	console_makeavail(d->kiu_console_handle, '2');
				d->d5 = 0x800; break;
		case '#':	console_makeavail(d->kiu_console_handle, '3');
				d->d5 = 0x800; break;
		case '$':	console_makeavail(d->kiu_console_handle, '4');
				d->d5 = 0x800; break;
		case '%':	console_makeavail(d->kiu_console_handle, '5');
				d->d5 = 0x800; break;
		case '^':	console_makeavail(d->kiu_console_handle, '6');
				d->d5 = 0x800; break;
		case '&':	console_makeavail(d->kiu_console_handle, '7');
				d->d5 = 0x800; break;
		case '*':	console_makeavail(d->kiu_console_handle, '8');
				d->d5 = 0x800; break;
		case '(':	console_makeavail(d->kiu_console_handle, '9');
				d->d5 = 0x800; break;
		case ')':	console_makeavail(d->kiu_console_handle, '0');
				d->d5 = 0x800; break;

		case '1':	d->d3 = 0x80; break;
		case '2':	d->d3 = 0x40; break;
		case '3':	d->d3 = 0x20; break;
		case '4':	d->d3 = 0x10; break;
		case '5':	d->d2 = 0x08; break;
		case '6':	d->d2 = 0x04; break;
		case '7':	d->d2 = 0x02; break;
		case '8':	d->d2 = 0x01; break;
		case '9':	d->d2 = 0x8000; break;
		case '0':	d->d2 = 0x4000; break;

		case ';':	d->d0 = 0x800; break;
		case '\'':	d->d0 = 0x400; break;
		case '[':	d->d0 = 0x200; break;
		case '/':	d->d0 = 0x8; break;
		case '\\':	d->d0 = 0x4; break;
		case ']':	d->d0 = 0x2; break;

		case 'a':	d->d1 = 0x8000; break;
		case 'b':	d->d2 = 0x800; break;
		case 'c':	d->d1 = 0x20; break;
		case 'd':	d->d1 = 0x2000; break;
		case 'e':	d->d2 = 0x20; break;
		case 'f':	d->d1 = 0x1000; break;
		case 'g':	d->d3 = 0x8; break;
		case 'h':	d->d3 = 0x4; break;
		case 'i':	d->d3 = 0x100; break;
		case 'j':	d->d3 = 0x2; break;
		case 'k':	d->d3 = 0x1; break;
		case 'l':	d->d0 = 0x80; break;
		case 'm':	d->d2 = 0x200; break;
		case 'n':	d->d2 = 0x400; break;
		case 'o':	d->d0 = 0x8000; break;
		case 'p':	d->d4 = 0x20; break;
		case 'q':	d->d2 = 0x80; break;
		case 'r':	d->d2 = 0x10; break;
		case 's':	d->d1 = 0x4000; break;
		case 't':	d->d3 = 0x800; break;
		case 'u':	d->d3 = 0x200; break;
		case 'v':	d->d1 = 0x10; break;
		case 'w':	d->d2 = 0x40; break;
		case 'x':	d->d1 = 0x40; break;
		case 'y':	d->d3 = 0x400; break;
		case 'z':	d->d1 = 0x80; break;

		case ',':	d->d2 = 0x100; break;
		case '.':	d->d0 = 0x4000; break;
		case '-':	d->d1 = 0x400; break;
		case '=':	d->d1 = 0x200; break;

		case '\r':
		case '\n':	d->d0 = 0x40; break;
		case ' ':	d->d0 = 0x01; break;
		case '\b':	d->d4 = 0x10; break;

		case 27:	d->escape_state = 1; break;

		default:
			/*  Shifted:  */
			if (ch >= 'A' && ch <= 'Z') {
				console_makeavail(d->kiu_console_handle,
				    ch + 32);
				d->d5 = 0x800;
				d->dont_clear_next = 1;
				break;
			}

			/*  CTRLed:  */
			if (ch >= 1 && ch <= 26) {
				console_makeavail(d->kiu_console_handle,
				    ch + 96);
				d->d5 = 0x4;
				d->dont_clear_next = 1;
				break;
			}
		}

		if (d->escape_state == 0)
			keychange = 1;
	}

	if (keychange) {
		/*  4=lost data, 2=data complete, 1=key input detected  */
		d->kiu_int_assert |= 3;
		recalc_kiu_int_assert(cpu, d);
	}
}


/*
 *  timer_tick():
 */
static void timer_tick(struct timer *timer, void *extra)
{
	struct vr41xx_data *d = (struct vr41xx_data *) extra;
	(void)timer;

	/*
	 * Each RTCL1 underflow latches the source bit in RTCINTREG and
	 * generates an interrupt edge into the ICU. SYSINT1 (the per-source
	 * ICU latch) coalesces repeated edges naturally; the queue cap
	 * (pending_timer_interrupts capped at 1 in the device tick) prevents
	 * an unbounded backlog. Do NOT gate on RTCINTREG bits here, because
	 * WinCE NK does not write RTCINTREG to ack — it acks the source via
	 * SYSINT1 write-1-to-clear, then re-enables the bit in MSYSINT1.
	 */
	d->pending_timer_interrupts ++;
	d->rtc.rtcint |= RTCINT_RTCLONG1_INT;
}


DEVICE_TICK(vr41xx)
{
	struct vr41xx_data *d = (struct vr41xx_data *) extra;
	uint64_t emulated_hz;
	uint64_t tick_cycles;

	emulated_hz = cpu->machine != NULL
	    ? (uint64_t)cpu->machine->emulated_hz : 0;
	if (emulated_hz == 0)
		emulated_hz = 131072000ULL;
	/*
	 * Use CP0 Count delta as the true elapsed-cycles measure since the
	 * last DEVICE_TICK call. Dyntrans batches terminate early (exceptions,
	 * page boundaries) so the callback fires far more often than the
	 * nominal 1<<TICKSHIFT cycle interval — treating the tick interval as
	 * a fixed 1<<TICKSHIFT causes accum to grow 10-100× too fast and
	 * RTCL1 to assert on nearly every DEVICE_TICK (observed ~131 kHz vs
	 * 1 kHz spec for WinCE NK). See memory/project_post_ppsh_stall.md
	 * Pass 8.
	 */
	{
		uint32_t cur_count =
		    (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_COUNT];
		uint32_t count_delta = cur_count - d->last_count_sample;
		d->last_count_sample = cur_count;
		/*
		 * Clamp to a sane upper bound on the first call / after
		 * Count-register writes by the guest, so a 32-bit wraparound
		 * or guest-driven reset doesn't inject a 4 GB cycle burst.
		 */
		if (count_delta > (uint32_t)(1U << DEV_VR41XX_TICKSHIFT) * 4)
			count_delta = (uint32_t)(1U << DEV_VR41XX_TICKSHIFT);
		tick_cycles = count_delta;

		/*
		 * Pet / fire the PMU HALTimer watchdog (UM §12.2.2). Mask
		 * to 32 bits because COP0_COUNT is stored sign-extended
		 * (cpu_dyntrans.c:469 uses `(int32_t)(old + n_instrs)`),
		 * so feeding the raw uint64_t drives the PMU delta math
		 * negative across the 0x80000000 boundary.
		 */
		pmu_tick(&d->pmu, (uint64_t) cur_count);
	}

	if (d->etime_period_cycles == 0) {
		d->etime_period_cycles = emulated_hz /
		    (uint64_t)ETIME_L_HZ;
		if (d->etime_period_cycles == 0)
			d->etime_period_cycles = 1;
	}

	d->etime_cycle_accum += tick_cycles;
	while (d->etime_cycle_accum >= d->etime_period_cycles) {
		d->etime_cycle_accum -= d->etime_period_cycles;
		rtc_tick(&d->rtc, 1);
	}

	if (d->rtcl1_divisor != 0 && d->rtcl1_period_cycles != 0) {
		/*
		 * Drive RTCL1 from emulated instruction progress. Each
		 * underflow of the divisor produces an interrupt edge.
		 *
		 * Backlog handling: pending_timer_interrupts is capped at 1
		 * below, and the SYSINT1 latch already coalesces repeated
		 * edges (if NK hasn't acked the previous one, the bit just
		 * stays set). Do NOT additionally gate on RTCINTREG bits —
		 * WinCE NK acks RTCL1 by writing SYSINT1 (write-1-to-clear),
		 * not by writing RTCINTREG, so gating on rtcint locks the
		 * timer up after the first fire.
		 */
		d->rtcl1_cycle_accum += tick_cycles;
		while (d->rtcl1_cycle_accum >= d->rtcl1_period_cycles) {
			d->rtcl1_cycle_accum -= d->rtcl1_period_cycles;
			timer_tick(NULL, d);
			if (d->pending_timer_interrupts > 0)
				break;
		}
	}

	/*
	 * RTCL1 delivery model (per VR4131 UM §11.2.1 SYSINT1 read-only,
	 * §13.2.9 RTCINTREG.RTCINTR1 sticky W1C).
	 *
	 * RTCL1 is a brief edge pulse on real hardware. WinCE NK's OAL IP2
	 * dispatcher does NOT write RTCINTREG to ack (only 2 RTCINTREG
	 * writes observed across 15 s of boot, both at init), and NK's ISR
	 * mask manipulation (MSYSINT1=0 then back to 0x0105) has RTCL1 bit 2
	 * enabled — so with a level-held IRQ, sysint1 bit 2 stays set and
	 * every eret re-fires the exception, producing a ~99 kHz ISR storm
	 * (memory/project_post_ppsh_stall.md pass 5-7).
	 *
	 * Fix: mark IP2 as edge-triggered via the CPU's ip_edge_triggered
	 * _mask (set in dev_vr41xx_init below). On EXCEPTION_INT entry,
	 * cpu_mips.c clears CAUSE.IP2 and calls vr41xx_on_interrupt_delivered
	 * here so we also clear the VRIP-side latch (sysint1 bit 2 +
	 * rtc.rtcint RTCINTR1). Net effect: each RTCL1 underflow produces a
	 * single CPU exception, and the subsequent eret sees IP=0.
	 */
	if (d->pending_timer_interrupts > 1)
		d->pending_timer_interrupts = 1;
	if (d->pending_timer_interrupts > 0) {
		INTERRUPT_ASSERT(d->rtcl1_irq);
		d->rtcl1_irq_asserted = 1;
		d->pending_timer_interrupts --;
	}

	/*
	 *  ETIME/ECMP compare interrupt — used by Linux 2.6 VR41xx timer
	 *  and WinCE elapsed time timer.
	 *  The 2.6 kernel programs ECMP and expects an interrupt when
	 *  ETIME reaches ECMP.  Only use this path when the RTCL1 timer
	 *  is not active; the 2.4 kernel creates the
	 *  RTCL1 timer via offset 0xd0 and the initial etime >> ecmp
	 *  would cause a spurious interrupt storm.
	 */
	if (d->rtcl1_divisor == 0 &&
	    (d->rtc.rtcint & RTCINT_ELAPSEDTIME_INT))
		INTERRUPT_ASSERT(d->timer_irq);

	/*  KIU keyboard tick — disabled (no X11 keyboard input)  */
	(void)vr41xx_keytick;
}


/*
 *  vr41xx_kiu():
 *
 *  Keyboard Interface Unit. Return value is "odata".
 *  (See NetBSD's vrkiu.c for more info.)
 */
static uint64_t vr41xx_kiu(struct cpu *cpu, int ofs, uint64_t idata,
	int writeflag, struct vr41xx_data *d)
{
	uint64_t odata = 0;

	switch (ofs) {
	case KIUDAT0:
		odata = d->d0; break;
	case KIUDAT1:
		odata = d->d1; break;
	case KIUDAT2:
		odata = d->d2; break;
	case KIUDAT3:
		odata = d->d3; break;
	case KIUDAT4:
		odata = d->d4; break;
	case KIUDAT5:
		odata = d->d5; break;
	case KIUSCANREP:
		if (writeflag == MEM_WRITE) {
			debug("[ vr41xx KIU: setting KIUSCANREP to 0x%04x ]\n",
			    (int)idata);
			/*  TODO  */
		} else
			fatal("[ vr41xx KIU: unimplemented read from "
			    "KIUSCANREP ]\n");
		break;
	case KIUSCANS:
		if (writeflag == MEM_WRITE) {
			debug("[ vr41xx KIU: write to KIUSCANS: 0x%04x: TODO"
			    " ]\n", (int)idata);
			/*  TODO  */
		} else
			debug("[ vr41xx KIU: unimplemented read from "
			    "KIUSCANS ]\n");
		break;
	case KIUINT:
		/*  Interrupt. A wild guess: zero-on-write  */
		if (writeflag == MEM_WRITE) {
			d->kiu_int_assert &= ~idata;
		} else {
			odata = d->kiu_int_assert;
		}
		recalc_kiu_int_assert(cpu, d);
		break;
	case KIURST:
		/*  Reset.  */
		break;
	default:
		if (writeflag == MEM_WRITE)
			debug("[ vr41xx KIU: unimplemented write to offset "
			    "0x%x, data=0x%016" PRIx64" ]\n", ofs,
			    (uint64_t) idata);
		else
			debug("[ vr41xx KIU: unimplemented read from offset "
			    "0x%x ]\n", ofs);
	}

	return odata;
}


DEVICE_ACCESS(vr41xx)
{
	struct vr41xx_data *d = (struct vr41xx_data *) extra;
	uint64_t idata = 0, odata = 0;
	int revision = 0;

	if (writeflag == MEM_WRITE)
		idata = memory_readmax64(cpu, data, len);

	/*  KIU ("Keyboard Interface Unit") is handled separately.  */
	if (relative_addr >= d->kiu_offset &&
	    relative_addr < d->kiu_offset + 0x20) {
		odata = vr41xx_kiu(cpu, relative_addr - d->kiu_offset,
		    idata, writeflag, d);
		goto ret;
	}

	/*  TODO: Maybe these should be handled separately as well?  */
	if (d->cpumodel == 4131) {
		if (relative_addr < 0x20) {
			/*
			 *  The ROM programs BCUCNT/ROMSIZE/IO timing via the
			 *  0x000-0x01f window and later polls offset 0x000 as
			 *  a completion flag. Keep that subwindow stateful so
			 *  the ROM can observe its own writes, but preserve the
			 *  strap-backed identity/clock bytes at 0x10-0x15.
			 */
			if (writeflag == MEM_READ) {
				odata = vr41xx_latch_read(d->bcu_regs,
				    (uint32_t)relative_addr, (unsigned)len);
			} else {
				uint8_t readonly_shadow[6];

				memcpy(readonly_shadow, d->bcu_regs + 0x10,
				    sizeof(readonly_shadow));
				vr41xx_latch_write(d->bcu_regs,
				    (uint32_t)relative_addr, (unsigned)len, idata);
				memcpy(d->bcu_regs + 0x10, readonly_shadow,
				    sizeof(readonly_shadow));
			}
			goto ret;
		}
		if (relative_addr >= 0x20 && relative_addr < 0x40) {
			uint32_t off = (uint32_t)(relative_addr - 0x20);
			if (writeflag == MEM_READ)
				odata = vr41xx_latch_read(d->dmaau_regs, off,
				    (unsigned)len);
			else
				vr41xx_latch_write(d->dmaau_regs, off,
				    (unsigned)len, idata);
			goto ret;
		}
		if (relative_addr >= 0x40 && relative_addr < 0x60) {
			uint32_t off = (uint32_t)(relative_addr - 0x40);
			if (writeflag == MEM_READ)
				odata = vr41xx_latch_read(d->dcu_regs, off,
				    (unsigned)len);
			else
				vr41xx_latch_write(d->dcu_regs, off,
				    (unsigned)len, idata);
			goto ret;
		}
		if (relative_addr >= 0x400 && relative_addr < 0x410) {
			uint32_t off = (uint32_t)(relative_addr - 0x400);
			if (writeflag == MEM_READ)
				odata = vr41xx_latch_read(d->sdramu_regs, off,
				    (unsigned)len);
			else
				vr41xx_latch_write(d->sdramu_regs, off,
				    (unsigned)len, idata);
			goto ret;
		}
	}

	switch (relative_addr) {

	/*  BCU:  0x00 .. 0x1c  */
	case BCUREVID_REG_W:	/*  0x010  */
	case BCU81REVID_REG_W:	/*  0x014  */
		/*
		 *  TODO?  Linux seems to read 0x14. The lowest bits are
		 *  a divisor for PClock, bits 8 and up seem to be a
		 *  divisor for VTClock (relative to PClock?)...
		 */
		switch (d->cpumodel) {
		case 4131:	revision = BCUREVID_RID_4131; break;
		case 4122:	revision = BCUREVID_RID_4122; break;
		case 4121:	revision = BCUREVID_RID_4121; break;
		case 4111:	revision = BCUREVID_RID_4111; break;
		case 4102:	revision = BCUREVID_RID_4102; break;
		case 4101:	revision = BCUREVID_RID_4101; break;
		case 4181:	revision = BCUREVID_RID_4181; break;
		}
		odata = (revision << BCUREVID_RIDSHFT) | 0x020c;
		break;
	case BCU81CLKSPEED_REG_W:	/*  0x018  */
		/*
		 *  TODO: Implement this for ALL cpu types:
		 */
		odata = BCUCLKSPEED_DIVT4 << BCUCLKSPEED_DIVTSHFT;
		break;

	/*  DMAAU:  0x20 .. 0x3c  */

	/*  DCU:  0x40 .. 0x5c  */

	/*  CMU:  0x60 .. 0x7c  */
	case 0x60:
	case 0x62:
		if (d->cpumodel == 4131) {
			uint32_t cmu_off = (uint32_t)(relative_addr - 0x60);
			if (writeflag == MEM_READ)
				odata = cmu_read(&d->cmu, cmu_off, (unsigned)len);
			else
				cmu_write(&d->cmu, cmu_off, (unsigned)len,
				    (uint32_t)idata);
			break;
		}
		goto unhandled;

	/*  ICU:  0x80 .. 0xbc  */
	case 0x80:	/*  Level 1 system interrupt reg 1...  */
		if (writeflag == MEM_READ)
			odata = d->sysint1;
		else {
			/*  TODO: clear-on-write-one?  */
			d->sysint1 &= ~idata;
			d->sysint1 &= 0xffff;
		}
		break;
	case 0x88:
		if (writeflag == MEM_READ)
			odata = d->giuint;
		else
			d->giuint &= ~idata;
		break;
	case 0x8c:
		if (writeflag == MEM_READ)
			odata = d->msysint1;
		else {
			/*
			 *  Let the kernel control MSYSINT1 fully.
			 *  Previously ETIMER (bit 3) was force-enabled
			 *  for Linux 2.6, but this prevents WinCE from
			 *  managing its own timer mask — the WinCE
			 *  interrupt dispatch loop checks SYSINT1 &
			 *  MSYSINT1 and loops forever if ETIMER is
			 *  stuck enabled while the timer fires.
			 */
			d->msysint1 = idata;
		}
		break;
	case 0x94:
		if (writeflag == MEM_READ)
			odata = d->giumask;
		else
			d->giumask = idata;
		break;
	case 0xa0:	/*  Level 1 system interrupt reg 2...  */
		if (writeflag == MEM_READ)
			odata = d->sysint2;
		else {
			/*  TODO: clear-on-write-one?  */
			d->sysint2 &= ~idata;
			d->sysint2 &= 0xffff;
		}
		break;
	case 0xa6:
		if (writeflag == MEM_READ)
			odata = d->msysint2;
		else
			d->msysint2 = idata;
		break;

	/*  VR4131 PMU lives at 0xc0; older VR41xx chips use this as RTC.  */
	case 0xc0:
	case 0xc2:
	case 0xc4:
	case 0xc6:
	case 0xc8:
	case 0xcc:
		if (d->cpumodel == 4131) {
			uint32_t pmu_off = (uint32_t)(relative_addr - 0xc0);
			if (writeflag == MEM_READ) {
				odata = pmu_read(&d->pmu, pmu_off, (unsigned)len);
			} else {
				pmu_write(&d->pmu, pmu_off, (unsigned)len,
				    (uint32_t)idata);
				/*
				 * SOFTRST dispatch: pmu_write staged the
				 * RSTSW cause bit and set softrst_pending;
				 * we supply cur_count (pmu.c has no cpu
				 * handle) and drive the cold-reset thunk.
				 */
				if (d->pmu.softrst_pending) {
					uint64_t cur_count = (uint32_t)
					    cpu->cd.mips.coproc[0]->reg[COP0_COUNT];
					d->pmu.softrst_pending = 0;
					pmu_apply_pending_reset_state(&d->pmu,
					    cur_count);
					if (d->pmu.cold_reset != NULL)
						d->pmu.cold_reset(d->pmu.cpu_opaque);
				}
			}
			break;
		}
		if (relative_addr > 0xc4)
			goto unhandled;
		{
			struct timeval tv;
			gettimeofday(&tv, NULL);
			/*  Adjust time by 120 years and 29 days.  */
			tv.tv_sec += (int64_t) (120*365 + 29) * 24*60*60;

			switch (relative_addr) {
			case 0xc0:
				odata = (tv.tv_sec & 1) << 15;
				odata += (uint64_t)tv.tv_usec * 32768 / 1000000;
				break;
			case 0xc2:
				odata = (tv.tv_sec >> 1) & 0xffff;
				break;
			case 0xc4:
				odata = (tv.tv_sec >> 17) & 0xffff;
				break;
			}
		}
		break;

	case 0xd0:	/*  RTCL1_L_REG_W (older VR41xx)  */
	case 0x110:	/*  RTCL1_L_REG_W (VR4131 relocated)  */
		if (writeflag == MEM_WRITE && idata != 0) {
			uint64_t emulated_hz = cpu->machine != NULL
			    ? (uint64_t)cpu->machine->emulated_hz : 0;

			if (emulated_hz == 0)
				emulated_hz = 131072000ULL;

			d->rtcl1_divisor = (uint32_t)idata;
			d->rtcl1_cycle_accum = 0;
			d->rtcl1_period_cycles =
			    (emulated_hz * (uint64_t)d->rtcl1_divisor)
			    / (uint64_t)RTCL1_L_HZ;
			if (d->rtcl1_period_cycles == 0)
				d->rtcl1_period_cycles = 1;
		}
		if (d->cpumodel == 4131 && relative_addr == 0x110) {
			uint32_t rtc_off = (uint32_t)(relative_addr - 0x100);

			if (writeflag == MEM_READ)
				odata = rtc_read(&d->rtc, rtc_off,
				    (unsigned)len);
			else
				rtc_write(&d->rtc, rtc_off, (unsigned)len,
				    (uint32_t)idata);
		}
		break;
	case 0xd2:	/*  RTCL1_H_REG_W (older VR41xx)  */
	case 0x112:	/*  RTCL1_H_REG_W (VR4131 relocated)  */
		if (d->cpumodel == 4131 && relative_addr == 0x112) {
			uint32_t rtc_off = (uint32_t)(relative_addr - 0x100);

			if (writeflag == MEM_READ)
				odata = rtc_read(&d->rtc, rtc_off,
				    (unsigned)len);
			else
				rtc_write(&d->rtc, rtc_off, (unsigned)len,
				    (uint32_t)idata);
		}
		break;

	/*
	 *  VR4131 RTC block (offset 0x100-0x13e). On older VR41xx chips,
	 *  only the ETIME/ECMP subset lives elsewhere; on VR4131 the full
	 *  RTC/RTC2 window is relocated here.
	 */
	case 0x100:	/*  VR4131 ETIMELREG  */
	case 0x102:	/*  VR4131 ETIMEMREG  */
	case 0x104:	/*  VR4131 ETIMEHREG  */
	case 0x108:	/*  VR4131 ECMPLREG / older VR41xx GIUINTREG  */
	case 0x10a:	/*  VR4131 ECMPMREG  */
	case 0x10c:	/*  VR4131 ECMPHREG  */
	case 0x114:	/*  VR4131 RTCL1CNTLREG  */
	case 0x116:	/*  VR4131 RTCL1CNTHREG  */
	case 0x118:	/*  VR4131 RTCL2LREG  */
	case 0x11a:	/*  VR4131 RTCL2HREG  */
	case 0x11c:	/*  VR4131 RTCL2CNTLREG  */
	case 0x11e:	/*  VR4131 RTCL2CNTHREG  */
	case 0x120:	/*  VR4131 TCLKLREG  */
	case 0x122:	/*  VR4131 TCLKHREG  */
	case 0x124:	/*  VR4131 TCLKCNTLREG  */
	case 0x126:	/*  VR4131 TCLKCNTHREG  */
		if (d->cpumodel == 4131) {
			uint32_t rtc_off = (uint32_t)(relative_addr - 0x100);
			if (writeflag == MEM_READ)
				odata = rtc_read(&d->rtc, rtc_off, (unsigned)len);
			else
				rtc_write(&d->rtc, rtc_off, (unsigned)len,
				    (uint32_t)idata);
			break;
		}
		if (relative_addr == 0x108) {
			/*  Older VR41xx: GIU interrupt register  */
			if (writeflag == MEM_READ)
				odata = d->giuint;
			else
				d->giuint &= ~idata;
			break;
		}
		goto unhandled;

	case 0x13e:	/*  on 4181? / VR4131 RTCINTREG  */
	case 0x1de:	/*  on 4121?  */
		/*  RTC interrupt register...  */
		/*  Ack. only the timer sources whose RTCINT bits are written. */
		if (idata & RTCINT_RTCLONG1_INT) {
			INTERRUPT_DEASSERT(d->rtcl1_irq);
			d->rtcl1_irq_asserted = 0;
		}
		if (idata & RTCINT_ELAPSEDTIME_INT)
			INTERRUPT_DEASSERT(d->timer_irq);
		if (d->cpumodel == 4131 && relative_addr == 0x13e)
			rtc_write(&d->rtc, RTC_RTCINTREG, (unsigned)len,
			    (uint32_t)idata);
		break;

	/*
	 *  VR4131 GIU (GPIO) registers at 0x140-0x15C.
	 *  GIUPIODL (0x144) returns pin input state — WinCE checks this
	 *  to determine hardware/power status during OAL init.
	 */
	case 0x140:	/*  GIUIOSELL - I/O direction select low  */
	case 0x142:	/*  GIUIOSELH - I/O direction select high  */
	case 0x146:	/*  GIUPIODH - Pin I/O data high  */
	case 0x14c:	/*  GIUINTENL  */
	case 0x14e:	/*  GIUINTENH  */
	case 0x150:	/*  GIUINTTYPL  */
	case 0x152:	/*  GIUINTTYPH  */
	case 0x154:	/*  GIUINTALSELL  */
	case 0x156:	/*  GIUINTALSELH  */
	case 0x158:	/*  GIUINTHTSELL  */
	case 0x15a:	/*  GIUINTHTSELH  */
		/*  Simple latch for most GIU registers  */
		{
			int idx = ((int)relative_addr - 0x140) / 2;
			if (idx >= 0 && idx < 16) {
				if (writeflag == MEM_WRITE)
					d->giu_regs[idx] = (uint16_t)idata;
				else
					odata = d->giu_regs[idx];
			}
		}
		break;
	case 0x148:	/*  GIUINTSTATL  */
	case 0x14a:	/*  GIUINTSTATH  */
		{
			int idx = ((int)relative_addr - 0x140) / 2;
			if (idx >= 0 && idx < 16) {
				if (writeflag == MEM_WRITE) {
					/*  VR4131 UM sections 14.2.5/14.2.6:
					    GIUINTSTAT bits are cleared when
					    1 is written to the bit.  The ICU
					    level-2 GIUINTL summary reflects
					    these low GPIO interrupt bits.  */
					d->giu_regs[idx] &= ~(uint16_t)idata;
					if (relative_addr == 0x148) {
						d->giuint &= ~(uint16_t)idata;
						if ((d->giuint & d->giumask) == 0)
							INTERRUPT_DEASSERT(d->giu_irq);
					}
				} else
					odata = d->giu_regs[idx];
			}
		}
		break;
	case 0x144:	/*  GIUPIODL - Pin I/O data low  */
		if (writeflag == MEM_WRITE) {
			/*  Just latch output bits  */
		} else {
			/*
			 *  Return board strap / GPIO input state.
			 *
			 *  The BE-300 does not cold-boot with GIUPIODL at
			 *  zero on real hardware. BEDiag dumps show
			 *  0x0F000144 = 0xAAE2 / 0xAAE6, which satisfies
			 *  the early FUN_80077210 wait on bits 0x0800 and
			 *  0x0200 while leaving 0x0400 clear.
			 */
			if (cpu->machine->machine_subtype ==
			    MACHINE_HPCMIPS_CASIO_BE300)
				odata = 0xAAE2;
			else
				odata = 0;
		}
		break;

	default:
	unhandled:
		be300_probe_note_mmio("vr41xx-default",
		    (uint32_t) relative_addr,
		    writeflag == MEM_WRITE ? 'W' : 'R',
		    (uint32_t) len, (uint64_t)(uint32_t) cpu->pc,
		    BE300_MMIO_CLASS_DEFAULT);
		if (writeflag == MEM_WRITE)
			debug("[ vr41xx: unimplemented write to address "
			    "0x%" PRIx64", data=0x%016" PRIx64" ]\n",
			    (uint64_t) relative_addr, (uint64_t) idata);
		else
			debug("[ vr41xx: unimplemented read from address "
			    "0x%" PRIx64" ]\n", (uint64_t) relative_addr);
	}

ret:
	/*
	 *  Recalculate interrupt assertions:
	 */
	if (d->giuint & d->giumask)
                INTERRUPT_ASSERT(d->giu_irq);
	else
                INTERRUPT_DEASSERT(d->giu_irq);
	if ((d->sysint1 & d->msysint1) | (d->sysint2 & d->msysint2))
                INTERRUPT_ASSERT(d->cpu_irq);
	else
                INTERRUPT_DEASSERT(d->cpu_irq);

	if (writeflag == MEM_READ)
		memory_writemax64(cpu, data, len, odata);

	return 1;
}


/*
 *  vr41xx_issue_cold_reset_thunk():
 *
 *  Bridge between pmu.c (CPU-free) and the gxemul CPU layer. Stored
 *  in pmu_state_t::cold_reset by pmu_arm_initial() and invoked on
 *  HALTimer expiry or PMUCNT2REG SOFTRST write.
 */
static void vr41xx_issue_cold_reset_thunk(void *opaque)
{
	mips_cpu_cold_reset((struct cpu *)opaque);
}


/*
 *  dev_vr41xx_init():
 *
 *  machine->path is something like "machine[0]".
 */
struct vr41xx_data *dev_vr41xx_init(struct machine *machine,
	struct memory *mem, int cpumodel)
{
	struct vr41xx_data *d;
	uint64_t baseaddr = 0;
	char tmps[300];
	int i;

	CHECK_ALLOCATION(d = (struct vr41xx_data *) malloc(sizeof(struct vr41xx_data)));
	memset(d, 0, sizeof(struct vr41xx_data));

	/*  Connect to MIPS irq 2:  */
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].2",
	    machine->path, machine->bootstrap_cpu);
	INTERRUPT_CONNECT(tmps, d->cpu_irq);

	/*
	 *  Mark IP2 as edge-triggered so mips_cpu_exception() auto-clears
	 *  the IRQ after the CPU takes the exception (see
	 *  vr41xx_on_interrupt_delivered above). CAUSE.IP2 is bit 10 of
	 *  CAUSE (STATUS_IM_SHIFT + 2). Without this, WinCE NK's RTCL1 ISR
	 *  re-enters ~15× per underflow — see memory/project_post_ppsh
	 *  _stall.md pass 5-7 and VR4131 UM §11.2.1 / §13.2.9.
	 */
	{
		struct cpu *c = machine->cpus[machine->bootstrap_cpu];
		c->cd.mips.ip_edge_triggered_mask |= (1u << 10);
	}
	g_vr41xx_for_edge_cb = d;

	/*
	 *  Register VRIP interrupt lines 0..25:
	 */
	for (i=0; i<=25; i++) {
		struct interrupt templ;
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
		    machine->path, machine->bootstrap_cpu, i);
		memset(&templ, 0, sizeof(templ));
		templ.line = i;
		templ.name = tmps;
		templ.extra = d;
		templ.interrupt_assert = vr41xx_vrip_interrupt_assert;
		templ.interrupt_deassert = vr41xx_vrip_interrupt_deassert;
		interrupt_handler_register(&templ);
	}

	/*
	 *  Register GIU interrupt lines 0..31:
	 */
	for (i=0; i<32; i++) {
		struct interrupt templ;
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i.giu.%i",
		    machine->path, machine->bootstrap_cpu, VRIP_INTR_GIU, i);
		memset(&templ, 0, sizeof(templ));
		templ.line = i;
		templ.name = tmps;
		templ.extra = d;
		templ.interrupt_assert = vr41xx_giu_interrupt_assert;
		templ.interrupt_deassert = vr41xx_giu_interrupt_deassert;
		interrupt_handler_register(&templ);
	}

	d->cpumodel = cpumodel;

	/*  VR4131 RTC elapsed-time counter (read-assist for SPL polling):  */
	rtc_init(&d->rtc);
	cmu_init(&d->cmu);
	pmu_init(&d->pmu);

	/*
	 * Arm the HALTimer (VR4131 UM §12.2.2): ~4 s window after RTCRST
	 * that must be petted via PMUCNTREG bit 2 or the CPU is cold-reset
	 * with SDRAM preserved.
	 *
	 * Count rate: gxemul's dyntrans increments COP0_COUNT by n_instrs
	 * per batch (cpu_dyntrans.c:469), so in-emulator Count advances at
	 * ~emulated_hz, not CPU/2. The hz/2 cadence fix in
	 * cpu_mips_coproc.c only affects the Compare wall-clock timer, not
	 * Count itself. Using emulated_hz * 4 here gives ~4 seconds of
	 * emulated time on the same clock basis as all other Count-driven
	 * timers in this tree.
	 */
	if (cpumodel == 4131) {
		struct cpu *c0 = machine->cpus[machine->bootstrap_cpu];
		uint64_t cur_count = (uint32_t)
		    c0->cd.mips.coproc[0]->reg[COP0_COUNT];
		uint64_t arm_cycles = (uint64_t)machine->emulated_hz * 4;
		pmu_arm_initial(&d->pmu, cur_count, arm_cycles,
		    vr41xx_issue_cold_reset_thunk, c0);
	}

	/*
	 * BCU: only seed read-only hardware values. The ROM programs
	 * bus timing registers (BCUCNTREG1, ROM/IO speed/size) during
	 * early init. REVIDREG and CLKSPEEDREG are fixed by silicon
	 * and pin strapping.
	 */
	vr41xx_seed_latch32(d->bcu_regs, 0x10, 0x00005002);  /* REVIDREG */
	vr41xx_latch_write(d->bcu_regs, 0x14, 2, 0x020c);    /* CLKSPEEDREG */

	/*  TODO: VRC4173 has the KIU at offset 0x100?  */
	d->kiu_offset = 0x180;
	d->kiu_console_handle = -1;

	/*  Connect to the KIU and GIU interrupts:  */
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
	    machine->path, machine->bootstrap_cpu, VRIP_INTR_GIU);
	INTERRUPT_CONNECT(tmps, d->giu_irq);
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
	    machine->path, machine->bootstrap_cpu, VRIP_INTR_KIU);
	INTERRUPT_CONNECT(tmps, d->kiu_irq);

	switch (cpumodel) {
	case 4101:
	case 4102:
	case 4111:
	case 4121:
		baseaddr = 0xb000000;
		break;
	case 4181:
		baseaddr = 0xa000000;
		dev_ram_init(machine, 0xb000000, 0x1000000, DEV_RAM_MIRROR,
		    0xa000000, NULL);
		break;
	case 4122:
	case 4131:
		baseaddr = 0xf000000;
		break;
	default:
		printf("Unimplemented VR cpu model\n");
		exit(1);
	}

	if (d->cpumodel == 4121 || d->cpumodel == 4181)
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].3",
		    machine->path, machine->bootstrap_cpu);
	else
		snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
		    machine->path, machine->bootstrap_cpu, VRIP_INTR_ETIMER);
	INTERRUPT_CONNECT(tmps, d->timer_irq);
	/*
	 * RTCL1 is a distinct VRIP source on VR41xx (line 2). Route it
	 * through its dedicated SYSINT1 bit so the guest sees and clears
	 * the correct interrupt source in the ICU/RTC registers.
	 */
	snprintf(tmps, sizeof(tmps), "%s.cpu[%i].vrip.%i",
	    machine->path, machine->bootstrap_cpu, VRIP_INTR_RTCL1);
	INTERRUPT_CONNECT(tmps, d->rtcl1_irq);

	memory_device_register(mem, "vr41xx", baseaddr, DEV_VR41XX_LENGTH,
	    dev_vr41xx_access, (void *)d, DM_DEFAULT, NULL);

	/*
	 *  TODO: Find out which controllers are at which addresses on
	 *  which chips.
	 */
	if (cpumodel == 4131) {
		snprintf(tmps, sizeof(tmps), "ns16550 irq=%s.cpu[%i].vrip.%i "
		    "addr=0x%" PRIx64" in_use=0 name2=siu", machine->path,
		    machine->bootstrap_cpu, VRIP_INTR_SIU,
		    (uint64_t) (baseaddr+0x800));
		device_add(machine, tmps);

		/* VR4131 DSIU (Debug SIU) at baseaddr+0x820.
		 * NK.exe OAL writes debug serial output here
		 * (FUN_80078308 polls LSR at +0x05, writes THR
		 * at +0x00).  The SIU ns16550 window is shrunk
		 * to 0x20 bytes to avoid collision. */
		snprintf(tmps, sizeof(tmps), "ns16550 irq=%s.cpu[%i].vrip.%i "
		    "addr=0x%" PRIx64" in_use=0 name2=dsiu", machine->path,
		    machine->bootstrap_cpu, VRIP_INTR_DSIU,
		    (uint64_t) (baseaddr+0x820));
		dev_vr41xx_dsiu_console_handle = (int)(size_t)
		    device_add(machine, tmps);
	} else {
		/*  This is used by Linux and NetBSD:  */
		snprintf(tmps, sizeof(tmps), "ns16550 irq=%s.cpu[%i]."
		    "vrip.%i addr=0x%x name2=serial", machine->path,
		    machine->bootstrap_cpu, VRIP_INTR_SIU, 0xc000000);
		device_add(machine, tmps);
	}

	/*  Hm... maybe this should not be here.  TODO  */
	snprintf(tmps, sizeof(tmps), "pcic irq=%s.cpu[%i].vrip.%i addr="
	    "0x140003e0", machine->path, machine->bootstrap_cpu,
	    VRIP_INTR_GIU);
	device_add(machine, tmps);

	machine_add_tickfunction(machine, dev_vr41xx_tick, d,
	    DEV_VR41XX_TICKSHIFT);

	/*  Some machines (?) use ISA space at 0x15000000 instead of
	    0x14000000, eg IBM WorkPad Z50.  */
	dev_ram_init(machine, 0x15000000, 0x1000000, DEV_RAM_MIRROR,
	    0x14000000, NULL);

	return d;
}
