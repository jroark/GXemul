# MIPS16 Support for GXemul

## Context

GXemul's MIPS emulation now includes MIPS16 (compact 16-bit encoding) support. The dyntrans framework assumes fixed 32-bit instructions, so MIPS16 uses a slow interpreter fallback, following the same pattern as ARM Thumb (`arm_cpu_interpret_thumb_SLOW()` in `cpu_arm.c`).

## Implementation

### Step 1: CPU State — ISA mode tracking
**File: `src/include/cpu_mips.h`** — `int mips16;` field in `struct mips_cpu`

### Step 2: CP0 Config Register — M16 bit
**File: `src/cpus/cpu_mips_coproc.c`** — `const int m16 = 1;` sets Config1 bit 2

### Step 3: MIPS16 Opcode Definitions
**File: `src/include/opcodes_mips16.h`** — Primary opcodes, RR/RRR/I8/SHIFT function codes, register mapping

### Step 4: Slow Interpreter and Disassembler
**File: `src/cpus/cpu_mips16.c`**
- `mips_cpu_interpret_mips16_SLOW()` — executes one MIPS16 instruction per call
- `mips_cpu_disassemble_instr_mips16()` — disassembles one MIPS16 instruction
- Covers: ALU (ADDIU, SLTI, SLTIU, LI, CMPI, RRIA), shifts (SLL/SRL/SRA), loads/stores (LB/LBU/LH/LHU/LW/LWSP/LWPC/SB/SH/SW/SWSP), branches (B/BEQZ/BNEZ/BTEQZ/BTNEZ), jumps (JAL/JALX/JR/JALR), RR group (SLT/SLTU/SLLV/SRLV/SRAV/CMP/NEG/AND/OR/XOR/NOT/MFHI/MFLO/MULT/MULTU/DIV/DIVU/BREAK), RRR (ADDU/SUBU), I8 (SWRASP/ADJSP/MOV32R/MOVR32), EXTEND prefix

### Step 5: Execution Loop Hook
**File: `src/cpus/cpu_dyntrans.c`** — MIPS16 check at the very top of `DYNTRANS_RUN_INSTR_DEF`, before `PC_TO_POINTERS` and interrupt checks. Runs a loop of up to `N_SAFE_DYNTRANS_LIMIT` MIPS16 instructions per batch.

### Step 6: Mode Switching from MIPS32
**File: `src/cpus/cpu_mips_instr.c`**
- JR, JR_RA, JR_RA_ADDIU, JR_RA_TRACE, JALR, JALR_TRACE: check bit 0 of target address; if set, enter MIPS16 mode via `nothing_call` pattern
- JALX (`HI6_JALX` = 0x1d): J-type instruction that always enters MIPS16 mode
- **File: `src/include/opcodes_mips.h`** — `HI6_JALX` constant and name table entry

### Step 7: Mode Switching from MIPS16
**File: `src/cpus/cpu_mips16.c`** — JR/JALR check bit 0 of target; JALX toggles to MIPS32; JAL stays in MIPS16

### Step 8: Exception Handling
- **`src/cpus/cpu_mips.c`** (`mips_cpu_exception()`): saves ISA mode in EPC bit 0; clears `mips16` on exception entry
- **`src/cpus/cpu_mips_instr.c`** (`X(eret)`): restores `mips16` from EPC bit 0

### Step 9: Disassembler Integration
**File: `src/cpus/cpu_mips.c`** — `mips_cpu_disassemble_instr()` dispatches to `mips_cpu_disassemble_instr_mips16()` when in MIPS16 mode

### Step 10: Build System
- **`configure`**: `cpu_mips16.o` added to `CPU_ARCHS`
- **`src/cpus/Makefile.skel`**: dependency rule for `cpu_mips16.o`

## Files

| File | Action | Purpose |
|------|--------|---------|
| `src/include/cpu_mips.h` | Edited | `mips16` field, function prototypes |
| `src/include/opcodes_mips.h` | Edited | `HI6_JALX`, name table |
| `src/include/opcodes_mips16.h` | Created | MIPS16 opcode definitions |
| `src/cpus/cpu_mips16.c` | Created | Slow interpreter + disassembler |
| `src/cpus/cpu_dyntrans.c` | Edited | MIPS16 mode detection, PC sync skip, execution loop |
| `src/cpus/cpu_mips_instr.c` | Edited | JR/JALR mode switch, JALX handler, ERET restore, `to_be_translated`/`end_of_page` guards |
| `src/cpus/cpu_mips.c` | Edited | Exception EPC bit 0, disassembler dispatch |
| `src/cpus/cpu_mips_coproc.c` | Edited | `m16 = 1` (Config1 bit) |
| `configure` | Edited | `cpu_mips16.o` in CPU_ARCHS |
| `src/cpus/Makefile.skel` | Edited | Dependency rule |
| `test/mips16/test_mips16.S` | Created | MIPS32→MIPS16→MIPS32 test program |
| `test/mips16/test_mips16` | Created | Compiled test binary (big-endian ELF) |

## Verification

1. **Build**: `./configure && make` — clean with no new warnings
2. **Non-trace mode**: `./gxemul -q -E testmips test/mips16/test_mips16` outputs `M32:M16:OK`
3. **Trace mode**: `./gxemul -E testmips -i test/mips16/test_mips16` shows full instruction trace with correct MIPS16 disassembly and mode transitions
4. **Test binary**: `test/mips16/test_mips16.S` — assembled with `mipsel-linux-gnu-gcc -EB -mips32r2` via Docker cross-toolchain (`mips-cross-dev` image)
