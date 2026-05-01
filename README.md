# riscv-emulator-linux

A minimal RV32IMA RISC-V emulator in a single C file, capable of booting Linux.

This is a refactored version of [cnlohr/mini-rv32ima](https://github.com/cnlohr/mini-rv32ima), rewritten for readability, portability, and as a clean foundation for a Rust port.

## Features

- Boots Linux (nommu, RV32IMA)
- Single C file — `mini-rv32ima.c` + `default64mbdtc.h`
- RV32IMA: integer, multiply/divide, and atomic instructions
- UART 8250/16550, CLINT timer, SYSCON (poweroff/restart)
- Embedded default 64MB DTB — no external files needed to run
- Windows and POSIX (Linux/macOS) support

## Building and running

### Windows (GCC)

```powershell
gcc mini-rv32ima.c -o mini-rv32ima.exe
.\mini-rv32ima.exe -f Image
```

Or use the included script which also downloads the Linux image:

```powershell
.\run.ps1
```

### Linux / macOS

```sh
gcc mini-rv32ima.c -o mini-rv32ima
./mini-rv32ima -f Image
```

### Getting a kernel image

```powershell
# PowerShell
$archive = 'linux-6.1.14-rv32nommu-cnl-1.zip'
Invoke-WebRequest -Uri https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/$archive -OutFile $archive
Expand-Archive $archive -DestinationPath .
```

```sh
# bash
wget https://github.com/cnlohr/mini-rv32ima-images/raw/master/images/linux-6.1.14-rv32nommu-cnl-1.zip
unzip linux-6.1.14-rv32nommu-cnl-1.zip
```

## Options

| Flag | Description |
|------|-------------|
| `-f <image>` | Kernel image to run (required) |
| `-m <bytes>` | RAM size (default: 64MB) |
| `-k <cmdline>` | Kernel command line |
| `-b <dtb>` | Custom DTB file, or `disable` |
| `-c <count>` | Stop after this many instructions |
| `-s` | Single-step: print full CPU state before each instruction |
| `-t <divisor>` | Time division base |
| `-l` | Lock time base to instruction count (deterministic) |
| `-p` | Disable sleep on WFI (spin instead) |
| `-d` | Fail immediately on any fault |

## Files

| File | Description |
|------|-------------|
| `mini-rv32ima.c` | Emulator — everything in one file |
| `default64mbdtc.h` | Embedded default DTB (64MB, pre-compiled) |
| `run.ps1` | PowerShell build + run script for Windows |

## Architecture

The emulator is structured as a pipeline of clean, named abstractions:

```
main
 └── emulator_load()     — loads image + DTB into RAM, resets CPU
 └── emulator_run()      — execution loop, returns StepResult
      └── MiniRV32IMAStep()  — single time slice of N instructions
           ├── decode_imm_{I,S,B,J,U}()  — one function per RV32 immediate format
           ├── HandleControlLoad/Store()  — MMIO dispatch (UART, CLINT, SYSCON)
           └── HandleOtherCSRRead/Write() — non-standard debug CSRs
```

Key types:

- `EmulatorState` — owns RAM buffer + CPU state, no globals
- `MiniRV32IMAState` — RISC-V CPU registers and CSRs
- `RVStepResult` — typed return codes from the execution loop
- `RVTrap` — typed exception/interrupt codes matching the RISC-V spec mcause table
- `RVCsr` — named CSR addresses from the privileged spec

## Emulator debug CSRs

The emulator exposes non-standard CSRs for bare-metal debug output:

| CSR | Address | Description |
|-----|---------|-------------|
| `CSR_PRINT_INT` | `0x136` | Print register value as decimal |
| `CSR_PRINT_HEX` | `0x137` | Print register value as hex |
| `CSR_PRINT_STR` | `0x138` | Print null-terminated string at guest address |
| `CSR_PRINT_CHAR` | `0x139` | Print single character |
| `CSR_READ_KBD` | `0x140` | Read one byte from keyboard (-1 if none) |

## MMIO map

| Address | Device |
|---------|--------|
| `0x10000000` | UART 8250/16550 data |
| `0x10000005` | UART line status |
| `0x11004000` | CLINT `timermatchl` |
| `0x11004004` | CLINT `timermatchh` |
| `0x1100bff8` | CLINT `timerl` |
| `0x1100bffc` | CLINT `timerh` |
| `0x11100000` | SYSCON (write `0x5555` = poweroff, `0x7777` = restart) |

## Differences from the original

This is a refactor of the original [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) by Charles Lohr. The emulator behaviour is identical. What changed:

- Merged `.c` and `.h` into a single file
- Replaced all logic `#define` macros with functions and named constants
- Introduced `EmulatorState` — no global mutable state (one exception: the POSIX signal handler)
- `goto restart` replaced with `do { load(); run(); } while (restart)`
- `RVStepResult`, `RVTrap`, `RVCsr` enums replace magic numbers throughout
- Immediate decode centralised into `decode_imm_{I,S,B,J,U}()`
- `extraflags` bit fields accessed through named accessor functions
- `cyclel/cycleh` composed via `cpu_get_cycle64` / `cpu_set_cycle64` (eliminates UB cast)
- SRL/SRA signedness bug fixed
- All DTB magic offsets and sentinel values named as constants
- Platform code (Windows / POSIX) moved above all logic — no forward declarations needed

## License

The original code is Copyright 2022 Charles Lohr, available under BSD, MIT, or CC0.
Modifications are Copyright 2026 Hernani Samuel Diniz.

This combined work is distributed under the MIT License — see [LICENSE](LICENSE) for details.