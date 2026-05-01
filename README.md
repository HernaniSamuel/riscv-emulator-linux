# riscv-emulator-linux

A minimal RISC-V emulator written in C that can run Linux.

---

## ⚠️ Project Status

> Works on my machine™ (Windows + Linux/WSL)

* Tested on Windows (PowerShell)
* Tested on Linux (WSL)
* Likely works on native Linux
* macOS: untested
* No guarantees
* No support
* No promises

If it runs on your machine, congratulations.

---

## 📦 Requirements

### Windows

* PowerShell
* GCC (MinGW or similar) in PATH

### Linux / WSL

* GCC
* Bash

---

## 🚀 How to run

### 🪟 Windows (PowerShell)

```
.\run.ps1
```

---

### 🐧 Linux / WSL

```
./run.sh
```

If needed, make it executable first:

```
chmod +x run.sh
```

---

### 🔧 Manual build (Linux / WSL)

```
gcc mini-rv32ima.c -O2 -o mini-rv32ima
./mini-rv32ima -f Image
```

---

## 🖥️ What to expect

If everything goes well, you should land in a Linux shell (BusyBox).

When prompted:

```
buildroot login:
```

Just type:

```
root
```

(No password is required)

---

## 🔐 Notes

* The system runs entirely inside the emulator
* There is no real disk, no network, and no access to your host machine
* You have full root access inside the emulated environment

---

## ⚠️ Controls / Behavior

* Pressing `Ctrl+C` will exit the emulator immediately
  (it does not send a signal to the emulated Linux system)

* In other words: it kills everything, not just the shell inside Linux

* This is a limitation of the current implementation

---

## 🧠 What is this?

This project is a minimal emulator of:

* RISC-V CPU (RV32IMA)
* Memory
* Basic devices (UART, timer)

Just enough to boot a Linux kernel without an MMU.

---

## 🎯 Goal

* Keep the emulator simple and readable
* Avoid obscure or undefined C behavior
* Make the architecture easy to understand and modify
* Serve as a base for ports to other languages (e.g. Rust)

This project aims to turn a minimal emulator into something **didactic, portable, and structurally clean**.

---

## 📦 What’s included

* Refactored emulator in C
* Precompiled Linux kernel
* Minimal root filesystem (Buildroot)
* One-command runner scripts (`.ps1` and `.sh`)

---

## ⚙️ Limitations

* No MMU
* No memory isolation
* Minimal hardware
* Not fast
* Not complete

---

## 📜 Credits

Based on the work of Charles Lohr (mini-rv32ima).
See `NOTICE.md` for details.

---

## 🤷 Why does this exist?

To explore the minimum requirements needed to run Linux.

And to understand how far a simple emulator can go.

---

## 🧪 Disclaimer

This project has been heavily modified and simplified from its original form.
Expect rough edges.

---

## LICENCE

MIT 
