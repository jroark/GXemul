/*
 *  Copyright (C) 2003-2021  Anders Gavare.  All rights reserved.
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
 *  MIPS16 slow interpreter.
 *
 *  This is a slow interpreter for the MIPS16 (compact 16-bit encoding)
 *  instruction set, following the same pattern as ARM Thumb support in
 *  cpu_arm.c.  When the CPU is in MIPS16 mode, the dyntrans execution
 *  loop calls this interpreter instead of the normal IC-based execution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "cpu.h"
#include "cop0.h"
#include "cpu_mips.h"
#include "machine.h"
#include "memory.h"
#include "misc.h"
#include "opcodes_mips16.h"
#include "symbol.h"
#include "wince_boot.h"


static const int mips16_reg_map[8] = MIPS16_REG_MAP;

#define	M16REG(x)	cpu->cd.mips.gpr[mips16_reg_map[(x) & 7]]

/*
 *  ROM MIPS16 call/return tracing (diagnostic, rate-limited).
 */
#define ROM_CALL_LOG_MAX  500
static int rom_call_log_count = 0;

/*
 *  SP change tracing — logs every SP modification during ROM MIPS16 execution.
 */
#define SP_TRACE_MAX  200
static int sp_trace_count = 0;
static uint32_t sp_trace_last = 0;

static void rom_m16_sp_trace(struct cpu *cpu)
{
	uint32_t pc32 = (uint32_t)cpu->pc;
	uint32_t cur_sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];

	if (pc32 < 0x9FC00000u || pc32 > 0x9FC03FFFu)
		return;
	if (cur_sp == sp_trace_last)
		return;
	if (sp_trace_count >= SP_TRACE_MAX)
		return;
	sp_trace_count++;
	fprintf(stderr,
	    "[SP_TRACE] PC=0x%08X SP: 0x%08X -> 0x%08X  RA=0x%08X #%d\n",
	    pc32, sp_trace_last, cur_sp,
	    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
	    sp_trace_count);
	sp_trace_last = cur_sp;
}

static void rom_m16_log_call(struct cpu *cpu, const char *type,
    uint32_t target)
{
	uint32_t pc32 = (uint32_t)cpu->pc;

	if (pc32 < 0x9FC00000u || pc32 > 0x9FC03FFFu)
		return;
	if (rom_call_log_count >= ROM_CALL_LOG_MAX)
		return;
	rom_call_log_count++;

	fprintf(stderr,
	    "[M16_CALL] %s PC=0x%08X->0x%08X SP=0x%08X RA=0x%08X"
	    " a0=0x%08X a1=0x%08X s0=0x%08X s1=0x%08X"
	    " v0=0x%08X v1=0x%08X #%d\n",
	    type, pc32, target,
	    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
	    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
	    (uint32_t)cpu->cd.mips.gpr[4],
	    (uint32_t)cpu->cd.mips.gpr[5],
	    (uint32_t)cpu->cd.mips.gpr[16],
	    (uint32_t)cpu->cd.mips.gpr[17],
	    (uint32_t)cpu->cd.mips.gpr[2],  /* v0 */
	    (uint32_t)cpu->cd.mips.gpr[3],  /* v1 */
	    rom_call_log_count);

	/* Dump stack when calling epilogue 0xAD0 */
	if (target == 0x9FC00AD0u) {
		uint32_t sp = (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP];
		fprintf(stderr, "[EPILOGUE_DUMP] SP=0x%08X"
		    " a0_delay=%d (delay slot LI not yet executed)\n",
		    sp, (int)(uint32_t)cpu->cd.mips.gpr[4]);
		/* Read 64 bytes from SP to show what epilogue will restore */
		for (int i = 0; i < 64; i += 4) {
			uint8_t buf[4];
			int ok2 = cpu->memory_rw(cpu, cpu->mem,
			    (uint64_t)(sp + i), buf, 4, MEM_READ,
			    CACHE_DATA);
			uint32_t val = buf[0] | (buf[1]<<8) |
			    (buf[2]<<16) | (buf[3]<<24);
			fprintf(stderr, "[EPILOGUE_DUMP] [SP+%02d] = "
			    "0x%08X (PA 0x%05X)\n",
			    i, val, (sp + i) & 0x1FFFFFFF);
			(void)ok2;
		}
	}
}

/*
 *  When a MIPS16 memory access fails, check if it was a TLB exception
 *  (EXL now set in Status).  If so, handle it directly by installing
 *  an identity-map TLB entry, clearing EXL, and retrying.  This avoids
 *  returning to the dyntrans loop for the general exception handler,
 *  which has issues with IC cache invalidation during TLB writes
 *  (GXemul flushes the IC cache on R4100 1KB TLB writes, preventing
 *  the handler's ERET from executing).
 *
 *  For non-TLB failures, kill the CPU.
 */
static void m16_tlb_fixup(struct cpu *cpu, uint64_t vaddr)
{
	/*  Create an identity-map 4KB TLB entry for the faulting page.  */
	struct mips_coproc *cp = cpu->cd.mips.coproc[0];
	uint64_t vpn2 = vaddr & ~(uint64_t)0x1FFF;  /* 4KB page aligned */
	uint64_t pfn0 = (vpn2 >> 12) << 6;
	uint64_t pfn1 = ((vpn2 + 0x1000) >> 12) << 6;

	cp->reg[COP0_ENTRYHI]  = vpn2 | (cp->reg[COP0_ENTRYHI] & 0xFF);
	cp->reg[COP0_ENTRYLO0] = pfn0 | 0x3F;  /* V D G C=3 */
	cp->reg[COP0_ENTRYLO1] = pfn1 | 0x3F;
	cp->reg[COP0_PAGEMASK] = 0x1800;  /* 4KB pages for R4100 */

	/*  Write to a random non-wired TLB entry.  */
	coproc_tlbwri(cpu, 1);  /* randomflag=1 → tlbwr */

	/*  Clear EXL to undo the exception state.  */
	cp->reg[COP0_STATUS] &= ~STATUS_EXL;
}

/*  Check result of a MIPS16 memory access.  If it was a TLB miss,
 *  install an identity-map 4KB entry and set m16_retry so the caller
 *  can re-execute the instruction.  For non-TLB exceptions, defer
 *  to the dyntrans exception handler.  For hard failures, stop.    */
#define	M16_CHECK_MEM_OK(ok)	do {					\
	if (!(ok)) {							\
		if (cpu->cd.mips.coproc[0]->reg[COP0_STATUS] & STATUS_EXL) { \
			uint32_t cause_exc = (cpu->cd.mips.coproc[0]	\
			    ->reg[COP0_CAUSE] >> 2) & 0x1F;		\
			if (cause_exc == EXCEPTION_TLBL ||		\
			    cause_exc == EXCEPTION_TLBS) {		\
				uint64_t bva = cpu->cd.mips.coproc[0]	\
				    ->reg[COP0_BADVADDR];		\
				m16_tlb_fixup(cpu, bva);		\
				return 1;  /* retry on next call */	\
			}						\
			return 1;  /* non-TLB exception: defer */	\
		}							\
		fprintf(stderr, "[MIPS16] fatal mem fail pc=0x%" PRIx64	\
		    " status=0x%08x\n", cpu->pc,			\
		    (uint32_t)cpu->cd.mips.coproc[0]->reg[COP0_STATUS]);\
		cpu->running = 0;					\
		return 0;						\
	}								\
} while (0)

/*  Sign-extend a value with 'bits' significant bits  */
#define	SIGN_EXTEND(val, bits)	\
	((int32_t)((val) << (32 - (bits))) >> (32 - (bits)))

/*  Regnames, for disassembly trace output  */
static const char *regnames[] = MIPS_REGISTER_NAMES;


/*
 *  mips_cpu_disassemble_instr_mips16():
 *
 *  Disassemble a MIPS16 instruction.
 */
int mips_cpu_disassemble_instr_mips16(struct cpu *cpu, unsigned char *ib,
	int running, uint64_t dumpaddr)
{
	uint16_t iw;
	int op, rx, ry, rz, sa, imm, func;

	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		iw = ib[0] | (ib[1] << 8);
	else
		iw = ib[1] | (ib[0] << 8);

	op = (iw >> 11) & 0x1f;
	rx = (iw >> 8) & 0x7;
	ry = (iw >> 5) & 0x7;
	rz = (iw >> 2) & 0x7;
	sa = (iw >> 2) & 0x7;
	imm = iw & 0xff;
	func = iw & 0x1f;

	debug("%04x\t", iw);

	switch (op) {
	case M16_OP_ADDIUSP:
		debug("addiu\t$%s, $sp, %i\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_ADDIUPC:
		debug("addiu\t$%s, $pc, %i\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_B:
		{
			int offset = SIGN_EXTEND(iw & 0x7ff, 11) << 1;
			debug("b\t0x%" PRIx64 "\n",
			    (uint64_t)(dumpaddr + 2 + offset));
		}
		break;
	case M16_OP_JAL:
		/*  32-bit instruction; need next halfword  */
		debug("jal/jalx (32-bit)\n");
		return 4;
	case M16_OP_BEQZ:
		{
			int offset = SIGN_EXTEND(imm, 8) << 1;
			debug("beqz\t$%s, 0x%" PRIx64 "\n",
			    regnames[mips16_reg_map[rx]],
			    (uint64_t)(dumpaddr + 2 + offset));
		}
		break;
	case M16_OP_BNEZ:
		{
			int offset = SIGN_EXTEND(imm, 8) << 1;
			debug("bnez\t$%s, 0x%" PRIx64 "\n",
			    regnames[mips16_reg_map[rx]],
			    (uint64_t)(dumpaddr + 2 + offset));
		}
		break;
	case M16_OP_SHIFT:
		{
			int type = iw & 0x3;
			if (sa == 0)
				sa = 8;
			switch (type) {
			case M16_SHIFT_SLL:
				debug("sll\t$%s, $%s, %i\n",
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]], sa);
				break;
			case M16_SHIFT_SRL:
				debug("srl\t$%s, $%s, %i\n",
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]], sa);
				break;
			case M16_SHIFT_SRA:
				debug("sra\t$%s, $%s, %i\n",
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]], sa);
				break;
			default:
				debug("UNIMPLEMENTED shift type %i\n", type);
			}
		}
		break;
	case M16_OP_RRIA:
		{
			int imm4 = SIGN_EXTEND((iw >> 0) & 0xf, 4);
			debug("addiu\t$%s, $%s, %i\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]], imm4);
		}
		break;
	case M16_OP_ADDIU8:
		debug("addiu\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]],
		    SIGN_EXTEND(imm, 8));
		break;
	case M16_OP_SLTI:
		debug("slti\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_SLTIU:
		debug("sltiu\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_I8:
		{
			int i8func = (iw >> 8) & 0x7;
			switch (i8func) {
			case M16_I8_BTEQZ:
				debug("bteqz\t0x%" PRIx64 "\n",
				    (uint64_t)(dumpaddr + 2 +
				    (SIGN_EXTEND(imm, 8) << 1)));
				break;
			case M16_I8_BTNEZ:
				debug("btnez\t0x%" PRIx64 "\n",
				    (uint64_t)(dumpaddr + 2 +
				    (SIGN_EXTEND(imm, 8) << 1)));
				break;
			case M16_I8_SWRASP:
				debug("sw\t$ra, %i($sp)\n", (imm << 2));
				break;
			case M16_I8_ADJSP:
				{
					int adj = SIGN_EXTEND(imm, 8) << 3;
					debug("addiu\t$sp, %i\n", adj);
				}
				break;
			case M16_I8_MOV32R:
				{
					int rz5 = iw & 0x1f;
					int r32 = (rz5 & ~3) |
					    ((rz5 & 1) << 1) |
					    ((rz5 >> 1) & 1);
					debug("mov32r\t$%s, $%s\n",
					    regnames[r32],
					    regnames[mips16_reg_map[ry]]);
				}
				break;
			case M16_I8_MOVR32:
				{
					int r32 = iw & 0x1f;
					debug("movr32\t$%s, $%s\n",
					    regnames[mips16_reg_map[ry]],
					    regnames[r32]);
				}
				break;
			default:
				debug("UNIMPLEMENTED I8 func %i\n", i8func);
			}
		}
		break;
	case M16_OP_LI:
		debug("li\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_CMPI:
		debug("cmpi\t$%s, %i\n",
		    regnames[mips16_reg_map[rx]], imm);
		break;
	case M16_OP_LB:
		debug("lb\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5),
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LH:
		debug("lh\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5) << 1,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LWSP:
		debug("lw\t$%s, %i($sp)\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_LW:
		debug("lw\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f) << 2,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LBU:
		debug("lbu\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f),
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LHU:
		debug("lhu\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f) << 1,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_LWPC:
		debug("lw\t$%s, %i($pc)\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_SB:
		debug("sb\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5),
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_SH:
		debug("sh\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    SIGN_EXTEND((iw & 0x1f), 5) << 1,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_SWSP:
		debug("sw\t$%s, %i($sp)\n",
		    regnames[mips16_reg_map[rx]], (imm << 2));
		break;
	case M16_OP_SW:
		debug("sw\t$%s, %i($%s)\n",
		    regnames[mips16_reg_map[ry]],
		    (iw & 0x1f) << 2,
		    regnames[mips16_reg_map[rx]]);
		break;
	case M16_OP_RRR:
		{
			int rrr_func = iw & 0x3;
			switch (rrr_func) {
			case M16_RRR_ADDU:
				debug("addu\t$%s, $%s, $%s\n",
				    regnames[mips16_reg_map[rz]],
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]]);
				break;
			case M16_RRR_SUBU:
				debug("subu\t$%s, $%s, $%s\n",
				    regnames[mips16_reg_map[rz]],
				    regnames[mips16_reg_map[rx]],
				    regnames[mips16_reg_map[ry]]);
				break;
			default:
				debug("UNIMPLEMENTED RRR func %i\n", rrr_func);
			}
		}
		break;
	case M16_OP_RR:
		switch (func) {
		case M16_RR_JR:
			if (rx == 0)
				debug("jr\t$ra\n");
			else
				debug("jr\t$%s\n",
				    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_JALR:
			debug("jalr\t$%s\n",
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_SLT:
			debug("slt\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_SLTU:
			debug("sltu\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_SLLV:
			debug("sllv\t$%s, $%s\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_BREAK:
			debug("break\t%i\n", (iw >> 5) & 0x3f);
			break;
		case M16_RR_SRLV:
			debug("srlv\t$%s, $%s\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_SRAV:
			debug("srav\t$%s, $%s\n",
			    regnames[mips16_reg_map[ry]],
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_CMP:
			debug("cmp\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_NEG:
			debug("neg\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_AND:
			debug("and\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_OR:
			debug("or\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_XOR:
			debug("xor\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_NOT:
			debug("not\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_MFHI:
			debug("mfhi\t$%s\n",
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_MFLO:
			debug("mflo\t$%s\n",
			    regnames[mips16_reg_map[rx]]);
			break;
		case M16_RR_MULT:
			debug("mult\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_MULTU:
			debug("multu\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_DIV:
			debug("div\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		case M16_RR_DIVU:
			debug("divu\t$%s, $%s\n",
			    regnames[mips16_reg_map[rx]],
			    regnames[mips16_reg_map[ry]]);
			break;
		default:
			debug("UNIMPLEMENTED RR func 0x%02x\n", func);
		}
		break;
	case M16_OP_EXTEND:
		debug("extend\t(prefix)\n");
		return 2;
	default:
		debug("UNIMPLEMENTED MIPS16 opcode 0x%02x\n", op);
	}

	return 2;
}


/*
 *  Helper: do a load/store via memory_rw.
 *  Returns 1 on success, 0 on failure (exception).
 */
static int m16_memory_rw(struct cpu *cpu, uint64_t addr,
	unsigned char *buf, int len, int writeflag)
{
	return cpu->memory_rw(cpu, cpu->mem, addr, buf, len,
	    writeflag ? MEM_WRITE : MEM_READ,
	    writeflag ? CACHE_DATA : CACHE_DATA);
}


/*
 *  Helper: read a 16-bit instruction halfword from memory.
 *  Returns the instruction word, or sets *ok = 0 on failure.
 */
static uint16_t m16_fetch(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t ib[2];
	if (!cpu->memory_rw(cpu, cpu->mem, addr, ib, sizeof(ib),
	    MEM_READ, CACHE_INSTRUCTION)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		return ib[0] | (ib[1] << 8);
	else
		return ib[1] | (ib[0] << 8);
}


/*
 *  Helper: read/write a 32-bit word from/to memory, handling endianness.
 */
static uint32_t m16_load_word(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t buf[4];
	if (!m16_memory_rw(cpu, addr, buf, 4, 0)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		return buf[0] | (buf[1] << 8) | (buf[2] << 16) |
		    (buf[3] << 24);
	else
		return buf[3] | (buf[2] << 8) | (buf[1] << 16) |
		    (buf[0] << 24);
}

/*
 *  Memory write watchpoint: log MIPS16 stores to the crash-area stack region.
 *  PA 0x8000-0x80C0 is kseg0 VA 0x80008000-0x800080C0.
 */
#define SW_WATCH_MAX 50
static int sw_watch_count = 0;

static int m16_store_word(struct cpu *cpu, uint64_t addr, uint32_t val)
{
	uint8_t buf[4];

	/* Watchpoint: detect writes near crash SP area */
	if (sw_watch_count < SW_WATCH_MAX) {
		uint32_t a32 = (uint32_t)addr;
		/* kseg0 (0x80000000-0x9FFFFFFF) → PA by masking top 3 bits */
		uint32_t pa = a32 & 0x1FFFFFFFu;
		if (pa >= 0x7F00u && pa < 0x8200u) {
			sw_watch_count++;
			fprintf(stderr,
			    "[SW_WATCH] PC=0x%08X SW [0x%08X]=0x%08X"
			    " (PA=0x%05X) SP=0x%08X #%d\n",
			    (uint32_t)cpu->pc, a32, val, pa,
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
			    sw_watch_count);
		}
	}

	if (cpu->byte_order == EMUL_LITTLE_ENDIAN) {
		buf[0] = val;
		buf[1] = val >> 8;
		buf[2] = val >> 16;
		buf[3] = val >> 24;
	} else {
		buf[3] = val;
		buf[2] = val >> 8;
		buf[1] = val >> 16;
		buf[0] = val >> 24;
	}
	return m16_memory_rw(cpu, addr, buf, 4, 1);
}

static uint32_t m16_load_half(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t buf[2];
	if (!m16_memory_rw(cpu, addr, buf, 2, 0)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN)
		return buf[0] | (buf[1] << 8);
	else
		return buf[1] | (buf[0] << 8);
}

static int m16_store_half(struct cpu *cpu, uint64_t addr, uint16_t val)
{
	uint8_t buf[2];
	if (cpu->byte_order == EMUL_LITTLE_ENDIAN) {
		buf[0] = val;
		buf[1] = val >> 8;
	} else {
		buf[1] = val;
		buf[0] = val >> 8;
	}
	return m16_memory_rw(cpu, addr, buf, 2, 1);
}

static uint32_t m16_load_byte(struct cpu *cpu, uint64_t addr, int *ok)
{
	uint8_t buf[1];
	if (!m16_memory_rw(cpu, addr, buf, 1, 0)) {
		*ok = 0;
		return 0;
	}
	*ok = 1;
	return buf[0];
}

static int m16_store_byte(struct cpu *cpu, uint64_t addr, uint8_t val)
{
	/* Watchpoint: detect byte writes near crash SP area */
	if (sw_watch_count < SW_WATCH_MAX) {
		uint32_t a32 = (uint32_t)addr;
		uint32_t pa = a32 & 0x1FFFFFFFu;
		if (pa >= 0x7F00u && pa < 0x8300u) {
			sw_watch_count++;
			fprintf(stderr,
			    "[SB_WATCH] PC=0x%08X SB [0x%08X]=0x%02X"
			    " (PA=0x%05X) SP=0x%08X #%d\n",
			    (uint32_t)cpu->pc, a32, val, pa,
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
			    sw_watch_count);
		}
	}
	return m16_memory_rw(cpu, addr, &val, 1, 1);
}


/*
 *  mips_cpu_interpret_mips16_SLOW():
 *
 *  Interpret a single MIPS16 instruction.  Called from the dyntrans
 *  execution loop when cpu->cd.mips.mips16 is set.
 *
 *  Returns 1 on success, 0 on failure.
 */
int mips_cpu_interpret_mips16_SLOW(struct cpu *cpu)
{
	uint16_t iw;
	uint64_t addr = cpu->pc;
	int op, rx, ry, rz, sa, func;
	int ok = 1;
	int extended = 0;
	uint16_t extend_word = 0;

	/* Debug: track s0 at MIPS16 interpreter entry for 0x10F4 */
	{
		static int entry_s0_count = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		if (rpc == 0x10F4u && entry_s0_count < 3) {
			entry_s0_count++;
			fprintf(stderr,
			    "[M16_ENTRY] PC=0x%08X s0=0x%08X"
			    " a0=0x%08X delay=%d #%d\n",
			    (uint32_t)cpu->pc,
			    (uint32_t)cpu->cd.mips.gpr[16],
			    (uint32_t)cpu->cd.mips.gpr[4],
			    cpu->delay_slot,
			    entry_s0_count);
		}
	}

	/*
	 *  JAL/JALX delay slot completion: if we just executed the
	 *  delay slot instruction (delay_slot == DELAYED), redirect
	 *  PC to the saved branch target now.
	 */
	if (cpu->delay_slot == DELAYED) {
		uint64_t target = cpu->cd.mips.m16_delay_target;
		int mode = cpu->cd.mips.m16_delay_jalx;
		cpu->delay_slot = NOT_DELAYED;
		cpu->cd.mips.m16_delay_jalx = 0;

		/* Debug: track $a0 changes through delay slot */
		{
			static int ds_a0_count = 0;
			uint32_t t32 = (uint32_t)(target & ~(uint64_t)1);
			if ((t32 & 0x3FFF) == 0x13F0 && ds_a0_count < 5) {
				ds_a0_count++;
				fprintf(stderr,
				    "[DS_COMPLETE] target=0x%08X"
				    " a0=0x%08X mode=%d #%d\n",
				    t32,
				    (uint32_t)cpu->cd.mips.gpr[4],
				    mode, ds_a0_count);
			}
		}

		/* Debug: track s0 around JALX to prologue 0xA98 */
		{
			static int jalx_s0_count = 0;
			uint32_t t32 = (uint32_t)(target & ~(uint64_t)1);
			if (mode == 1 && (t32 & 0x3FFF) == 0x0A98 &&
			    jalx_s0_count < 10) {
				jalx_s0_count++;
				fprintf(stderr,
				    "[JALX_OUT] target=0x%08X"
				    " s0=0x%08X a0=0x%08X"
				    " PC_was=0x%08X #%d\n",
				    t32,
				    (uint32_t)cpu->cd.mips.gpr[16],
				    (uint32_t)cpu->cd.mips.gpr[4],
				    (uint32_t)cpu->pc,
				    jalx_s0_count);
			}
		}

		switch (mode) {
		case 0:
			/*  JAL: stay in MIPS16 mode, target is
			 *  a plain address (bit 0 not meaningful)  */
			cpu->pc = target;
			break;
		case 1:
			/*  JALX: switch to MIPS32 mode  */
			cpu->cd.mips.mips16 = 0;
			cpu->pc = target;
			break;
		case 2:
			/*  JR/JALR: mode determined by target bit 0  */
			if (target & 1) {
				cpu->pc = target & ~(uint64_t)1;
			} else {
				cpu->cd.mips.mips16 = 0;
				cpu->pc = target;
			}
			break;
		}
		return 1;
	}

	if (!cpu->cd.mips.mips16) {
		fatal("mips_cpu_interpret_mips16_SLOW called when not in "
		    "MIPS16 mode?\n");
		cpu->running = 0;
		return 0;
	}

	/* Debug: detect when PC enters 0x13F0 */
	{
		static int pc13f0_count = 0;
		if (((uint32_t)cpu->pc & 0x3FFF) == 0x13F0 && pc13f0_count < 10) {
			pc13f0_count++;
			fprintf(stderr,
			    "[PC_13F0] PC=0x%08X a0=0x%08X SP=0x%08X #%d\n",
			    (uint32_t)cpu->pc,
			    (uint32_t)cpu->cd.mips.gpr[4],
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
			    pc13f0_count);
		}
	}

	/* Diagnostic: watchpoint on $s3 changes (rate-limited) */
	{
		static uint32_t last_s3 = 0;
		static int s3_watch_count = 0;
		uint32_t cur_s3 = (uint32_t)cpu->cd.mips.gpr[19];
		if (cur_s3 != last_s3 && s3_watch_count < 10) {
			s3_watch_count++;
			fprintf(stderr,
			    "[S3_WATCH] PC=0x%08X $s3: 0x%08X -> 0x%08X #%d\n",
			    (uint32_t)cpu->pc, last_s3, cur_s3,
			    s3_watch_count);
			last_s3 = cur_s3;
		}
	}

	/* Diagnostic: trace SP changes from previous instruction */
	rom_m16_sp_trace(cpu);

	/* Diagnostic: watch $a0 changes in the callback processing range */
	{
		static uint32_t a0_watch_last = 0xDEADDEAD;
		static int a0_watch_count = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		uint32_t cur_a0 = (uint32_t)cpu->cd.mips.gpr[4];
		if (rpc >= 0x1160u && rpc <= 0x11C0u &&
		    cur_a0 != a0_watch_last && a0_watch_count < 20) {
			a0_watch_count++;
			fprintf(stderr,
			    "[A0_WATCH] PC=0x%08X $a0: 0x%08X -> 0x%08X"
			    " delay=%d #%d\n",
			    (uint32_t)cpu->pc, a0_watch_last, cur_a0,
			    cpu->delay_slot, a0_watch_count);
			a0_watch_last = cur_a0;
		}
	}

	/* Check for DMA autocopy at ROM DMA polling function entry */
	wince_boot_check_dma_autocopy(cpu);

	/* Debug: s0 checkpoint before fetch for 0x10F4 */
	{
		static int s0chk = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		if (rpc == 0x10F4u && s0chk < 3) {
			s0chk++;
			fprintf(stderr,
			    "[S0_PRE_FETCH] PC=0x%08X s0=0x%08X #%d\n",
			    (uint32_t)cpu->pc,
			    (uint32_t)cpu->cd.mips.gpr[16],
			    s0chk);
		}
	}

	/*  Fetch the instruction  */
	iw = m16_fetch(cpu, addr, &ok);
	if (!ok) {
		if (cpu->cd.mips.coproc[0]->reg[COP0_STATUS] & STATUS_EXL)
			return 1;
		fatal("mips_cpu_interpret_mips16_SLOW(): could not read "
		    "the instruction at 0x%" PRIx64 "\n", addr);
		cpu->running = 0;
		return 0;
	}

	op = (iw >> 11) & 0x1f;

	/*  Check for EXTEND prefix  */
	if (op == M16_OP_EXTEND) {
		extend_word = iw;
		addr += 2;
		iw = m16_fetch(cpu, addr, &ok);
		if (!ok) {
			if (cpu->cd.mips.coproc[0]->reg[COP0_STATUS] & STATUS_EXL)
				return 1;
			fatal("mips_cpu_interpret_mips16_SLOW(): could not "
			    "read extended instruction\n");
			cpu->running = 0;
			return 0;
		}
		op = (iw >> 11) & 0x1f;
		extended = 1;
	}

	/*  Instruction trace  */
	if (cpu->machine->instruction_trace) {
		uint64_t offset;
		char *symbol = get_symbol_name(&cpu->machine->symbol_context,
		    cpu->pc, &offset);
		if (symbol != NULL && offset == 0)
			debug("<%s>\n", symbol);
		if (cpu->machine->ncpus > 1)
			debug("cpu%i:\t", cpu->cpu_id);
		debug("%08" PRIx64 ":  ", cpu->pc);
		if (extended) {
			uint8_t ib[2];
			ib[0] = extend_word & 0xff;
			ib[1] = (extend_word >> 8) & 0xff;
			if (cpu->byte_order != EMUL_LITTLE_ENDIAN) {
				ib[0] = (extend_word >> 8) & 0xff;
				ib[1] = extend_word & 0xff;
			}
			mips_cpu_disassemble_instr_mips16(cpu, ib, 1, cpu->pc);
		}
		{
			uint8_t ib[2];
			if (cpu->byte_order == EMUL_LITTLE_ENDIAN) {
				ib[0] = iw & 0xff;
				ib[1] = (iw >> 8) & 0xff;
			} else {
				ib[0] = (iw >> 8) & 0xff;
				ib[1] = iw & 0xff;
			}
			mips_cpu_disassemble_instr_mips16(cpu, ib, 1,
			    extended ? cpu->pc + 2 : cpu->pc);
		}
	}

	cpu->ninstrs ++;

	rx = (iw >> 8) & 0x7;
	ry = (iw >> 5) & 0x7;
	rz = (iw >> 2) & 0x7;
	sa = (iw >> 2) & 0x7;
	func = iw & 0x1f;

	/* Targeted instruction trace: ROM MIPS16 0x10EC-0x1120 */
	{
		static int rom_itrace_count = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		if (rpc >= 0x10ECu && rpc <= 0x11C0u &&
		    rom_itrace_count < 200) {
			rom_itrace_count++;
			fprintf(stderr,
			    "[M16_ITRACE] PC=0x%08X iw=0x%04X op=%d"
			    " ext=%d extw=0x%04X SP=0x%08X"
			    " v0=0x%08X v1=0x%08X"
			    " a0=0x%08X a1=0x%08X"
			    " a2=0x%08X a3=0x%08X"
			    " s0=0x%08X s1=0x%08X"
			    " t8=0x%08X #%d\n",
			    (uint32_t)cpu->pc, iw, op,
			    extended, extend_word,
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
			    (uint32_t)cpu->cd.mips.gpr[2],
			    (uint32_t)cpu->cd.mips.gpr[3],
			    (uint32_t)cpu->cd.mips.gpr[4],
			    (uint32_t)cpu->cd.mips.gpr[5],
			    (uint32_t)cpu->cd.mips.gpr[6],
			    (uint32_t)cpu->cd.mips.gpr[7],
			    (uint32_t)cpu->cd.mips.gpr[16],
			    (uint32_t)cpu->cd.mips.gpr[17],
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T8],
			    rom_itrace_count);
		}
	}

	/* Full register dump for critical range 0x110C-0x1116 */
	{
		static int regdump_count = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		if (rpc >= 0x110Cu && rpc <= 0x1116u &&
		    regdump_count < 20) {
			regdump_count++;
			fprintf(stderr,
			    "[M16_REGDUMP] PC=0x%08X"
			    " s0=0x%08X s1=0x%08X v0=0x%08X v1=0x%08X"
			    " a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X"
			    " t8=0x%08X ra=0x%08X SP=0x%08X #%d\n",
			    (uint32_t)cpu->pc,
			    (uint32_t)cpu->cd.mips.gpr[16],
			    (uint32_t)cpu->cd.mips.gpr[17],
			    (uint32_t)cpu->cd.mips.gpr[2],
			    (uint32_t)cpu->cd.mips.gpr[3],
			    (uint32_t)cpu->cd.mips.gpr[4],
			    (uint32_t)cpu->cd.mips.gpr[5],
			    (uint32_t)cpu->cd.mips.gpr[6],
			    (uint32_t)cpu->cd.mips.gpr[7],
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_T8],
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_RA],
			    (uint32_t)cpu->cd.mips.gpr[MIPS_GPR_SP],
			    regdump_count);
		}
	}

	/* FIFO loop diagnostic: log s1 (store target) at 0x13A4 */
	{
		static int fifo_loop_count = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		if (rpc == 0x13A4u && fifo_loop_count < 5) {
			fifo_loop_count++;
			fprintf(stderr,
			    "[FIFO_LOOP] PC=0x%08X s1=0x%08X"
			    " v0=0x%08X v1=0x%08X #%d\n",
			    (uint32_t)cpu->pc,
			    (uint32_t)cpu->cd.mips.gpr[17],
			    (uint32_t)cpu->cd.mips.gpr[2],
			    (uint32_t)cpu->cd.mips.gpr[3],
			    fifo_loop_count);
		}
	}

	/* Dump NAND buffer and TLB at function 0x10EC entry */
	{
		static int bufdump_done = 0;
		uint32_t rpc = (uint32_t)cpu->pc & 0x3FFF;
		if (rpc == 0x10ECu && !bufdump_done) {
			bufdump_done = 1;
			uint32_t buf_va = 0x80010060u;
			fprintf(stderr, "[BUF_DUMP] PC=0x%08X"
			    " dumping MEM[0x%08X]:\n",
			    (uint32_t)cpu->pc, buf_va);
			for (int j = 0; j < 64; j += 4) {
				uint32_t w = m16_load_word(cpu,
				    (uint64_t)(buf_va + j), &ok);
				fprintf(stderr, "[BUF_DUMP]   [+%02X]"
				    " = 0x%08X\n", j, w);
			}
			/* Dump all TLB entries */
			int ntlb = cpu->cd.mips.cpu_type
			    .nr_of_tlb_entries;
			fprintf(stderr, "[TLB_DUMP] %d entries"
			    " (Wired=%d):\n", ntlb,
			    (int)cpu->cd.mips.coproc[0]
			    ->reg[COP0_WIRED]);
			for (int t = 0; t < ntlb; t++) {
				uint64_t hi = cpu->cd.mips
				    .coproc[0]->tlbs[t].hi;
				uint64_t lo0 = cpu->cd.mips
				    .coproc[0]->tlbs[t].lo0;
				uint64_t lo1 = cpu->cd.mips
				    .coproc[0]->tlbs[t].lo1;
				uint64_t mask = cpu->cd.mips
				    .coproc[0]->tlbs[t].mask;
				if (lo0 || lo1 || hi)
					fprintf(stderr,
					    "[TLB_DUMP]   [%2d]"
					    " hi=0x%08X"
					    " lo0=0x%08X"
					    " lo1=0x%08X"
					    " mask=0x%08X\n",
					    t,
					    (uint32_t)hi,
					    (uint32_t)lo0,
					    (uint32_t)lo1,
					    (uint32_t)mask);
			}
		}
	}

	switch (op) {

	case M16_OP_ADDIUSP:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			M16REG(rx) = (int32_t)(
			    (int32_t)cpu->cd.mips.gpr[MIPS_GPR_SP] + imm8);
		}
		break;

	case M16_OP_ADDIUPC:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			M16REG(rx) = (int32_t)(
			    (int32_t)(cpu->pc & ~(uint32_t)3) + imm8);
		}
		break;

	case M16_OP_B:
		{
			int offset = iw & 0x7ff;
			if (extended) {
				offset = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				offset = SIGN_EXTEND(offset, 16);
			} else {
				offset = SIGN_EXTEND(offset, 11);
			}
			offset <<= 1;
			cpu->pc = cpu->pc + 2 + offset;
			return 1;
		}

	case M16_OP_JAL:
		{
			/*
			 *  JAL/JALX: This is a 32-bit instruction.
			 *  The first halfword has been consumed as the EXTEND
			 *  check above, but JAL uses a different encoding.
			 *  We need to re-decode: the full 32 bits are:
			 *    [15:11]=00011 [10:0]=target_hi
			 *    [15:0]=target_lo
			 *
			 *  Actually for JAL, the first word IS the JAL opcode
			 *  (not EXTEND).  So if we got here via EXTEND, that's
			 *  wrong.  JAL is always 32-bit.
			 */
			uint16_t hi_word, lo_word;
			uint32_t target;
			int jalx;

			if (extended) {
				/*  We consumed EXTEND then got JAL as next -
				    this shouldn't happen in valid code.  */
				fatal("EXTEND + JAL: invalid\n");
				cpu->running = 0;
				return 0;
			}

			/*  First halfword is iw (the JAL opcode word)  */
			hi_word = iw;
			/*  Fetch second halfword  */
			lo_word = m16_fetch(cpu, cpu->pc + 2, &ok);
			if (!ok) {
				if (cpu->cd.mips.coproc[0]->reg[COP0_STATUS] & STATUS_EXL)
					return 1;
				fatal("mips_cpu_interpret_mips16_SLOW(): "
				    "could not read JAL second halfword\n");
				cpu->running = 0;
				return 0;
			}

			/*  Bit 10 of hi_word = X bit (JALX vs JAL)  */
			jalx = (hi_word >> 10) & 1;
			/*
			 *  The 26-bit target is encoded across both
			 *  halfwords with bits [20:16] and [25:21]
			 *  swapped relative to the standard MIPS J
			 *  encoding:
			 *
			 *  hi_word[9:5]  -> target[20:16]
			 *  hi_word[4:0]  -> target[25:21]
			 *  lo_word[15:0] -> target[15:0]
			 */
			target = ((uint32_t)(hi_word & 0x1f) << 21) |
			    ((uint32_t)((hi_word >> 5) & 0x1f) << 16) |
			    lo_word;

			/*  Link: RA = address after the 2-byte delay slot
			 *  (PC + 4 for the 32-bit JAL + 2 for delay slot).
			 *  Set bit 0 to indicate MIPS16 return mode.  */
			cpu->cd.mips.gpr[MIPS_GPR_RA] =
			    (cpu->pc + 6) | 1;

			/*  Compute target: 26-bit field shifted left 2  */
			target <<= 2;
			cpu->cd.mips.m16_delay_target =
			    ((cpu->pc + 2) &
			    ~(uint64_t)0x0fffffff) | target;
			cpu->cd.mips.m16_delay_jalx = jalx;

			/*  Advance PC to delay slot (PC + 4).  The next
			 *  interpreter call executes the delay slot, then
			 *  the completion code at the top redirects to
			 *  the branch target.  */
			cpu->pc += 4;
			cpu->delay_slot = TO_BE_DELAYED;

			if (cpu->machine->show_trace_tree)
				cpu_functioncall_trace(cpu,
				    cpu->cd.mips.m16_delay_target);
			rom_m16_log_call(cpu,
			    jalx ? "JALX" : "JAL ",
			    (uint32_t)cpu->cd.mips.m16_delay_target);
			return 1;
		}

	case M16_OP_BEQZ:
		{
			int offset = iw & 0xff;
			if (extended) {
				offset = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				offset = SIGN_EXTEND(offset, 16);
			} else {
				offset = SIGN_EXTEND(offset, 8);
			}
			offset <<= 1;
			if (M16REG(rx) == 0) {
				cpu->pc = cpu->pc + 2 + offset;
				return 1;
			}
		}
		break;

	case M16_OP_BNEZ:
		{
			int offset = iw & 0xff;
			if (extended) {
				offset = ((extend_word & 0x1f) << 11) |
				    (iw & 0x7ff);
				offset = SIGN_EXTEND(offset, 16);
			} else {
				offset = SIGN_EXTEND(offset, 8);
			}
			offset <<= 1;
			if (M16REG(rx) != 0) {
				cpu->pc = cpu->pc + 2 + offset;
				return 1;
			}
		}
		break;

	case M16_OP_SHIFT:
		{
			int type = iw & 0x3;
			int amount = sa;
			if (extended) {
				amount = (extend_word >> 6) & 0x1f;
			} else {
				if (amount == 0)
					amount = 8;
			}
			switch (type) {
			case M16_SHIFT_SLL:
				M16REG(rx) = (int32_t)(
				    (uint32_t)M16REG(ry) << amount);
				break;
			case M16_SHIFT_SRL:
				M16REG(rx) = (int32_t)(
				    (uint32_t)M16REG(ry) >> amount);
				break;
			case M16_SHIFT_SRA:
				M16REG(rx) = (int32_t)(
				    (int32_t)M16REG(ry) >> amount);
				break;
			default:
				fatal("UNIMPLEMENTED MIPS16 shift type %i "
				    "at 0x%" PRIx64 "\n", type, cpu->pc);
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_RRIA:
		{
			int imm4;
			if (extended) {
				imm4 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm4 = SIGN_EXTEND(imm4, 16);
			} else {
				imm4 = SIGN_EXTEND(iw & 0xf, 4);
			}
			M16REG(ry) = (int32_t)(
			    (int32_t)M16REG(rx) + imm4);
		}
		break;

	case M16_OP_ADDIU8:
		{
			int imm8;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 = SIGN_EXTEND(iw & 0xff, 8);
			}
			M16REG(rx) = (int32_t)(
			    (int32_t)M16REG(rx) + imm8);
		}
		break;

	case M16_OP_SLTI:
		{
			int imm8;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 = iw & 0xff;
			}
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((int32_t)M16REG(rx) < imm8) ? 1 : 0;
		}
		break;

	case M16_OP_SLTIU:
		{
			uint32_t imm8;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 = iw & 0xff;
			}
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) < imm8) ? 1 : 0;
		}
		break;

	case M16_OP_I8:
		{
			int i8func = (iw >> 8) & 0x7;
			int imm8 = iw & 0xff;
			switch (i8func) {
			case M16_I8_BTEQZ:
				{
					int offset;
					if (extended) {
						offset = ((extend_word & 0x1f)
						    << 11) | (iw & 0x7ff);
						offset = SIGN_EXTEND(offset, 16);
					} else {
						offset = SIGN_EXTEND(imm8, 8);
					}
					offset <<= 1;
					/* Diagnostic: BTEQZ in ROM range */
					{
						static int bteqz_diag = 0;
						uint32_t rpc =
						    (uint32_t)cpu->pc & 0x3FFF;
						if (rpc >= 0x1110u &&
						    rpc <= 0x1120u &&
						    bteqz_diag < 5) {
							bteqz_diag++;
							fprintf(stderr,
							    "[BTEQZ_DIAG]"
							    " PC=0x%08X"
							    " t8=0x%016"
							    PRIx64
							    " t8_32=0x%08X"
							    " taken=%d"
							    " target=0x%08X"
							    " #%d\n",
							    (uint32_t)cpu->pc,
							    cpu->cd.mips.gpr
							    [MIPS_GPR_T8],
							    (uint32_t)cpu->cd
							    .mips.gpr
							    [MIPS_GPR_T8],
							    (cpu->cd.mips.gpr
							    [MIPS_GPR_T8]
							    == 0) ? 1 : 0,
							    (uint32_t)(cpu->pc
							    + 2 + offset),
							    bteqz_diag);
						}
					}
					if (cpu->cd.mips.gpr[MIPS_GPR_T8]
					    == 0) {
						cpu->pc = cpu->pc + 2 +
						    offset;
						return 1;
					}
				}
				break;
			case M16_I8_BTNEZ:
				{
					int offset;
					if (extended) {
						offset = ((extend_word & 0x1f)
						    << 11) | (iw & 0x7ff);
						offset = SIGN_EXTEND(offset, 16);
					} else {
						offset = SIGN_EXTEND(imm8, 8);
					}
					offset <<= 1;
					if (cpu->cd.mips.gpr[MIPS_GPR_T8]
					    != 0) {
						cpu->pc = cpu->pc + 2 +
						    offset;
						return 1;
					}
				}
				break;
			case M16_I8_SWRASP:
				{
					int offset8;
					if (extended) {
						offset8 = ((extend_word & 0x1f)
						    << 11) |
						    ((extend_word >> 5) & 0x3f)
						    << 5 | (iw & 0x1f);
						offset8 = SIGN_EXTEND(
						    offset8, 16);
					} else {
						offset8 = imm8 << 2;
					}
					uint64_t a =
					    cpu->cd.mips.gpr[MIPS_GPR_SP] +
					    offset8;
					{
						int sw_ok = m16_store_word(cpu, a,
						    cpu->cd.mips.gpr[MIPS_GPR_RA]);
						M16_CHECK_MEM_OK(sw_ok);
					}
				}
				break;
			case M16_I8_ADJSP:
				{
					int adj;
					if (extended) {
						adj = ((extend_word & 0x1f)
						    << 11) |
						    ((extend_word >> 5) & 0x3f)
						    << 5 | (iw & 0x1f);
						adj = SIGN_EXTEND(adj, 16);
					} else {
						adj = SIGN_EXTEND(imm8, 8)
						    << 3;
					}
					cpu->cd.mips.gpr[MIPS_GPR_SP] =
					    (int32_t)(
					    (int32_t)cpu->cd.mips.
					    gpr[MIPS_GPR_SP] + adj);
				}
				break;
			case M16_I8_MOV32R:
				{
					/*
					 *  MOV32R: move MIPS16 reg to
					 *  any MIPS32 register.
					 *  [7:5] = MIPS16 source reg (ry)
					 *  [4:0] = MIPS32 dest reg, encoded
					 *  with bits [1:0] swapped vs normal.
					 */
					int rz5 = iw & 0x1f;
					int r32 = (rz5 & ~3) |
					    ((rz5 & 1) << 1) |
					    ((rz5 >> 1) & 1);
					cpu->cd.mips.gpr[r32] =
					    M16REG(ry);
				}
				break;
			case M16_I8_MOVR32:
				{
					/*  MOVR32: move MIPS32 reg to
					 *  MIPS16 reg.
					 *  [4:0] = MIPS32 source reg (direct,
					 *  NOT rearranged like MOV32R). */
					int r32 = iw & 0x1f;
					M16REG(ry) = cpu->cd.mips.gpr[r32];
				}
				break;
			default:
				fatal("UNIMPLEMENTED MIPS16 I8 func %i "
				    "at 0x%" PRIx64 "\n", i8func, cpu->pc);
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_LI:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				/*  Register-immediate encoding:
				 *  imm = extend[4:0]<<11 | extend[10:5]<<5 | iw[4:0]
				 *  (NOT branch encoding which uses iw[10:0])  */
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			}
			M16REG(rx) = (int32_t)imm8;
		}
		break;

	case M16_OP_CMPI:
		{
			int imm8 = iw & 0xff;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			}
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) ^ (uint32_t)imm8);
		}
		break;

	case M16_OP_LB:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5);
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_byte(cpu, a, &ok);
			M16_CHECK_MEM_OK(ok);
			M16REG(ry) = (int32_t)(int8_t)val;
		}
		break;

	case M16_OP_LH:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5) << 1;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_half(cpu, a, &ok);
			M16_CHECK_MEM_OK(ok);
			M16REG(ry) = (int32_t)(int16_t)val;
		}
		break;

	case M16_OP_LWSP:
		{
			int imm8 = iw & 0xff;
			uint64_t a;
			uint32_t val;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)
			    cpu->cd.mips.gpr[MIPS_GPR_SP] + imm8);
			val = m16_load_word(cpu, a, &ok);
			M16_CHECK_MEM_OK(ok);
			M16REG(rx) = (int32_t)val;
		}
		break;

	case M16_OP_LW:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_word(cpu, a, &ok);
			M16_CHECK_MEM_OK(ok);
			M16REG(ry) = (int32_t)val;
		}
		break;

	case M16_OP_LBU:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				/*  unsigned offset, not shifted  */
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			/* Debug: track LBU at 0x117E */
			{
				static int lbu_diag = 0;
				uint32_t rpc =
				    (uint32_t)cpu->pc & 0x3FFF;
				if (rpc == 0x117Eu && lbu_diag < 5) {
					uint32_t a0_pre =
					    (uint32_t)cpu->cd.mips
					    .gpr[4];
					fprintf(stderr,
					    "[LBU_117E] BEFORE"
					    " a0=0x%08X a3=0x%08X"
					    " addr=0x%08X"
					    " EXL=%d #%d\n",
					    a0_pre,
					    (uint32_t)M16REG(rx),
					    (uint32_t)a,
					    (int)((cpu->cd.mips
					    .coproc[0]->reg
					    [COP0_STATUS] &
					    STATUS_EXL) ? 1 : 0),
					    lbu_diag + 1);
				}
			}
			val = m16_load_byte(cpu, a, &ok);
			/* Debug: after load at 0x117E */
			{
				static int lbu_diag2 = 0;
				uint32_t rpc =
				    (uint32_t)cpu->pc & 0x3FFF;
				if (rpc == 0x117Eu && lbu_diag2 < 5) {
					lbu_diag2++;
					fprintf(stderr,
					    "[LBU_117E] AFTER"
					    " a0=0x%08X ok=%d"
					    " val=0x%02X"
					    " EXL=%d #%d\n",
					    (uint32_t)cpu->cd.mips
					    .gpr[4],
					    ok, val & 0xFF,
					    (int)((cpu->cd.mips
					    .coproc[0]->reg
					    [COP0_STATUS] &
					    STATUS_EXL) ? 1 : 0),
					    lbu_diag2);
				}
			}
			M16_CHECK_MEM_OK(ok);
			M16REG(ry) = val;
		}
		break;

	case M16_OP_LHU:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			uint32_t val;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 <<= 1;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			val = m16_load_half(cpu, a, &ok);
			M16_CHECK_MEM_OK(ok);
			M16REG(ry) = val;
		}
		break;

	case M16_OP_LWPC:
		{
			int imm8 = iw & 0xff;
			uint64_t a;
			uint32_t val;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			a = (cpu->pc & ~(uint64_t)3) + imm8;
			val = m16_load_word(cpu, a, &ok);
			M16_CHECK_MEM_OK(ok);
			M16REG(rx) = (int32_t)val;
		}
		break;

	case M16_OP_SB:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5);
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			if (!m16_store_byte(cpu, a, M16REG(ry))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_SH:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 = SIGN_EXTEND(offset5, 5) << 1;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			if (!m16_store_half(cpu, a, M16REG(ry))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_SWSP:
		{
			int imm8 = iw & 0xff;
			uint64_t a;
			if (extended) {
				imm8 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				imm8 = SIGN_EXTEND(imm8, 16);
			} else {
				imm8 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)
			    cpu->cd.mips.gpr[MIPS_GPR_SP] + imm8);
			if (!m16_store_word(cpu, a, M16REG(rx))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_SW:
		{
			int offset5 = iw & 0x1f;
			uint64_t a;
			if (extended) {
				offset5 = ((extend_word & 0x1f) << 11) |
				    ((extend_word >> 5) & 0x3f) << 5 |
				    (iw & 0x1f);
				offset5 = SIGN_EXTEND(offset5, 16);
			} else {
				offset5 <<= 2;
			}
			a = (uint64_t)((int64_t)(int32_t)M16REG(rx) +
			    offset5);
			if (!m16_store_word(cpu, a, M16REG(ry))) {
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_RRR:
		{
			int rrr_func = iw & 0x3;
			switch (rrr_func) {
			case M16_RRR_ADDU:
				M16REG(rz) = (int32_t)(
				    (uint32_t)M16REG(rx) +
				    (uint32_t)M16REG(ry));
				break;
			case M16_RRR_SUBU:
				{
					static int subu_diag = 0;
					uint32_t rpc =
					    (uint32_t)cpu->pc & 0x3FFF;
					uint32_t s1v =
					    (uint32_t)M16REG(rx);
					uint32_t s2v =
					    (uint32_t)M16REG(ry);
					uint32_t res = s1v - s2v;
					if (rpc >= 0x1170u &&
					    rpc <= 0x1180u &&
					    subu_diag < 5) {
						subu_diag++;
						fprintf(stderr,
						    "[RRR_SUBU]"
						    " PC=0x%08X"
						    " rx=%d(r%d)"
						    "=0x%08X"
						    " ry=%d(r%d)"
						    "=0x%08X"
						    " rz=%d(r%d)"
						    " res=0x%08X"
						    " #%d\n",
						    (uint32_t)cpu->pc,
						    rx,
						    mips16_reg_map[rx],
						    s1v,
						    ry,
						    mips16_reg_map[ry],
						    s2v,
						    rz,
						    mips16_reg_map[rz],
						    res,
						    subu_diag);
					}
					M16REG(rz) = (int32_t)res;
				}
				break;
			default:
				fatal("UNIMPLEMENTED MIPS16 RRR func %i "
				    "at 0x%" PRIx64 "\n", rrr_func, cpu->pc);
				cpu->running = 0;
				return 0;
			}
		}
		break;

	case M16_OP_RR:
		switch (func) {
		case M16_RR_JR:
			{
				uint64_t target;
				/*
				 *  MIPS16 JR/JALR share func=0.
				 *  The ry field [7:5] distinguishes them:
				 *    ry=000: JR rx (no link)
				 *    ry=010: JALR rx (link RA)
				 *  When rx=0: JR $ra (regardless of ry)
				 */
				int is_jalr = (ry == 2 && rx != 0);
				if (rx == 0) {
					/*  JR $ra  */
					target = cpu->cd.mips.gpr[MIPS_GPR_RA];
				} else {
					target = M16REG(rx);
				}
				if (is_jalr) {
					/*  JALR: link RA = PC + 4
					 *  (PC+2 for insn + 2 for delay slot)  */
					cpu->cd.mips.gpr[MIPS_GPR_RA] =
					    (cpu->pc + 4) | 1;
				}
				/*  Both JR and JALR have a delay slot.  */
				cpu->cd.mips.m16_delay_target = target;
				cpu->cd.mips.m16_delay_jalx = 2;  /* use bit 0 */
				cpu->pc += 2;  /*  advance to delay slot  */
				cpu->delay_slot = TO_BE_DELAYED;
				if (!is_jalr && rx == 0 &&
				    cpu->machine->show_trace_tree)
					cpu_functioncall_trace_return(cpu);
				else if (is_jalr &&
				    cpu->machine->show_trace_tree)
					cpu_functioncall_trace(cpu,
					    target & ~(uint64_t)1);
				if (is_jalr)
					rom_m16_log_call(cpu, "JALR",
					    (uint32_t)(target & ~(uint64_t)1));
				else if (rx == 0)
					rom_m16_log_call(cpu, "RET ",
					    (uint32_t)(target & ~(uint64_t)1));
				else
					rom_m16_log_call(cpu, "JR  ",
					    (uint32_t)(target & ~(uint64_t)1));
				return 1;
			}
		case M16_RR_JALR:
			{
				uint64_t target = M16REG(rx);
				/*  Link: RA = PC + 4, with MIPS16 bit
				 *  (PC+2 for JR insn + 2 for delay slot)  */
				cpu->cd.mips.gpr[MIPS_GPR_RA] =
				    (cpu->pc + 4) | 1;
				/*  JALR has a one-instruction delay slot.  */
				cpu->cd.mips.m16_delay_target = target;
				cpu->cd.mips.m16_delay_jalx = 2;  /* JR/JALR: use bit 0 */
				cpu->pc += 2;  /*  advance to delay slot  */
				cpu->delay_slot = TO_BE_DELAYED;
				if (cpu->machine->show_trace_tree)
					cpu_functioncall_trace(cpu, cpu->pc);
				rom_m16_log_call(cpu, "JALR",
				    (uint32_t)(target & ~(uint64_t)1));
				return 1;
			}
		case M16_RR_SLT:
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((int32_t)M16REG(rx) < (int32_t)M16REG(ry))
			    ? 1 : 0;
			break;
		case M16_RR_SLTU:
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) < (uint32_t)M16REG(ry))
			    ? 1 : 0;
			break;
		case M16_RR_SLLV:
			M16REG(ry) = (int32_t)(
			    (uint32_t)M16REG(ry) <<
			    (M16REG(rx) & 0x1f));
			break;
		case M16_RR_BREAK:
			mips_cpu_exception(cpu, EXCEPTION_BP, 0, 0,
			    0, 0, 0, 0);
			return 1;
		case M16_RR_SRLV:
			M16REG(ry) = (int32_t)(
			    (uint32_t)M16REG(ry) >>
			    (M16REG(rx) & 0x1f));
			break;
		case M16_RR_SRAV:
			M16REG(ry) = (int32_t)(
			    (int32_t)M16REG(ry) >>
			    (M16REG(rx) & 0x1f));
			break;
		case M16_RR_CMP:
			cpu->cd.mips.gpr[MIPS_GPR_T8] =
			    ((uint32_t)M16REG(rx) ^
			    (uint32_t)M16REG(ry));
			break;
		case M16_RR_NEG:
			M16REG(rx) = (int32_t)(-(int32_t)M16REG(ry));
			break;
		case M16_RR_AND:
			M16REG(rx) = M16REG(rx) & M16REG(ry);
			break;
		case M16_RR_OR:
			M16REG(rx) = M16REG(rx) | M16REG(ry);
			break;
		case M16_RR_XOR:
			M16REG(rx) = M16REG(rx) ^ M16REG(ry);
			break;
		case M16_RR_NOT:
			M16REG(rx) = ~M16REG(ry);
			break;
		case M16_RR_MFHI:
			M16REG(rx) = cpu->cd.mips.hi;
			break;
		case M16_RR_MFLO:
			M16REG(rx) = cpu->cd.mips.lo;
			break;
		case M16_RR_MULT:
			{
				int64_t result = (int64_t)(int32_t)M16REG(rx) *
				    (int64_t)(int32_t)M16REG(ry);
				cpu->cd.mips.lo = (int32_t)result;
				cpu->cd.mips.hi = (int32_t)(result >> 32);
			}
			break;
		case M16_RR_MULTU:
			{
				uint64_t result =
				    (uint64_t)(uint32_t)M16REG(rx) *
				    (uint64_t)(uint32_t)M16REG(ry);
				cpu->cd.mips.lo = (int32_t)result;
				cpu->cd.mips.hi = (int32_t)(result >> 32);
			}
			break;
		case M16_RR_DIV:
			if ((int32_t)M16REG(ry) != 0) {
				cpu->cd.mips.lo = (int32_t)(
				    (int32_t)M16REG(rx) /
				    (int32_t)M16REG(ry));
				cpu->cd.mips.hi = (int32_t)(
				    (int32_t)M16REG(rx) %
				    (int32_t)M16REG(ry));
			}
			break;
		case M16_RR_DIVU:
			if ((uint32_t)M16REG(ry) != 0) {
				cpu->cd.mips.lo = (int32_t)(
				    (uint32_t)M16REG(rx) /
				    (uint32_t)M16REG(ry));
				cpu->cd.mips.hi = (int32_t)(
				    (uint32_t)M16REG(rx) %
				    (uint32_t)M16REG(ry));
			}
			break;
		default:
			fatal("UNIMPLEMENTED MIPS16 RR func 0x%02x "
			    "at 0x%" PRIx64 "\n", func, cpu->pc);
			cpu->running = 0;
			return 0;
		}
		break;

	default:
		fatal("UNIMPLEMENTED MIPS16 opcode 0x%02x "
		    "at 0x%" PRIx64 "\n", op, cpu->pc);
		cpu->running = 0;
		return 0;
	}

	/*  Advance PC past this instruction  */
	if (extended)
		cpu->pc += 4;
	else
		cpu->pc += 2;

	/*  Transition delay slot state: after executing the delay
	 *  slot instruction, the next call will redirect to target.  */
	if (cpu->delay_slot == TO_BE_DELAYED)
		cpu->delay_slot = DELAYED;

	return 1;
}
