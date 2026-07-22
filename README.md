# ⚠️ Attention

This project is still in an early and rough development phase. While some parts are already functional, the software is not yet stable, optimized, or production-ready.

Below is a list of the main tasks and improvements that still need to be completed before the project can be considered mature.

## TODO
* [ ] Implement an audio, keyboard and joystick controller
* [ ] Test the floppy disk controller
* [x] Optimize the BSL (the SPRK32 language) compiler
* [ ] Create a rom with a BSL compiler and an UI for the console
* [ ] Upgrade the video processing unit (for now has only 255 variants of grey)
* [ ] Fix known bugs (a lots of bugs)
* [ ] Complete software/hardware documentation
* [ ] Make the emulator cross-platform (for now the code is running under Macos)

# SPRK32

SPRK32 is a fantasy computer environment inspired by 80/90s home computers, built for pure low-level fun and experimentation.
The system is designed around the idea of writing software close to the hardware, where every resource matters.

SPRK32 reflects the spirit of early personal computers:
* CPU clock: **16MHz**
* Display resolution: **160×120**
* Color depth: **256 colors**
* Storage: **3.5" HD floppy disk**

These limitations are part of the design and encourage efficient and creative programming.

The project provides a complete development environment including:

* An emulator (for now with a CPU, ICU, DMA, FDC and a VPU)
* A custom high-level assembler-like language (BSL) compiler
* A disassembler

All tools are accessible through a built-in command interpreter.
Commands can be executed either interactively via terminal or through a configuration file (`sprk32.cfg`).

Example of a sprk32.cfg:
```
compile rom.bsl to rom.bin as 65536
disasm rom.bin rom.s
isok? // Check for errors

// Set rom/disk binaries paths
disk = disk.bin
rom = rom.bin
emulate
```

---

# Base system language

Software for the SPRK32 is written in a custom assembly language designed specifically for the architecture called BSL. While it maps almost one-to-one to the underlying instruction set, the assembler provides a set of compile-time features that significantly improve readability without introducing any runtime overhead.

Rather than treating assembly as a stream of anonymous instructions, the language encourages explicit declaration of resources. Procedures declare the registers they use, memory layouts are described symbolically, and hardware registers are accessed through named structures instead of hardcoded addresses.

```
:alloc
    use r0 u32, r1 u8, r3 u32
    push(r0)
    r3 = abs_get(sys, current_ptr)
    r0 = [r3]

    r0 += r1
    if r0 >= endof(mmap@wram) { hlt }

    [r3] = r0
    pop(r0)
;
```

Memory maps can be declared directly in source code, allowing both RAM and memory-mapped peripherals to be addressed symbolically.

```
layout mmap align {
    rom   [rom_size],
    ivt   sizeof ivt,
    sys   sizeof sys,
    ...
}
```

The same mechanism can also be used to describe application-specific data structures at compile time.

```
layout game align {
    padding mmap@wram,
    x 2,
    y 2
}

bitset flag { zero, sign, overflow, carry, interrupt }

r0 = [r3 + game@x]
r1 = [r3 + game@y]
...
```

The assembler also includes a lightweight compile-time metaprogramming system. Macros may accept variadic arguments, iterate over them, and generate code during assembly, allowing common idioms to remain concise while expanding into ordinary instructions.

```
#define push(...) {
    #forvarg { [sp--] = varg }
}

#define pop(...) {
    #forvarg { varg = [++sp] }
}
```

Since macros are expanded entirely at assembly time, they introduce no abstraction cost.

Functions can freely mix ordinary instructions, compile-time expressions, and symbolic constants.

```
#define video_swap {
    r3 = abs_get(vpu, ctrl)
    r0 = [r3]
    r0 |= vpu_ctrl_swap
    [r3] = r0
}
```

Registers may optionally carry type annotations (u8, u16, u32, s16, ...). They affect the generated machine code.

Interrupt handlers, device drivers, DMA operations, and memory allocation are all written using the same language.

Overall, the language aims to preserve the simplicity and predictability of classic assembly while removing much of its boilerplate. It remains entirely deterministic, produces straightforward machine code, and provides just enough compile-time abstraction to make large programs practical without hiding the underlying hardware.

# LICENSE

SPRK32 is licensed under the MIT License. See the [`LICENSE`](LICENSE) file for more details.