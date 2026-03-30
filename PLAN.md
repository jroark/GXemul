# MIPS16 Support for GXemul

## Context

GXemul's MIPS emulation currently only supports 32-bit MIPS instructions. The codebase has explicit TODO markers for MIPS16 support (`cpu_mips_coproc.c:60`) and acknowledges the ISA in `opcodes_mips.h:46-48`. MIPS16 is a compact 16-bit instruction encoding for MIPS32/64 that reduces code size. It is used by real-world toolchains (GCC, LLVM) and operating systems (Linux MIPS with MIPS16 interworking).

The dyntrans framework assumes fixed 32-bit instructions and explicitly documents that variable-length ISAs are unsupported (`README_DYNTRANS:26`). However, ARM Thumb (also 16-bit) is already handled via a slow interpreter fallback (`cpu_dyntrans.c:255-262` calls `arm_cpu_interpret_thumb_SLOW()` in `cpu_arm.c:1083-1700`). We will follow this same pattern.

## Approach: Slow Interpreter (following ARM Thumb precedent)

When MIPS16 mode is detected in the execution loop, bypass dyntrans and call a slow interpreter function that reads 16-bit instruction words, decodes them, and executes them one at a time. This avoids modifying the dyntrans IC entry array layout.

## Implementation Steps

### Step 1: CPU State — Add ISA mode tracking
**File: `src/include/cpu_mips.h`** (~line 205, in `struct mips_cpu`)
- Add `int mips16_mode;` field to track whether the CPU is executing in MIPS16 mode
- This mirrors how ARM uses `cpsr & ARM_FLAG_T` to track Thumb mode

### Step 2: CP0 Config Register — Enable M16 bit
**File: `src/cpus/cpu_mips_coproc.c`** (line 60)
- Change `const int m16 = 0;` to `const int m16 = 1;`
- This sets bit 2 in Config Select 1 (line 103: `| ( 4 * m16)`) to advertise MIPS16 support

### Step 3: MIPS16 Opcode Definitions
**File: `src/include/opcodes_mips16.h`** (NEW file)
- Define MIPS16 5-bit primary opcode constants (0x00-0x1F):
  - `M16_ADDIU_SP` (0x00), `M16_ADDIUPC` (0x01), `M16_B` (0x02), `M16_JAL_JALX` (0x03)
  - `M16_BEQZ` (0x04), `M16_BNEZ` (0x05), `M16_SHIFT` (0x06), `M16_LD` (0x07) (64-bit only)
  - `M16_RRI_A` (0x08), `M16_ADDIU` (0x09), `M16_SLTI` (0x0A), `M16_SLTIU` (0x0B)
  - `M16_I8` (0x0C), `M16_LI` (0x0D), `M16_CMPI` (0x0E), `M16_SD` (0x0F) (64-bit only)
  - `M16_LB` (0x10), `M16_LH` (0x11), `M16_LWSP` (0x12), `M16_LW` (0x13)
  - `M16_LBU` (0x14), `M16_LHU` (0x15), `M16_LWPC` (0x16), `M16_LWU` (0x17) (64-bit only)
  - `M16_SB` (0x18), `M16_SH` (0x19), `M16_SWSP` (0x1A), `M16_SW` (0x1B)
  - `M16_RRR` (0x1C), `M16_RR` (0x1D), `M16_EXTEND` (0x1E), `M16_I64` (0x1F) (64-bit only)
- Define RR function codes (bits 4:0): JR, JALR, MFHI, MFLO, etc.
- Define RRR function codes: ADDU, SUBU, AND, OR, XOR, etc.
- Define I8 function codes: BTEQZ, BTNEZ, SWRASP, ADJSP, etc.
- Define MIPS16-to-MIPS32 register mapping: `{16, 17, 2, 3, 4, 5, 6, 7}` (the 8 directly-addressable registers)

### Step 4: Slow Interpreter Function
**File: `src/cpus/cpu_mips16.c`** (NEW file)
- `int mips_cpu_interpret_mips16_SLOW(struct cpu *cpu)` — main interpreter entry point
- `int mips_cpu_disassemble_instr_mips16(struct cpu *cpu, unsigned char *ib, int running, uint64_t dumpaddr)` — disassembler

**Interpreter logic:**
1. Read 16-bit instruction word from `cpu->pc & ~1` (mask off ISA mode bit)
2. Extract 5-bit primary opcode (bits 15:11)
3. Check for EXTEND prefix (opcode 0x1E): if so, read next 16-bit word and form 32-bit extended instruction
4. Decode and execute based on opcode, handling:
   - **ALU**: ADDIU, SLTI, SLTIU, LI, CMPI, ADDIU8, and RRR-type (ADDU, SUBU, AND, OR, XOR, SLT, SLTU)
   - **Shifts**: SLL, SRL, SRA (opcode 0x06)
   - **Loads/Stores**: LB, LBU, LH, LHU, LW, LWSP, LWPC, SB, SH, SW, SWSP (and 64-bit: LD, SD, LWU)
   - **Branches**: B (unconditional), BEQZ, BNEZ, BTEQZ, BTNEZ — with delay slots
   - **Jumps**: JAL/JALX (opcode 0x03), JR/JALR (RR function) — with delay slots
   - **RR misc**: MFHI, MFLO, BREAK, SEB, SEH, NOT, NEG, CMP
   - **I8 group**: MOV32R, MOVR32, SWRASP, ADJSP
   - **EXTEND**: Prefix that extends immediate fields of the following instruction
5. Update `cpu->pc` by 2 (or 4 for extended/JAL instructions)
6. Handle delay slots similarly to the ARM Thumb interpreter (execute next instruction before branching)
7. Handle trace/debug output when `cpu->machine->instruction_trace` is set

**Register mapping helper** (inline or macro):
```c
static const int mips16_reg_map[8] = { 16, 17, 2, 3, 4, 5, 6, 7 };
#define M16_REG(x) cpu->cd.mips.gpr[mips16_reg_map[(x)]]
```

### Step 5: Mode Switching in Execution Loop
**File: `src/cpus/cpu_dyntrans.c`** (~line 253, after the ARM Thumb block)
- Add MIPS16 mode detection block (similar pattern to ARM Thumb at lines 254-262):
```c
#ifdef DYNTRANS_MIPS
    if (cpu->cd.mips.mips16_mode) {
        mips_cpu_interpret_mips16_SLOW(cpu);
        return 1;
    }
#endif
```

### Step 6: Mode Switching in MIPS32 Jump/Branch Instructions
**File: `src/cpus/cpu_mips_instr.c`**

Modify existing `jr`, `jr_ra`, `jalr` implementations (~lines 987-1076) to check bit 0 of the target address:
- In `X(jr)`: after `cpu->pc = rs;`, check if `cpu->pc & 1` — if set, enter MIPS16 mode: `cpu->cd.mips.mips16_mode = 1; cpu->pc &= ~1;`
- Same for `X(jr_ra)`, `X(jr_ra_addiu)`, `X(jr_ra_trace)`, `X(jalr)`, `X(jalr_trace)`

Add JALX instruction support in the `to_be_translated` switch (~line 4158):
- JALX uses the same J-type encoding but toggles ISA mode. Its opcode is `0x1D` (HI6 = 0x1D, currently mapped to `"hi6_1d"` in `opcodes_mips.h:64`)
- Add a new `X(jalx)` handler that sets `mips16_mode = 1` and links `$31`

### Step 7: Mode Switching in MIPS16 Interpreter (exiting MIPS16)
**File: `src/cpus/cpu_mips16.c`**

In the MIPS16 interpreter:
- `JR rx`: If target address bit 0 is clear, set `mips16_mode = 0` (return to MIPS32)
- `JR ra`: Always clears MIPS16 mode (convention for returning to MIPS32 caller)
- `JALR ra, rx`: Links and jumps; check bit 0
- `JALX`: Toggles ISA mode (exits MIPS16, enters MIPS32)
- `JAL`: Stays in MIPS16 mode (bit 0 of target is set implicitly)

### Step 8: Exception Handling Changes
**File: `src/cpus/cpu_mips.c`** (`mips_cpu_exception()`, ~line 1920-2005)

- When saving EPC during exceptions: if in MIPS16 mode, set bit 0 of EPC to preserve ISA mode info (line 1931-1935)
- Clear `mips16_mode` when entering exception handler (exceptions always execute MIPS32)
- In ERET (handled in `cpu_mips_coproc.c`): when restoring PC from EPC, check bit 0 to restore `mips16_mode`

**File: `src/cpus/cpu_mips_coproc.c`** (search for `eret`/`ERET`)
- In the ERET handler: `cpu->cd.mips.mips16_mode = (epc & 1); cpu->pc = epc & ~1;`

### Step 9: Disassembler Integration
**File: `src/cpus/cpu_mips.c`** (`mips_cpu_disassemble_instr()`, ~line 688)

- At the start of the disassembly function, check if address bit 0 is set (or if `mips16_mode` is active)
- If so, dispatch to `mips_cpu_disassemble_instr_mips16()` (same pattern as ARM at `cpu_arm.c:1729`)

### Step 10: Build System Integration
**File: `src/cpus/Makefile.skel`** (~line 147)
- Add `cpu_mips16.c` to the MIPS CPU object dependencies
- Include `opcodes_mips16.h` in the header dependencies

### Critical Files Summary

| File | Action | Purpose |
|------|--------|---------|
| `src/include/cpu_mips.h` | Edit | Add `mips16_mode` field |
| `src/include/opcodes_mips16.h` | New | MIPS16 opcode definitions |
| `src/cpus/cpu_mips16.c` | New | Slow interpreter + disassembler |
| `src/cpus/cpu_mips_coproc.c` | Edit | Enable M16 config bit, ERET mode restore |
| `src/cpus/cpu_dyntrans.c` | Edit | MIPS16 mode detection in execution loop |
| `src/cpus/cpu_mips_instr.c` | Edit | Mode switching in JR/JALR, add JALX |
| `src/cpus/cpu_mips.c` | Edit | Exception EPC bit 0, disassembler dispatch |
| `src/cpus/Makefile.skel` | Edit | Build system |
| `src/include/opcodes_mips.h` | Edit | Add HI6_JALX constant |

### Existing code to reuse
- `arm_cpu_interpret_thumb_SLOW()` in `cpu_arm.c:1083-1700` as structural template
- `memory_rw()` for instruction fetches in the slow interpreter
- Existing `mips_cpu_exception()` for exception delivery
- Existing `mips16_reg_map` concept (just need to define the array)
- `cpu->byte_order` and endian conversion macros (`LE16_TO_HOST`, etc.)

## Verification

1. **Build test**: Run `./configure && make` — ensure clean compilation with no warnings
2. **Config register check**: Boot a MIPS32 guest and verify Config1 bit 2 (M16) is set via debugger register dump
3. **Disassembler test**: Use GXemul's built-in disassembler on a binary containing MIPS16 code to verify correct disassembly
4. **Functional test**: Cross-compile a simple C program with `mips-linux-gnu-gcc -mips16` and run it in GXemul, verifying:
   - Mode entry (JALX or JR with bit 0 set)
   - Basic ALU operations
   - Load/store operations
   - Branch/jump instructions
   - Mode exit (JR $ra back to MIPS32)
   - Exception handling (interrupt during MIPS16 execution, ERET return)
5. **EXTEND instruction**: Test extended immediate instructions (common in compiler output)
