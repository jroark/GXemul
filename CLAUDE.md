# CLAUDE.md — GXemul Development Guide

## Project Overview

GXemul is a full-system computer architecture emulator supporting ARM, MIPS, Motorola 88K, PowerPC, and SuperH processors. It uses a dynamic translation (dyntrans) framework that translates guest instructions into C function pointer calls. The codebase is C99, originally by Anders Gavare (2003-2021).

## Build

```sh
./configure
make
```

The build generates intermediate files (`tmp_*`) in `src/cpus/` via code generators. Run `./configure` first — it creates `Makefile` from `Makefile.skel`. No cmake/autotools — it's a custom configure script.

The resulting binary is `gxemul` in the project root.

## Architecture

### Dyntrans Framework

The core execution engine is in `src/cpus/cpu_dyntrans.c` — a template included by each architecture via `#include` with preprocessor defines (`DYNTRANS_MIPS`, `DYNTRANS_ARM`, etc.). It:

- Maintains an array of `instr_call` structs per physical page (function pointer + 3 args)
- On first execution, `to_be_translated()` decodes the instruction, fills in the IC entry, then executes it
- Subsequent executions just call the cached function pointer
- Page boundaries use special end-of-page handlers (2 extra slots for delay-slot architectures like MIPS)

### MIPS CPU Files

| File | Purpose |
|------|---------|
| `src/cpus/cpu_mips.c` | Main CPU init, exception handling, disassembler, TLB dump |
| `src/cpus/cpu_mips_instr.c` | Instruction handlers (`X(name)` macros) and `to_be_translated()` decoder |
| `src/cpus/cpu_mips_coproc.c` | CP0/CP1 coprocessor register handling |
| `src/cpus/cpu_mips_instr_loadstore.c` | Load/store template (generates `tmp_mips_loadstore.c`) |
| `src/cpus/memory_mips.c` | MMU, v2p translation, R3000 cache |
| `src/include/cpu_mips.h` | `struct mips_cpu`, dyntrans macros, register constants |
| `src/include/opcodes_mips.h` | MIPS32/64 opcode definitions |
| `src/include/opcodes_mips16.h` | MIPS16 opcode definitions |
| `src/include/cop0.h` | CP0 register indices and bit definitions |
| `src/include/mips_cpu_types.h` | CPU type table (R2000 through MIPS64 rev2) |

### Key Patterns

- **Instruction handlers**: Defined as `X(name) { ... }` which expands to a function taking `(struct cpu *, struct instr_call *)`. Register operands are passed as `size_t` pointers in `ic->arg[0..2]`, accessed via `reg(ic->arg[N])`.
- **PC management**: The virtual `cpu->pc` and the IC pointer `cpu->cd.mips.next_ic` are kept in sync. `quick_pc_to_pointers()` converts PC to IC pointers after jumps.
- **Delay slots**: Branch instructions set `cpu->delay_slot = TO_BE_DELAYED`, execute `ic[1]` (the delay slot), then take the branch.
- **Variable-length ISAs**: Not supported by dyntrans. ARM Thumb uses a slow interpreter fallback (`arm_cpu_interpret_thumb_SLOW()` in `cpu_arm.c`). MIPS16 follows the same pattern.
- **Generated code**: `generate_head.c` / `generate_tail.c` produce `tmp_<arch>_head.c` / `tmp_<arch>_tail.c` which are `#include`d by the main CPU file. Don't edit `tmp_*` files.

### MIPS16 Implementation

MIPS16 support follows the ARM Thumb precedent: a slow interpreter (`mips_cpu_interpret_mips16_SLOW()` in `src/cpus/cpu_mips16.c`) that bypasses dyntrans when the CPU is in MIPS16 mode (`cpu->cd.mips.mips16 == 1`).

Key aspects:
- **Mode entry**: JR/JALR to an address with bit 0 set, or JALX from MIPS32
- **Mode exit**: JR/JALR to an address with bit 0 clear, JALX from MIPS16, or any exception
- **Register mapping**: MIPS16 3-bit register fields map to GPRs {$16,$17,$2,$3,$4,$5,$6,$7}
- **EXTEND prefix**: A 16-bit prefix (opcode 11110) that widens the immediate field of the next instruction
- **Exceptions**: EPC bit 0 preserves ISA mode; ERET restores it

### Exception Handling

`mips_cpu_exception()` in `cpu_mips.c` handles all MIPS exceptions. It saves PC to EPC (with ISA mode in bit 0 for MIPS16), sets the exception vector, and enters kernel mode. ERET (`X(eret)` in `cpu_mips_instr.c`) restores PC from EPC and re-enters MIPS16 mode if bit 0 is set.

## Conventions

- Use `fatal()` for error messages, `debug()` for trace/diagnostic output
- Register names: `MIPS_GPR_SP` (29), `MIPS_GPR_RA` (31), `MIPS_GPR_T8` (24), `MIPS_GPR_T9` (25)
- Memory access in slow interpreters: `cpu->memory_rw(cpu, cpu->mem, addr, buf, len, MEM_READ|MEM_WRITE, CACHE_INSTRUCTION|CACHE_DATA)`
- Match the existing code style: tabs for indentation, K&R braces, `/* C-style comments */`
- All source files carry the BSD 3-clause license header
