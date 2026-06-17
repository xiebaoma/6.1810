# MIT 6.1810 Operating System Engineering Lab Project (xv6-riscv)

## Project Overview

This repository is based on `xv6-riscv` and documents the implementation of the MIT 6.1810 (Operating System Engineering) lab sequence.
It covers core operating system topics including system calls, virtual memory, traps, synchronization, file systems, networking, and memory-mapped files.

The original upstream xv6 introduction is preserved in `README`.

## Lab Coverage

The `lab/` directory contains the official lab specifications (`lab1.txt` to `lab9.txt`), covering:

1. **Lab 1 - Xv6 and Unix utilities**
2. **Lab 2 - System calls**
3. **Lab 3 - Page tables**
4. **Lab 4 - Traps**
5. **Lab 5 - Copy-on-Write fork**
6. **Lab 6 - Networking (E1000 and UDP/IP receive path)**
7. **Lab 7 - Locks (parallelism and contention reduction)**
8. **Lab 8 - File system (large files and symbolic links)**
9. **Lab 9 - mmap/munmap**

## Environment Requirements

- Linux or macOS
- RISC-V cross-compilation toolchain (e.g. `riscv64-unknown-elf-gcc`)
- `qemu-system-riscv64`
- `make`, `gcc` (`gdb` optional for debugging)

## Build and Run

From the repository root:

```bash
make qemu
```

For multi-core startup:

```bash
make qemu3
```

After boot, run user programs and tests from the xv6 shell.

## Validation

Common validation commands include:

```bash
make grade
```

or inside xv6:

```bash
usertests -q
```

Some labs also rely on dedicated tests such as `cowtest`, `mmaptest`, `kalloctest`, `bigfile`, and `nettest`.

## Repository Layout

```text
.
├── kernel/        # xv6 kernel source code
├── user/          # user-space programs and syscall stubs
├── lab/           # lab specifications
├── mkfs/          # filesystem image builder
├── README         # upstream xv6 README
└── README.md      # project-level documentation
```

## Learning Outcomes

- Understand the end-to-end flow of system calls and trap handling
- Implement and debug page-table-based virtual memory mechanisms
- Improve kernel scalability via lock and data-structure redesign
- Extend xv6 file-system and address-space capabilities
- Build practical understanding of network paths from driver to protocol handling

## References

- MIT 6.1810 course page: <https://pdos.csail.mit.edu/6.1810/>
- xv6 book and per-lab specifications
