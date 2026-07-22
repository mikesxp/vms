#ifndef EMULATOR_H
#define EMULATOR_H
#include "../utils.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define REG_COUNT 4

#define HZ_PER_MHZ   1000000
#define CPU_HZ       (16 * HZ_PER_MHZ) // 16MHz

#define NS_PER_CYCLE (NS_PER_SEC / CPU_HZ)
#define NS_PER_FRAME (NS_PER_SEC / FRAME_RATE)

#define SCREEN_WIDTH  160
#define SCREEN_HEIGHT 120
#define SCREEN_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT)
#define FRAME_RATE 60

#define ROM_SIZE  (KB * 64) // 64kB
#define RAM_SIZE  (KB * 128) // 128kB

#define ROM_ADDR 0
#define RAM_ADDR (ROM_ADDR + ROM_SIZE)
#define IO_ADDR  (RAM_ADDR + RAM_SIZE)

#define INT_COUNT 8
#define IVT_ADDR RAM_ADDR
#define IVT_NMI_ADDR IVT_ADDR
#define IVT_IRQ_BASE_ADDR (IVT_NMI_ADDR + SIZE_WORD)
#define IVT_EXC_BASE_ADDR (IVT_IRQ_BASE_ADDR + (IVT_IRQ_COUNT * SIZE_WORD))
#define IVT_INT_START     (IVT_EXC_BASE_ADDR + (IVT_EXC_COUNT * SIZE_WORD))
#define IVT_SIZE ((1 + IVT_IRQ_COUNT + IVT_EXC_COUNT + INT_COUNT) * SIZE_WORD)

// Interrupts addresses
#define ICU_ADDR             IO_ADDR
#define ICU_NMI_STATUS_ADDR  (ICU_ADDR + 0)
#define ICU_IRQ_ENABLE_ADDR  (ICU_ADDR + 1)
#define ICU_IRQ_PENDING_ADDR (ICU_ADDR + 2)
#define ICU_SIZE             3

// DMA addresses
#define DMA_ADDR        (ICU_ADDR + ICU_SIZE + 1)  // +1 alignment
#define DMA_SRC_ADDR    DMA_ADDR
#define DMA_DST_ADDR    (DMA_ADDR + 4)
#define DMA_COUNT_ADDR  (DMA_ADDR + 8)
#define DMA_CTRL_ADDR   (DMA_ADDR + 12)
#define DMA_STATUS_ADDR (DMA_ADDR + 13)
#define DMA_SIZE        (SIZE_WORD * 3 + 2)

// FDC addresses
#define FDC_ADDR        (DMA_ADDR + DMA_SIZE)
#define FDC_STATUS_ADDR (FDC_ADDR + 0)
#define FDC_CTRL_ADDR   (FDC_ADDR + 1)
#define FDC_DATA_ADDR   (FDC_ADDR + 2)
#define FDC_TRACK_ADDR  (FDC_ADDR + 6)
#define FDC_SECTOR_ADDR (FDC_ADDR + 7)
#define FDC_ERROR_ADDR  (FDC_ADDR + 8)
#define FDC_DMA_ADDR    (FDC_ADDR + 9) // +1 alignment
#define FDC_SIZE        (6 + SIZE_WORD*2)

// Vpu addresses
#define VPU_ADDR           (FDC_ADDR + FDC_SIZE)
#define VPU_FRAME_PTR_ADDR VPU_ADDR
#define VPU_CTRL_ADDR      (VPU_ADDR + 4)
#define VPU_STATUS_ADDR    (VPU_ADDR + 5)
#define VPU_BUFFER0_ADDR   (VPU_ADDR + 6)
#define VPU_BUFFER1_ADDR   (VPU_BUFFER0_ADDR + SCREEN_SIZE)
#define VPU_SIZE           (6 + SCREEN_SIZE * 2)

// Timer addresses
#define TIMER_ADDR         (VPU_ADDR + VPU_SIZE)
#define TIMER_COUNTER_ADDR (TIMER_ADDR + 0)
#define TIMER_RELOAD_ADDR  (TIMER_ADDR + 1)
#define TIMER_CONTROL_ADDR (TIMER_ADDR + 2)

#define IO_SIZE (ICU_SIZE + DMA_SIZE + FDC_ADDR + VPU_SIZE)

// Stack addresses
#define STACK_TOP    (RAM_ADDR + RAM_SIZE)
#define STACK_BOTTOM (STACK_TOP - STACK_SIZE)
#define STACK_SIZE   256

typedef enum {
    NMI_WATCHDOG_TIMER,
} nmi_type;
typedef enum {
    IVT_IRQ_TIMER,
    IVT_IRQ_VBLANK,
    IVT_IRQ_FDC,
    IVT_IRQ_AUDIO, // TODO: implement audio hardware
    IVT_IRQ_DMA,
    IVT_IRQ_COUNT,
} ivt_irq;
typedef enum {
    IVT_EXC_ACCESS_VIOLATION,
    IVT_EXC_STACK_OVERFLOW,
    IVT_EXC_ILLEGAL_INSTRUCTION,
    IVT_EXC_ALIGNMENT_FAULT,
    IVT_EXC_DIVISION_BY_ZERO,
    IVT_EXC_STACK_UNDERFLOW,
    IVT_EXC_COUNT
} ivt_exc;

typedef struct {
    uint8_t nmi_status;
    uint8_t irq_enable;
    uint8_t irq_pending;
} emu_icu; // Interrupt control unit
typedef enum {
    DMA_CTRL_ENABLE = (1 << 0),
    DMA_CTRL_SRC_FIXED = (1 << 1),
    DMA_CTRL_DST_FIXED = (1 << 2),
    DMA_CTRL_WIDTH_SHIFT = 3,
    DMA_CTRL_WIDTH_MASK  = 0b11 << DMA_CTRL_WIDTH_SHIFT,
} dma_ctrl;
typedef enum {
    DMA_STATUS_BUSY = (1 << 0),
    DMA_STATUS_CONFIG_ERROR    = (1 << 1),
    DMA_STATUS_ALIGNMENT_ERROR = (1 << 2),
    DMA_STATUS_ACCESS_ERROR    = (1 << 3),
} dma_status;
typedef enum {
    DMA_STATE_IDLE,
    DMA_STATE_READ_LO,
    DMA_STATE_READ_HI,
    DMA_STATE_WRITE_LO,
    DMA_STATE_WRITE_HI
} dma_state;
typedef struct emu_bus emu_bus;
typedef struct {
    uint32_t src, dst, count;
    uint8_t ctrl, status;
    uint16_t buffer_lo, buffer_hi;
    dma_state state;
    emu_icu *icu;
    emu_bus *bus;
} emu_dma;

// FDC consts (for floppy disk 3.5" HD)
#define FDC_TRACKS      80
#define FDC_SECTORS     18
#define FDC_SECTOR_SIZE 512
#define FDC_HEADS       2
#define FDC_DISK_SIZE   (FDC_TRACKS * FDC_HEADS * FDC_SECTORS * FDC_SECTOR_SIZE)
#define FDC_RPM         300

#define FDC_STEP_TIME_NS 3000000ULL // 3 ms per track step
#define FDC_SPIN_UP_NS 300000000ULL // 300 ms motor spin-up
#define FDC_ROTATION_TIME_NS (60ULL * 1000000000ULL / FDC_RPM)
#define FDC_SECTOR_TIME_NS (FDC_ROTATION_TIME_NS / FDC_SECTORS)
#define FDC_CMD_DELAY_NS 1000000ULL // 1 ms command processing latency
#define FDC_BYTE_TIME_NS 16000ULL

#define FDC_INDEX_DURATION_NS 200000ULL

#define FDC_MOTOR_TIMEOUT_NS 2000000000ULL // 2 seconds
#define FDC_DRQ_TIMEOUT_NS (2 * FDC_BYTE_TIME_NS)

typedef enum {
    FDC_STATE_IDLE,
    FDC_STATE_CMD_ARG1, // Waiting for 1st command argument
    FDC_STATE_CMD_ARG2, // Waiting for 2nd command argument
    FDC_STATE_BUSY_SEEK, // Stepping head toward target track
    FDC_STATE_BUSY_SPIN, // Waiting for motor to spin up
    FDC_STATE_BUSY_SEARCH, // Rotating to find the right sector
    FDC_STATE_BUSY_RECAL,
    FDC_STATE_TRANSFER_READ, // Transferring bytes from disk to CPU
    FDC_STATE_TRANSFER_WRITE, // Transferring bytes from CPU to disk
    FDC_STATE_RESULT, // Result bytes ready to be read back
    FDC_STATE_ERROR, // Error condition, awaiting reset/ack
} fdc_state;
typedef enum {
    FDC_STATUS_BUSY  = (1 << 0),
    FDC_STATUS_DRQ   = (1 << 1), // Data Request
    FDC_STATUS_SEEK_DONE = (1 << 2),
    FDC_STATUS_ERROR = (1 << 3),
    FDC_STATUS_WRITE_PROTECTED = (1 << 4),
    FDC_STATUS_TRACK0  = (1 << 5), // Head at track 0
    FDC_STATUS_DISK_IN = (1 << 6),
    FDC_STATUS_INDEX = (1 << 7), // Index pulse
} fdc_status;
typedef enum {
    FDC_CTRL_RESET = (1 << 0), // Soft reset
    FDC_CTRL_MOTOR = (1 << 1), // Motor on/off
    FDC_CTRL_DMA_ENABLED = (1 << 2), // Enable DMA transfers
    FDC_CTRL_HEAD = (1 << 3) // Head select (0 = upper, 1 = lower)
} fdc_ctrl;
typedef enum {
    FDC_CMD_SEEK,
    FDC_CMD_READ,
    FDC_CMD_WRITE,
    FDC_CMD_FORMAT,
    FDC_CMD_RECAL, // Recalibrate (seek to track 0)
    FDC_CMD_STATUS, // Read drive status into result
    FDC_CMD_ID // Read sector ID (current pos)
} fdc_cmd;
typedef enum {
    FDC_ERROR_NOT_READY = (1 << 0),
    FDC_ERROR_INVALID_COMMAND = (1 << 1),
    FDC_ERROR_SEEK = (1 << 2),
    FDC_ERROR_RECORD = (1 << 3),
    FDC_ERROR_WRITE_PROTECT = (1 << 4),
    FDC_ERROR_LOST_DATA = (1 << 5),
    FDC_ERROR_DMA = (1 << 6),
    FDC_ERROR_RECORD_NOT_FOUND = (1 << 7),
} fdc_error;
typedef struct {
    uint8_t *disk_data;
    uint64_t last_rotation;

    uint8_t current_track;
    uint8_t current_head;
    uint8_t current_sector;

    fdc_state state;
    uint8_t command; // Current command byte
    uint8_t cmd_arg[2]; // Up to 2 command arguments
    uint8_t cmd_arg_count; // Args expected for current cmd
    uint8_t cmd_arg_idx;

    uint8_t target_track;
    uint8_t target_head;
    uint8_t target_sector;

    uint8_t  transfer_buffer[FDC_SECTOR_SIZE];
    uint16_t transfer_position; // Current byte position in buffer
    uint16_t transfer_len; // Total bytes to transfer

    uint8_t result[8];
    uint8_t result_len;
    uint8_t result_idx;

    uint8_t status;
    uint32_t data;
    uint8_t ctrl;
    uint8_t error;
    uint32_t dma_addr;

    uint64_t drq_timeout;
    uint64_t motor_timeout;
    uint64_t busy_until;
    uint64_t index_until;

    bool dma_enabled;

    bool data_written;

    emu_icu *icu;
    emu_dma *dma;
} emu_fdc;

typedef enum {
    VPU_CTRL_ENABLE_IRQ = (1 << 0),
    VPU_CTRL_SWAP = (1 << 1),
} vpu_ctrl;
typedef enum {
    VPU_STATUS_VBLANK = (1 << 0),
} vpu_status;
typedef struct {
    uint8_t buffers[2][SCREEN_SIZE];
    uint8_t *source;

    uint32_t frame_ptr;
    uint8_t status;
    uint8_t ctrl;

    uint16_t current_scanline;
    bool frame_completed;

    uint64_t cycles;

    emu_bus *bus;
} emu_vpu;

typedef enum {
    DEVICE_ICU,
    DEVICE_DMA,
    DEVICE_FDC,
    DEVICE_VPU,
    DEVICE_COUNT
} device_type;
typedef struct {
    uint8_t *(*access)(void *device, bool write, uint32_t addr);
    void *device;
} bus_device;
struct emu_bus {
    uint8_t *ram;
    uint8_t *rom;
    bus_device devices[DEVICE_COUNT];
};

typedef enum { BUS_OK, BUS_ERROR_ACCESS_VIOLATION, BUS_ERROR_ALIGNMENT_FAULT } bus_result;
bus_result bus_read8(emu_bus *bus, uint32_t addr, uint8_t *out, uint64_t *cycle_counter);
bus_result bus_read16(emu_bus *bus, uint32_t addr, uint16_t *out, uint64_t *cycle_counter);
bus_result bus_read32(emu_bus *bus, uint32_t addr, uint32_t *out, uint64_t *cycle_counter);
bus_result bus_write8(emu_bus *bus, uint32_t addr, uint8_t value, uint64_t *cycle_counter);
bus_result bus_write16(emu_bus *bus, uint32_t addr, uint16_t value, uint64_t *cycle_counter);
bus_result bus_write32(emu_bus *bus, uint32_t addr, uint32_t value, uint64_t *cycle_counter);

typedef enum {
    BLOCK_SYSTEM, BLOCK_FLOW, BLOCK_MOVE, BLOCK_LOAD_IMM, BLOCK_LOAD_IDX,
    BLOCK_STORE_IDX, BLOCK_LOAD_IND, BLOCK_STORE_IND, BLOCK_ARITH, BLOCK_MULDIV,
    BLOCK_LOGIC, BLOCK_SHIFT, BLOCK_BIT, BLOCK_UNARY, BLOCK_STACK, BLOCK_CMP,
} opcode_block;

typedef enum { FLAG_ZERO, FLAG_SIGN, FLAG_OVERFLOW, FLAG_CARRY, FLAG_INTERRUPT, FLAG_HALTED, FLAG_COUNT } emu_flag;
typedef enum { REG_A, REG_X, REG_Y, REG_I } emu_register;

typedef struct {
    uint32_t registers[REG_COUNT]; // Index register for memory addresses

    uint32_t ip; // Instruction pointer
    uint32_t sp; // Stack pointer
    uint64_t cycles;
    uint8_t flags;

    bool nmi_pending;
    bool in_exception;
    bool debug_mode; // Emulator flag (not in the real CPU)
    emu_bus *bus;
} emu_cpu;
typedef struct {
    emu_cpu cpu;
    emu_vpu vpu;
    emu_dma dma;
    emu_icu icu;
    emu_fdc fdc;
    emu_bus bus;
} emulator;

static inline void raise_irq(emu_icu *icu, uint8_t irq) {
    icu->irq_pending |= (1 << irq);
}
static inline void raise_nmi(emu_cpu *cpu, uint8_t nmi) {
    emu_icu *icu = (emu_icu*)&cpu->bus->devices[DEVICE_ICU];
    icu->nmi_status |= 1 << nmi;
    cpu->nmi_pending = true;
}
static inline bool get_flag(emu_cpu *cpu, uint8_t flag) {
    return (cpu->flags >> flag) & 1;
}
static inline void set_flag(emu_cpu *cpu, uint8_t flag, bool value) {
    if (value) cpu->flags |= (1 << flag);
    else cpu->flags &= ~(1 << flag);
}

emulator emu_init(uint8_t *rom, uint8_t *disk, bool debug_mode);

void cpu_step(emu_cpu *cpu);
bool dma_step(emu_dma *dma, uint64_t *cycle_counter);
void vpu_tick(emu_vpu *vpu, uint64_t cycles);

static inline void emu_free(emulator *emu) {
    my_free(emu->bus.rom);
    my_free(emu->bus.ram);
}

extern const char *opcode_block_str[];
#endif