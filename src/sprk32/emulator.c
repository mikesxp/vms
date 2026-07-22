#include "emulator.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>

const char *opcode_block_str[] = {
    [BLOCK_SYSTEM]    = "system",    [BLOCK_FLOW]      = "flow",
    [BLOCK_MOVE]      = "move",      [BLOCK_LOAD_IMM]  = "load imm",
    [BLOCK_LOAD_IDX]  = "load idx",  [BLOCK_STORE_IDX] = "store idx",
    [BLOCK_LOAD_IND]  = "load ind",  [BLOCK_STORE_IND] = "store ind",
    [BLOCK_ARITH]     = "arith",     [BLOCK_MULDIV]    = "muldiv",
    [BLOCK_LOGIC]     = "logic",     [BLOCK_SHIFT]     = "shift",
    [BLOCK_BIT]       = "bit",       [BLOCK_UNARY]     = "unary",
    [BLOCK_STACK]     = "stack",     [BLOCK_CMP]       = "cmp",
};

#define ROM_END (ROM_ADDR + ROM_SIZE)
#define RAM_END (RAM_ADDR + RAM_SIZE)
#define IO_END  (IO_ADDR  + IO_SIZE)

#define ADDR_IN_IO(addr) (addr) >= IO_ADDR && (addr) < IO_END
#define SIGN_BIT_32(x) ((x) & 0x80000000)

#define VPU_VBLANK_LINES   12
#define TOTAL_SCANLINES    (SCREEN_HEIGHT + VPU_VBLANK_LINES)
#define CYCLES_PER_SCANLINE (CPU_HZ / (FRAME_RATE * TOTAL_SCANLINES))

static inline uint32_t swap32(uint32_t v) {
    return ((v & 0x000000FF) << 24) | ((v & 0x0000FF00) << 8)  |
           ((v & 0x00FF0000) >> 8)  | ((v & 0xFF000000) >> 24);
}
static inline bool host_is_little_endian() {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return true;
#elif defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return false;
#else
    const uint16_t x = 1;
    return *((const uint8_t *)&x) == 1;
#endif
}

static inline uint8_t *get_be_ptr16(uint16_t *value, unsigned index) {
    if (index >= SIZE_HWORD) return 0;
    uint8_t *p = (uint8_t *)value;
    if (host_is_little_endian()) return p + (1 - index);
    return p + index;
}
static inline uint8_t *get_be_ptr32(uint32_t *value, uint8_t index) {
    if (index >= SIZE_WORD) return 0;
    uint8_t *p = (uint8_t *)value;
    if (host_is_little_endian()) return p + (3 - index);
    return p + index;
}

static void fdc_set_error(emu_fdc *fdc, uint8_t error);
static uint32_t fdc_disk_offset(emu_fdc *fdc, uint8_t track, uint8_t head, uint8_t sector);
static inline void fdc_clear_busy(emu_fdc *fdc) {
    fdc->status &= ~FDC_STATUS_BUSY;
    fdc->busy_until = 0;
}
static inline uint8_t fdc_get_current_sector(const emu_fdc *fdc, uint64_t current_time) {
    uint64_t pos = current_time % FDC_ROTATION_TIME_NS;
    return (uint8_t)(pos / FDC_SECTOR_TIME_NS) + 1;
}
static inline uint64_t fdc_wait_for_sector(uint64_t current_time, uint8_t target_sector) {
    uint64_t pos = current_time % FDC_ROTATION_TIME_NS;
    uint64_t target_pos = (uint64_t)(target_sector - 1) * FDC_SECTOR_TIME_NS;

    if (target_pos >= pos) return target_pos - pos;
    return FDC_ROTATION_TIME_NS - pos + target_pos;
}
static inline void fdc_drq(emu_fdc *fdc, uint64_t current_time) {
    fdc->drq_timeout = current_time + FDC_DRQ_TIMEOUT_NS;
    fdc->status |= FDC_STATUS_DRQ;
}

static inline void fdc_finish_transfer(emu_fdc *fdc, uint64_t current_time) {
    fdc->status &= ~(FDC_STATUS_BUSY | FDC_STATUS_DRQ);
    fdc->state = FDC_STATE_IDLE;
    fdc_clear_busy(fdc);
    raise_irq(fdc->icu, IVT_IRQ_FDC);
}
static void fdc_dispatch_command(emu_fdc *fdc, uint64_t current_time) {
    if (fdc->state == FDC_STATE_CMD_ARG1) {
        fdc->cmd_arg[0] = fdc->data;
        if (fdc->cmd_arg_count == 2) {
            fdc->state = FDC_STATE_CMD_ARG2;
            return;
        }
        goto execute;
    }

    if (fdc->state == FDC_STATE_CMD_ARG2) {
        fdc->cmd_arg[1] = fdc->data;
        goto execute;
    }

    // New command byte
    fdc->command = fdc->data & 0x0F;
    fdc->cmd_arg_idx = 0;
    fdc->status &= ~FDC_STATUS_ERROR;

    switch (fdc->command) {
    case FDC_CMD_RECAL:
        fdc->target_head = 0;
        fdc->target_track = 0;
        fdc->cmd_arg_count = 0;
        fdc->target_sector = 1;
        goto execute;
    case FDC_CMD_STATUS:
    case FDC_CMD_ID:
        fdc->cmd_arg_count = 0;
        goto execute;
    case FDC_CMD_SEEK:
        fdc->cmd_arg_count = 1; // Arg0 = target track
        fdc->state = FDC_STATE_CMD_ARG1;
        return;
    case FDC_CMD_READ:
    case FDC_CMD_WRITE:
    case FDC_CMD_FORMAT:
        fdc->cmd_arg_count = 2; // Arg0 = track, Arg1 = sector
        fdc->state = FDC_STATE_CMD_ARG1;
        return;
    default:
        // Unknown command error
        fdc_set_error(fdc, FDC_ERROR_INVALID_COMMAND);
        return;
    }

execute:
    // Execute the command with all arguments collected
    fdc->status |= FDC_STATUS_BUSY;

    switch (fdc->command) {
    case FDC_CMD_RECAL:
        fdc->busy_until = current_time + FDC_STEP_TIME_NS;
        fdc->state = FDC_STATE_BUSY_RECAL;

        fdc->ctrl |= FDC_CTRL_MOTOR;
        break;
    case FDC_CMD_SEEK:
        fdc->target_track = fdc->cmd_arg[0];
        if (fdc->target_track >= FDC_TRACKS) {
            fdc_set_error(fdc, FDC_ERROR_SEEK);
            return;
        }
        fdc->state = FDC_STATE_BUSY_SEEK;
        fdc->busy_until = current_time + FDC_STEP_TIME_NS;
        fdc->ctrl |= FDC_CTRL_MOTOR;
        break;
    case FDC_CMD_READ:
    case FDC_CMD_WRITE:
        fdc->target_track = fdc->cmd_arg[0];
        if (fdc->target_track >= FDC_TRACKS) {
            fdc_set_error(fdc, FDC_ERROR_SEEK);
            return;
        }
        fdc->target_sector = fdc->cmd_arg[1];
        if (fdc->target_sector == 0 || fdc->target_sector > FDC_SECTORS) {
            fdc_set_error(fdc, FDC_ERROR_RECORD_NOT_FOUND);
            return;
        }
        fdc->target_head = (fdc->ctrl & FDC_CTRL_HEAD) ? 1 : 0;

        bool motor_was_off = !(fdc->ctrl & FDC_CTRL_MOTOR);
        fdc->ctrl |= FDC_CTRL_MOTOR;

        if (motor_was_off) {
            // Motor needs to spin up first
            fdc->state = FDC_STATE_BUSY_SPIN;
            fdc->busy_until = current_time + FDC_SPIN_UP_NS;
        } else if (fdc->current_track != fdc->target_track) {
            fdc->state = FDC_STATE_BUSY_SEEK;
            fdc->busy_until = current_time + FDC_STEP_TIME_NS;
        } else {
            fdc->state = FDC_STATE_BUSY_SEARCH;
            fdc->busy_until = current_time + fdc_wait_for_sector(current_time, fdc->target_sector);
        }

        fdc->dma_enabled = fdc->ctrl & FDC_CTRL_DMA_ENABLED;
        break;
    case FDC_CMD_FORMAT:
        fdc->target_track = fdc->cmd_arg[0];
        if (fdc->target_track >= FDC_TRACKS) {
            fdc_set_error(fdc, FDC_ERROR_SEEK);
            return;
        }
        fdc->target_head = (fdc->ctrl & FDC_CTRL_HEAD) ? 1 : 0;
        fdc->ctrl |= FDC_CTRL_MOTOR;

        if (!(fdc->status & FDC_STATUS_WRITE_PROTECTED) &&
            fdc->status & FDC_STATUS_DISK_IN && fdc->disk_data)
        {
            for (uint8_t s = 1; s <= FDC_SECTORS; s++) {
                uint32_t offset = fdc_disk_offset(fdc, fdc->target_track, fdc->target_head, s);
                if (offset + FDC_SECTOR_SIZE <= FDC_DISK_SIZE)
                    memset(fdc->disk_data + offset, 0xE5, FDC_SECTOR_SIZE);
            }
            fdc->state = FDC_STATE_IDLE;
            fdc->status |= FDC_STATUS_BUSY;
            fdc->busy_until = current_time + FDC_ROTATION_TIME_NS;
            raise_irq(fdc->icu, IVT_IRQ_FDC);
        }
        break;
    case FDC_CMD_STATUS:
    case FDC_CMD_ID:
        // Return a small result packet: track, head, sector
        fdc->result[0] = fdc->current_track;
        fdc->result[1] = fdc->current_head;
        fdc->result[2] = fdc->current_sector;
        fdc->result[3] = fdc->status & FDC_STATUS_DISK_IN;
        fdc->result[4] = fdc->status & FDC_STATUS_WRITE_PROTECTED;
        fdc->result_len = 5;
        fdc->result_idx = 0;
        fdc->state = FDC_STATE_RESULT;
        fdc_drq(fdc, current_time);
        break;
    }
}

static void fdc_update_status_reg(emu_fdc *fdc);
static void fdc_step(emu_fdc *fdc, uint64_t *cycle_counter, uint64_t current_time) {
    uint64_t rotation = current_time / FDC_ROTATION_TIME_NS;
    if (rotation != fdc->last_rotation) {
        fdc->last_rotation = rotation;
        fdc->status |= FDC_STATUS_INDEX;
        fdc->index_until = current_time + FDC_INDEX_DURATION_NS;
        return;
    }
    if (fdc->index_until != 0 && current_time >= fdc->index_until) {
        fdc->status &= ~FDC_STATUS_INDEX;
        fdc->index_until = 0;
        return;
    }

    if (current_time < fdc->busy_until) {
        fdc_update_status_reg(fdc);
        if (fdc->data_written) fdc->data_written = false;
        return;
    }

    if (fdc->status & FDC_STATUS_DRQ) {
        if (current_time >= fdc->drq_timeout)
            fdc_set_error(fdc, FDC_ERROR_LOST_DATA);
        return;
    }

    switch (fdc->state) {
    case FDC_STATE_IDLE:
        if (fdc->data_written) {
            fdc_dispatch_command(fdc, current_time);
            fdc->data_written = false;
            break;
        }

        if (!(fdc->ctrl & FDC_CTRL_MOTOR)) { // The motor is not ON
            fdc->motor_timeout = 0;
            break;
        }
        if (fdc->motor_timeout == 0) {
            fdc->motor_timeout = current_time + FDC_MOTOR_TIMEOUT_NS;
            break;
        }
        if (current_time >= fdc->motor_timeout) fdc->ctrl &= ~FDC_CTRL_MOTOR;
        break;
    case FDC_STATE_BUSY_RECAL:
        if (fdc->current_track > 0) {
            fdc->busy_until = current_time + FDC_STEP_TIME_NS;
            fdc->current_track--;
            break;
        }

        fdc->status |= FDC_STATUS_TRACK0;
        fdc->status |= FDC_STATUS_SEEK_DONE;

        fdc_finish_transfer(fdc, current_time);
        raise_irq(fdc->icu, IVT_IRQ_FDC);
        break;
    case FDC_STATE_BUSY_SPIN:
        fdc->state = FDC_STATE_BUSY_SEARCH;
        fdc->busy_until = current_time +
            fdc_wait_for_sector(current_time, fdc->target_sector);;
        break;
    case FDC_STATE_BUSY_SEEK: {
        if (fdc->current_track < fdc->target_track) fdc->current_track++;
        else if (fdc->current_track > fdc->target_track) fdc->current_track--;

        if (fdc->current_track == fdc->target_track) {
            fdc->status |= FDC_STATUS_SEEK_DONE;
            fdc->status &= ~FDC_STATUS_BUSY;

            if (fdc->current_track == 0) fdc->status |= FDC_STATUS_TRACK0;
            else fdc->status &= ~FDC_STATUS_TRACK0;

            if (fdc->command == FDC_CMD_SEEK || fdc->command == FDC_CMD_RECAL) {
                fdc->state = FDC_STATE_IDLE;
                fdc_clear_busy(fdc);
                raise_irq(fdc->icu, IVT_IRQ_FDC);
                break;
            }

            fdc->state = FDC_STATE_BUSY_SEARCH;
            fdc->busy_until = current_time + fdc_wait_for_sector(current_time, fdc->target_sector);
            break;
        }
        fdc->busy_until = current_time + FDC_STEP_TIME_NS;
        break;
    }
    case FDC_STATE_BUSY_SEARCH: {
        if (!(fdc->status & FDC_STATUS_DISK_IN)) {
            fdc_set_error(fdc, FDC_ERROR_NOT_READY);
            break;
        }

        fdc->current_head = fdc->target_head;
        fdc->current_sector = fdc_get_current_sector(fdc, current_time);
        if (fdc->current_sector != fdc->target_sector) {
            fdc->busy_until = current_time + FDC_SECTOR_TIME_NS / 8;
            return;
        }

        // The requested sector is now under the head, load the sector data into the transfer buffer
        uint32_t offset = fdc_disk_offset(fdc, fdc->current_track, fdc->current_head, fdc->current_sector);
        if (fdc->command == FDC_CMD_READ || fdc->command == FDC_CMD_ID) {
            if (offset + FDC_SECTOR_SIZE > FDC_DISK_SIZE) {
                fdc_set_error(fdc, FDC_ERROR_RECORD_NOT_FOUND);
                break;
            }
            memcpy(fdc->transfer_buffer, fdc->disk_data + offset, FDC_SECTOR_SIZE);
        } else if (fdc->status & FDC_STATUS_WRITE_PROTECTED) {
            fdc->status |= FDC_STATUS_WRITE_PROTECTED;
            fdc_set_error(fdc, FDC_ERROR_WRITE_PROTECT);
            break;
        }

        fdc->transfer_position = 0;
        fdc->transfer_len = FDC_SECTOR_SIZE;

        if (fdc->dma_enabled) {
            bool is_write = fdc->command == FDC_CMD_WRITE;
            // Move the whole sector with DMA
            if (is_write) {
                if (offset + FDC_SECTOR_SIZE > FDC_DISK_SIZE) {
                    fdc_set_error(fdc, FDC_ERROR_RECORD_NOT_FOUND);
                    break;
                }
                fdc->dma->ctrl |= DMA_CTRL_DST_FIXED;
                fdc->dma->ctrl &= ~DMA_CTRL_SRC_FIXED;
                fdc->dma->dst = FDC_DATA_ADDR;
                fdc->dma->src = fdc->dma_addr;
            } else {
                fdc->dma->ctrl |= DMA_CTRL_SRC_FIXED;
                fdc->dma->ctrl &= ~DMA_CTRL_DST_FIXED;
                fdc->dma->dst = fdc->dma_addr;
                fdc->dma->src = FDC_DATA_ADDR;
            }

            fdc->dma->ctrl |= DMA_CTRL_ENABLE;
            fdc_drq(fdc, current_time);
            break;
        }

        // Programmed IO mode: assert DRQ so CPU can read/write bytes
        fdc_drq(fdc, current_time);
        fdc->state = (fdc->command == FDC_CMD_READ || fdc->command == FDC_CMD_ID)
                        ? FDC_STATE_TRANSFER_READ : FDC_STATE_TRANSFER_WRITE;
        fdc->busy_until = current_time + FDC_BYTE_TIME_NS;
        break;
    }
    case FDC_STATE_TRANSFER_READ: {
        if (fdc->transfer_position >= fdc->transfer_len) {
            fdc_finish_transfer(fdc, current_time);
            break;
        }
        fdc->data_written = false;
        fdc_drq(fdc, current_time);
        fdc->busy_until = current_time + FDC_BYTE_TIME_NS;

        if (dma_step(fdc->dma, cycle_counter))
            fdc->data = fdc->transfer_buffer[fdc->transfer_position++];
        break;
    }
    case FDC_STATE_TRANSFER_WRITE: {
        if (fdc->transfer_position >= fdc->transfer_len) {
            uint32_t offset = fdc_disk_offset(fdc, fdc->current_track, fdc->current_head, fdc->current_sector);
            if (fdc->disk_data && offset + FDC_SECTOR_SIZE <= FDC_DISK_SIZE)
                memcpy(fdc->disk_data + offset, fdc->transfer_buffer, FDC_SECTOR_SIZE);

            fdc_finish_transfer(fdc, current_time);
            break;
        }
        fdc->data_written = false;
        fdc_drq(fdc, current_time);
        fdc->busy_until = current_time + FDC_BYTE_TIME_NS;

        if (dma_step(fdc->dma, cycle_counter))
            fdc->data = fdc->transfer_buffer[fdc->transfer_position++];
        break;
    }
    default: break;
    }

    fdc_update_status_reg(fdc);
}

static uint8_t *fdc_access(void *device, bool write, uint32_t addr) {
    emu_fdc *fdc = (emu_fdc*)device;
    if (addr >= FDC_ADDR + FDC_SIZE) return NULL;

    if (addr >= FDC_DMA_ADDR && addr < FDC_DMA_ADDR + SIZE_WORD)
        return get_be_ptr32(&fdc->dma_addr, addr - FDC_DMA_ADDR);
    if (addr >= FDC_DATA_ADDR && addr < FDC_DATA_ADDR + SIZE_WORD) {
        uint8_t *ptr = get_be_ptr32(&fdc->data, addr - FDC_DATA_ADDR);
        if (write) {
            fdc->data_written = true;
            return ptr;
        }

        if (fdc->state == FDC_STATE_RESULT) {
            // Feed result bytes one at a time
            if (fdc->result_idx < fdc->result_len)
                fdc->data = fdc->result[fdc->result_idx++];

            if (fdc->result_idx >= fdc->result_len) {
                fdc->state = FDC_STATE_IDLE;
                fdc->status &= ~(FDC_STATUS_BUSY | FDC_STATUS_DRQ);
                fdc->busy_until = 0;
            }
            return ptr;
        } 
        
        if (fdc->state == FDC_STATE_TRANSFER_READ) {
            if (fdc->transfer_position < fdc->transfer_len)
                fdc->data = fdc->transfer_buffer[fdc->transfer_position++];
            else fdc->status &= ~FDC_STATUS_DRQ;
        }
        return ptr;
    }
    if (!write) {
        switch (addr) {
        case FDC_STATUS_ADDR:
            fdc_update_status_reg(fdc);
            return &fdc->status;
        case FDC_ERROR_ADDR:  return &fdc->error;
        case FDC_SECTOR_ADDR: return &fdc->current_sector;
        case FDC_TRACK_ADDR:  return &fdc->current_track;
        case FDC_CTRL_ADDR:   return &fdc->ctrl;
        }
        return NULL;
    }

    if (addr == FDC_CTRL_ADDR) return &fdc->ctrl;
    return NULL;
}

static uint32_t fdc_disk_offset(emu_fdc *fdc, uint8_t track, uint8_t head, uint8_t sector) {
    return ((uint32_t)track * FDC_HEADS * FDC_SECTORS
          + (uint32_t)head * FDC_SECTORS + (uint32_t)(sector - 1)) * FDC_SECTOR_SIZE;
}

static void fdc_update_status_reg(emu_fdc *fdc) {
    fdc->status &= ~FDC_STATUS_TRACK0;
    if (fdc->current_track == 0) fdc->status |= FDC_STATUS_TRACK0;

    switch (fdc->state) {
    case FDC_STATE_IDLE: break;
    case FDC_STATE_CMD_ARG1:
    case FDC_STATE_CMD_ARG2:
    case FDC_STATE_BUSY_SEEK:
    case FDC_STATE_BUSY_SPIN:
    case FDC_STATE_BUSY_SEARCH:
    case FDC_STATE_BUSY_RECAL:
        fdc->status |= FDC_STATUS_BUSY;
        break;
    case FDC_STATE_TRANSFER_READ:
    case FDC_STATE_TRANSFER_WRITE:
    case FDC_STATE_RESULT:
        fdc->status |= FDC_STATUS_BUSY | FDC_STATUS_DRQ;
        break;
    case FDC_STATE_ERROR:
        fdc->status |= FDC_STATUS_ERROR | FDC_STATUS_BUSY;
        break;
    }
}

static void fdc_set_error(emu_fdc *fdc, uint8_t error) {
    fdc->error = error;

    fdc->status |= FDC_STATUS_ERROR;
    fdc->status &= ~(FDC_STATUS_BUSY | FDC_STATUS_DRQ);

    fdc->busy_until = 0;
    fdc->state = FDC_STATE_IDLE;

    raise_irq(fdc->icu, IVT_IRQ_FDC);
}

void vpu_tick(emu_vpu *vpu, uint64_t cycles) {
    vpu->cycles += cycles;

    while (vpu->cycles >= CYCLES_PER_SCANLINE) {
        vpu->cycles -= CYCLES_PER_SCANLINE;    
        vpu->current_scanline++;

        if (vpu->current_scanline == SCREEN_HEIGHT) {
            vpu->status |= VPU_STATUS_VBLANK;

            if (vpu->ctrl & VPU_CTRL_SWAP) {
                if (vpu->frame_ptr == VPU_BUFFER0_ADDR) {
                    vpu->frame_ptr = VPU_BUFFER1_ADDR;
                    vpu->source = vpu->buffers[0];
                } else {
                    vpu->frame_ptr = VPU_BUFFER0_ADDR;
                    vpu->source = vpu->buffers[1];
                }
                vpu->ctrl &= ~VPU_CTRL_SWAP;
            }

            if (vpu->ctrl & VPU_CTRL_ENABLE_IRQ)
                raise_irq(vpu->bus->devices[DEVICE_ICU].device, IVT_IRQ_VBLANK);
        }

        if (vpu->current_scanline >= TOTAL_SCANLINES) {
            vpu->status &= ~VPU_STATUS_VBLANK;
            vpu->current_scanline = 0;
            vpu->frame_completed = true;
        }
    }
}

static void enter_interrupt(emu_cpu *cpu, bool is_nmi, uint32_t base, uint32_t n);
static inline void enter_exception(emu_cpu *cpu, ivt_exc index) {
#ifdef DEBUG
    printf("[CPU] enter exception %d\n", index);
#endif
    if (cpu->in_exception) {
        set_flag(cpu, FLAG_HALTED, true);
        return;
    }
    cpu->in_exception = true;
    enter_interrupt(cpu, false, IVT_EXC_BASE_ADDR, index);
}
static inline bool dma_finish_write(emu_dma *dma, prim_size width) {
    if (!(dma->ctrl & DMA_CTRL_SRC_FIXED)) dma->src += width;
    if (!(dma->ctrl & DMA_CTRL_DST_FIXED)) dma->dst += width;
    dma->count -= width;

    if (dma->count == 0) return true;
    dma->state = DMA_STATE_READ_LO;
    return false;
}
bool dma_step(emu_dma *dma, uint64_t *cycle_counter) {
    if (!(dma->ctrl & DMA_CTRL_ENABLE)) {
        dma->state = DMA_STATE_IDLE;
        return false;
    }

    prim_size width;
    switch (dma->ctrl & DMA_CTRL_WIDTH_MASK) {
    case 0: width = SIZE_BYTE;  break;
    case 1: width = SIZE_HWORD; break;
    case 2: width = SIZE_WORD;  break;
    default: goto config_error;
    }

    bus_result result = BUS_OK;
    bool is_word = width == SIZE_WORD;
    uint32_t hi_offset = is_word ? SIZE_HWORD : 0;
    switch (dma->state) {
    case DMA_STATE_IDLE: {
        if (dma->count == 0 || dma->count < width) goto config_error;

        dma->status |= DMA_STATUS_BUSY;
        dma->state = DMA_STATE_READ_LO;
        *cycle_counter += 1;
        return true;
    }
    case DMA_STATE_READ_LO:
        if (width == SIZE_BYTE) {
            uint8_t byte_val;
            result = bus_read8(dma->bus, dma->src, &byte_val, cycle_counter);
            dma->buffer_lo = byte_val;
            dma->state = DMA_STATE_WRITE_LO;
            break;
        }

        result = bus_read16(dma->bus, dma->src, &dma->buffer_lo, cycle_counter);
        dma->state = is_word ? DMA_STATE_READ_HI : DMA_STATE_WRITE_LO;
        break;
    case DMA_STATE_WRITE_LO:
        switch (width) {
        case SIZE_BYTE:
            result = bus_write8(dma->bus, dma->dst, (uint8_t)dma->buffer_lo, cycle_counter);
            break;
        case SIZE_WORD: dma->state = DMA_STATE_WRITE_HI;
        default:
            result = bus_write16(dma->bus, dma->dst, dma->buffer_lo, cycle_counter);
            break;
        }
        if (dma_finish_write(dma, width)) goto transfer_complete;
        break;
    case DMA_STATE_READ_HI:
        result = bus_read16(dma->bus, dma->src + hi_offset, &dma->buffer_hi, cycle_counter);
        dma->state = DMA_STATE_WRITE_LO;
        break;
    case DMA_STATE_WRITE_HI:
        result = bus_write16(dma->bus, dma->dst + hi_offset, dma->buffer_hi, cycle_counter);
        if (dma_finish_write(dma, width)) goto transfer_complete;
        break;
    }

    switch (result) {
    case BUS_OK: return true;
    case BUS_ERROR_ALIGNMENT_FAULT:
        dma->status |= DMA_STATUS_ALIGNMENT_ERROR;
        goto error;
    case BUS_ERROR_ACCESS_VIOLATION:
        dma->status |= DMA_STATUS_ACCESS_ERROR;
        goto error;
    }

transfer_complete:
    dma->status &= ~DMA_STATUS_BUSY;
    dma->ctrl &= ~DMA_CTRL_ENABLE;
    dma->state = DMA_STATE_IDLE;
    raise_irq(dma->icu, IVT_IRQ_DMA);
    return true;

config_error:
    dma->status |= DMA_STATUS_CONFIG_ERROR;
error:
    dma->status &= ~DMA_STATUS_BUSY;
    dma->ctrl &= ~DMA_CTRL_ENABLE;
    dma->state = DMA_STATE_IDLE;
    raise_irq(dma->icu, IVT_IRQ_DMA);
    return false;
}

static uint8_t *dma_access(void *device, bool write, uint32_t addr) {
    emu_dma *dma = (emu_dma*)device;
    if (addr == DMA_CTRL_ADDR)   return &dma->ctrl;
    if (addr == DMA_STATUS_ADDR && !write) return &dma->status;

    if (addr < DMA_SRC_ADDR) return NULL;
    if (addr < DMA_SRC_ADDR + SIZE_WORD) return get_be_ptr32(&dma->src, addr - DMA_SRC_ADDR);
    if (addr < DMA_DST_ADDR + SIZE_WORD) return get_be_ptr32(&dma->dst, addr - DMA_DST_ADDR);
    if (addr < DMA_COUNT_ADDR + SIZE_WORD) return get_be_ptr32(&dma->count, addr - DMA_COUNT_ADDR);
    return NULL;
}
static uint8_t *vpu_access(void *device, bool write, uint32_t addr) {
    emu_vpu *vpu = (emu_vpu*)device;
    if (addr < VPU_ADDR) return NULL;

    if (!write) {
        if (addr == VPU_STATUS_ADDR) return &vpu->status;
        if (addr < VPU_FRAME_PTR_ADDR + 4)
            return get_be_ptr32(&vpu->frame_ptr, addr - VPU_FRAME_PTR_ADDR);
    }
    if (addr == VPU_CTRL_ADDR) return &vpu->ctrl;
    if (addr >= VPU_BUFFER0_ADDR && addr < VPU_BUFFER0_ADDR + SCREEN_SIZE)
        return &vpu->buffers[0][addr - VPU_BUFFER0_ADDR];
    if (addr >= VPU_BUFFER1_ADDR && addr < VPU_BUFFER1_ADDR + SCREEN_SIZE)
        return &vpu->buffers[1][addr - VPU_BUFFER1_ADDR];
    return NULL;
}
static uint8_t *icu_access(void *device, bool write, uint32_t addr) {
    emu_icu *icu = (emu_icu*)device;
    switch (addr) {
    case ICU_NMI_STATUS_ADDR:  return &icu->nmi_status;
    case ICU_IRQ_ENABLE_ADDR:  return &icu->irq_enable;
    case ICU_IRQ_PENDING_ADDR: return &icu->irq_pending;
    }
    return NULL;
}
static inline bus_result bus_validate16(uint32_t addr) {
    if (addr & 1) return BUS_ERROR_ALIGNMENT_FAULT;
    if (ADDR_IN_IO(addr) && addr + 1 >= IO_END) return BUS_ERROR_ACCESS_VIOLATION;
    return BUS_OK;
}
static bus_result bus_raw_access8(emu_bus *bus, uint32_t addr, uint8_t **ptr, bool write, uint64_t *cycle_counter) {
    if (!write && addr < ROM_END) {
        if (cycle_counter) *cycle_counter += 2;
        *ptr = &bus->rom[addr];
        return BUS_OK;
    }
    if (addr >= RAM_ADDR && addr < RAM_END) {
        if (cycle_counter) *cycle_counter += 1;
        *ptr = &bus->ram[addr - RAM_ADDR];
        return BUS_OK;
    }

    if (cycle_counter) *cycle_counter += 4;
    for (int i = 0; i < ARRAYLEN(bus->devices); i++) {
        bus_device *dev = &bus->devices[i];
        *ptr = dev->access(dev->device, write, addr);
        if (*ptr) return BUS_OK;
    }
    return BUS_ERROR_ACCESS_VIOLATION;
}
static bus_result bus_raw_access16(
    emu_bus *bus, uint32_t addr, uint8_t **ptr1, uint8_t **ptr2, bool write, uint64_t *cycle_counter
) {
    bus_result exc = bus_validate16(addr);
    if (exc != BUS_OK) return exc;
    exc = bus_raw_access8(bus, addr, ptr1, write, cycle_counter);
    if (exc != BUS_OK) return exc;

    exc = bus_raw_access8(bus, addr + 1, ptr2, write, cycle_counter);
    return exc;
}

bus_result bus_read8(emu_bus *bus, uint32_t addr, uint8_t *out, uint64_t *cycle_counter) {
    uint8_t *ptr = NULL;
    bus_result exc = bus_raw_access8(bus, addr, &ptr, false, cycle_counter);
    if (exc == BUS_OK) *out = *ptr;
    return exc;
}

bus_result bus_read16(emu_bus *bus, uint32_t addr, uint16_t *out, uint64_t *cycle_counter) {
    uint8_t *ptr1 = NULL, *ptr2 = NULL;
    bus_result exc = bus_raw_access16(bus, addr, &ptr1, &ptr2, false, cycle_counter);

    if (exc == BUS_OK) {
        if (!ptr1 || !ptr2) return BUS_ERROR_ACCESS_VIOLATION;
        *out = (*ptr1 << 8) | *ptr2;
    }
    return exc;
}

bus_result bus_read32(emu_bus *bus, uint32_t addr, uint32_t *out, uint64_t *cycle_counter) {
    uint16_t hi, lo;

    bus_result exc = bus_read16(bus, addr, &hi, cycle_counter);
    if (exc != BUS_OK) return exc;

    exc = bus_read16(bus, addr + 2, &lo, cycle_counter);
    if (exc != BUS_OK) return exc;

    *out = ((uint32_t)hi << 16) | lo;
    return BUS_OK;
}

bus_result bus_write8(emu_bus *bus, uint32_t addr, uint8_t value, uint64_t *cycle_counter) {
    uint8_t *ptr;
    bus_result exc = bus_raw_access8(bus, addr, &ptr, true, cycle_counter);
    if (exc == BUS_OK) *ptr = value;
    return exc;
}

bus_result bus_write16(emu_bus *bus, uint32_t addr, uint16_t value, uint64_t *cycle_counter) {
    uint8_t *ptr1 = NULL, *ptr2 = NULL;
    bus_result exc = bus_raw_access16(bus, addr, &ptr1, &ptr2, true, cycle_counter);
    if (exc == BUS_OK) {
        *ptr1 = value >> 8;
        *ptr2 = value & 0xFF;
    }
    return exc;
}

bus_result bus_write32(emu_bus *bus, uint32_t addr, uint32_t value, uint64_t *cycle_counter) {
    bus_result exc = bus_write16(bus, addr, value >> 16, cycle_counter);
    if (exc == BUS_OK)
        exc = bus_write16(bus, addr + 2, value & 0xFFFF, cycle_counter);
    return exc;
}

static inline bool cpu_handle_bus_exception(emu_cpu *cpu, bus_result exc) {
    switch (exc) {
    case BUS_OK: return true;
    case BUS_ERROR_ALIGNMENT_FAULT:
        enter_exception(cpu, IVT_EXC_ALIGNMENT_FAULT);
        break;
    case BUS_ERROR_ACCESS_VIOLATION:
        enter_exception(cpu, IVT_EXC_ACCESS_VIOLATION);
        break;
    }
    return false;
}
static inline uint8_t cpu_read8(emu_cpu *cpu, uint32_t addr) {
    uint8_t value;
    bus_result exc = bus_read8(cpu->bus, addr, &value, &cpu->cycles);
    if (!cpu_handle_bus_exception(cpu, exc)) return 0;
    return value;
}
static inline uint16_t cpu_read16(emu_cpu *cpu, uint32_t addr) {
    uint16_t value;
    bus_result exc = bus_read16(cpu->bus, addr, &value, &cpu->cycles);
    if (!cpu_handle_bus_exception(cpu, exc)) return 0;
    return value;
}
static inline uint32_t cpu_read32(emu_cpu *cpu, uint32_t addr) {
    uint32_t value;
    bus_result exc = bus_read32(cpu->bus, addr, &value, &cpu->cycles);
    if (!cpu_handle_bus_exception(cpu, exc)) return 0;
    return value;
}
static inline void cpu_write8(emu_cpu *cpu, uint32_t addr, uint8_t value) {
    bus_result exc = bus_write8(cpu->bus, addr, value, &cpu->cycles);
    if (exc == BUS_OK) return;

    cpu_handle_bus_exception(cpu, exc);
}
static inline void cpu_write16(emu_cpu *cpu, uint32_t addr, uint16_t value) {
    bus_result exc = bus_write16(cpu->bus, addr, value, &cpu->cycles);
    if (exc == BUS_OK) return;

    cpu_handle_bus_exception(cpu, exc);
}
static inline void cpu_write32(emu_cpu *cpu, uint32_t addr, uint32_t value) {
    bus_result exc = bus_write32(cpu->bus, addr, value, &cpu->cycles);
    if (exc == BUS_OK) return;

    cpu_handle_bus_exception(cpu, exc);
}

static inline uint8_t fetch8(emu_cpu *cpu) {
    return cpu_read8(cpu, cpu->ip++);
}
static inline uint16_t fetch16(emu_cpu *cpu) {
    return ((uint16_t)fetch8(cpu) << 8) | (uint16_t)fetch8(cpu);
}
static inline uint32_t fetch32(emu_cpu *cpu) {
    return ((uint32_t)fetch8(cpu) << 24) |
       ((uint32_t)fetch8(cpu) << 16) |
       ((uint32_t)fetch8(cpu) << 8)  |
       (uint32_t)fetch8(cpu);
}

static inline void set_zs(emu_cpu *cpu, uint32_t result) {
    set_flag(cpu, FLAG_ZERO, result == 0);
    set_flag(cpu, FLAG_SIGN, SIGN_BIT_32(result));
}

static inline void set_add_overflow(emu_cpu *cpu, uint32_t a, uint32_t b, uint32_t r) {
    set_flag(cpu, FLAG_OVERFLOW, SIGN_BIT_32(~(a ^ b) & (a ^ r)));
}
static inline void set_sub_overflow(emu_cpu *cpu, uint32_t a, uint32_t b, uint32_t r) {
    set_flag(cpu, FLAG_OVERFLOW, SIGN_BIT_32((a ^ b) & (a ^ r)));
}
static void op_add(emu_cpu *cpu, uint32_t value) {
    uint64_t result = (uint64_t)cpu->registers[REG_A] + value;
    uint32_t result32 = (uint32_t)result;
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY, result > UINT32_MAX);
    set_add_overflow(cpu, cpu->registers[REG_A], value, result32);
    set_zs(cpu, result32);

    cpu->registers[REG_A] = result32;
}
static void op_adc(emu_cpu *cpu, uint32_t value) {
    uint64_t result = (uint64_t)cpu->registers[REG_A] + (uint64_t)value + (uint64_t)get_flag(cpu, FLAG_CARRY);
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY, result > UINT32_MAX);
    set_add_overflow(cpu, cpu->registers[REG_A], value, (uint32_t)result);
    set_zs(cpu, (uint32_t)result);
    cpu->registers[REG_A] = (uint32_t)result;
}
static void op_sub(emu_cpu *cpu, uint32_t value) {
    uint32_t result = cpu->registers[REG_A] - value;
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY, cpu->registers[REG_A] >= value);
    set_sub_overflow(cpu, cpu->registers[REG_A], value, result);
    set_zs(cpu, result);

    cpu->registers[REG_A] = (uint32_t)result;
}
static void op_sbc(emu_cpu *cpu, uint32_t value) {
    uint32_t inv_carry = get_flag(cpu, FLAG_CARRY) ? 0 : 1;
    uint64_t result = (uint64_t)cpu->registers[REG_A] - value - inv_carry;
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY, result <= UINT32_MAX);
    set_sub_overflow(cpu, cpu->registers[REG_A], value, (uint32_t)result);
    set_zs(cpu, (uint32_t)result);

    cpu->registers[REG_A] = (uint32_t)result;
}
static void op_mul(emu_cpu *cpu, uint32_t value) {
    uint64_t result = (uint64_t)cpu->registers[REG_A] * (uint64_t)value;

    cpu->registers[REG_A] = (uint32_t)result;
    cpu->registers[REG_Y] = (uint32_t)(result >> 32);
    cpu->cycles += 3;

    bool overflowed = (result > UINT32_MAX);
    set_flag(cpu, FLAG_CARRY, overflowed);
    set_flag(cpu, FLAG_OVERFLOW, overflowed);
    set_zs(cpu, (uint32_t)result);
}
static void op_imul(emu_cpu *cpu, uint32_t value) {
    int32_t a = (int32_t)cpu->registers[REG_A];
    int32_t b = (int32_t)value;

    int64_t result = (int64_t)a * (int64_t)b;
    cpu->registers[REG_A] = (uint32_t)result;
    cpu->registers[REG_Y] = (uint32_t)((uint64_t)result >> 32);
    cpu->cycles += 3;

    bool overflowed = (result > INT32_MAX || result < INT32_MIN);
    set_flag(cpu, FLAG_CARRY, overflowed);
    set_flag(cpu, FLAG_OVERFLOW, overflowed);
    set_zs(cpu, cpu->registers[REG_A]);
}
static inline void op_shl(emu_cpu *cpu, uint8_t value) {
    value &= 31;
    if (value == 0) return;

    set_flag(cpu, FLAG_CARRY, (cpu->registers[REG_A] >> (32 - value)) & 1);
    cpu->registers[REG_A] <<= value;
    cpu->cycles++;
    set_zs(cpu, cpu->registers[REG_A]);
}
static void op_shr(emu_cpu *cpu, uint8_t value) {
    value &= 31;
    if (value == 0) return;

    set_flag(cpu, FLAG_CARRY, (cpu->registers[REG_A] >> (value - 1)) & 1);
    cpu->registers[REG_A] >>= value;
    cpu->cycles++;
    set_zs(cpu, cpu->registers[REG_A]);
}
static inline void op_rol(emu_cpu *cpu, uint8_t value) {
    value &= 31;
    if (value == 0) return;

    uint32_t result = (cpu->registers[REG_A] << value) | (cpu->registers[REG_A] >> (32 - value));
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY, (cpu->registers[REG_A] >> (32 - value)) & 1);
    set_flag(cpu, FLAG_OVERFLOW, 0);
    set_zs(cpu, result);

    cpu->registers[REG_A] = (uint32_t)result;
}
static inline void op_ror(emu_cpu *cpu, uint8_t value) {
    value &= 31;
    if (value == 0) return;

    uint32_t result = (cpu->registers[REG_A] >> value) | (cpu->registers[REG_A] << (32 - value));
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY, (cpu->registers[REG_A] >> (value - 1)) & 1);
    set_flag(cpu, FLAG_OVERFLOW, 0);
    set_zs(cpu, result);

    cpu->registers[REG_A] = (uint32_t)result;
}
static inline void op_neg(emu_cpu *cpu, emu_register reg) {
    uint32_t result = 0 - cpu->registers[reg];
    cpu->registers[reg] = result;

    set_flag(cpu, FLAG_CARRY, cpu->registers[reg] == 0);
    set_flag(cpu, FLAG_OVERFLOW, (result == 0x80000000));
    set_zs(cpu, result);
}

static inline void op_cmp(emu_cpu *cpu, emu_register reg, uint32_t value) {
    uint32_t result = cpu->registers[reg] - value;
    cpu->cycles++;

    set_flag(cpu, FLAG_CARRY,  cpu->registers[reg] >= value);
    set_sub_overflow(cpu, cpu->registers[reg], value, result);
    set_zs(cpu, result);
}

static inline void op_div(emu_cpu *cpu, uint32_t value) {
    if (value == 0) {
        enter_exception(cpu, IVT_EXC_DIVISION_BY_ZERO);
        return;
    }

    uint32_t dividend = cpu->registers[REG_A];
    uint32_t result = dividend / value;
    uint32_t rem = dividend % value;

    uint32_t leading = __builtin_clz(value);
    uint32_t complexity = 32 - leading;

    cpu->cycles += 8 + complexity;
    set_zs(cpu, result);

    cpu->registers[REG_A] = (uint32_t)result;
    cpu->registers[REG_Y] = rem;
}

static inline void op_idiv(emu_cpu *cpu, uint32_t value) {
    if (value == 0) {
        enter_exception(cpu, IVT_EXC_DIVISION_BY_ZERO);
        return;
    }

    int32_t dividend = cpu->registers[REG_A];
    int32_t divisor = value;

    // (INT32_MIN / -1) > INT32_MAX
    if (dividend == INT32_MIN && divisor == -1) {
        enter_exception(cpu, IVT_EXC_DIVISION_BY_ZERO);
        return;
    }

    int32_t result = dividend / divisor;
    int32_t rem = dividend % divisor;
    uint32_t abs_div = (divisor < 0) ? -divisor : divisor;
    uint32_t leading = __builtin_clz(abs_div);
    uint32_t complexity = 32 - leading;

    cpu->cycles += 10 + complexity;
    set_zs(cpu, result);

    cpu->registers[REG_A] = (uint32_t)result;
    cpu->registers[REG_Y] = rem;
}

static inline void op_inc(emu_cpu *cpu, uint32_t *value) {
    uint32_t result = *value + 1;
    cpu->cycles++;

    set_add_overflow(cpu, *value, 1, result);
    set_zs(cpu, result);

    *value = result;
}
static inline void op_dec(emu_cpu *cpu, uint32_t *value) {
    uint32_t result = *value - 1;
    cpu->cycles++;

    set_sub_overflow(cpu, *value, 1, result);
    set_zs(cpu, result);

    *value = result;
}

static inline void op_and(emu_cpu *cpu, uint32_t value) {
    cpu->registers[REG_A] &= value;
    cpu->cycles++;
    set_zs(cpu, cpu->registers[REG_A]);
}
static inline void op_test(emu_cpu *cpu, uint32_t value) {
    uint32_t result = cpu->registers[REG_A] & value;
    cpu->cycles++;
    set_zs(cpu, result);
}
static inline void op_or(emu_cpu *cpu, uint32_t value) {
    cpu->registers[REG_A] |= value;
    cpu->cycles++;
    set_zs(cpu, cpu->registers[REG_A]);
}
static inline void op_xor(emu_cpu *cpu, uint32_t value) {
    cpu->registers[REG_A] ^= value;
    cpu->cycles++;
    set_zs(cpu, cpu->registers[REG_A]);
}
static inline void op_not(emu_cpu *cpu, emu_register reg) {
    cpu->registers[reg] = ~cpu->registers[reg];
    cpu->cycles++;
    set_zs(cpu, cpu->registers[reg]);
}

static inline void push(emu_cpu *cpu, uint32_t value) {
    cpu->sp -= 4;
    if (cpu->sp < STACK_BOTTOM) {
        enter_exception(cpu, IVT_EXC_STACK_OVERFLOW);
        return;
    }
    cpu_write32(cpu, cpu->sp, value);
}
static inline uint32_t pop(emu_cpu *cpu) {
    if (cpu->sp >= STACK_TOP) {
        enter_exception(cpu, IVT_EXC_STACK_UNDERFLOW);
        return 0;
    }

    uint32_t value = cpu_read32(cpu, cpu->sp);
    cpu->sp += 4;
    return value;
}

static inline void set_ip(emu_cpu *cpu, uint32_t addr) {
    cpu->ip = addr;
    cpu->cycles++;
}

static void enter_interrupt(emu_cpu *cpu, bool is_nmi, uint32_t base, uint32_t n) {
    push(cpu, cpu->ip);
    push(cpu, cpu->flags);

    uint32_t addr = base + n * SIZE_WORD;
    if (addr >= IVT_ADDR + IVT_SIZE) {
        enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION);
        return;
    }

    uint32_t handler = cpu_read32(cpu, addr);
    if (!is_nmi) set_flag(cpu, FLAG_INTERRUPT, true);
    set_ip(cpu, handler);
}

static void poll_interrupts(emu_cpu *cpu) {
    if (cpu->nmi_pending) {
        cpu->nmi_pending = false;
        enter_interrupt(cpu, true, IVT_NMI_ADDR, 0);
        return;
    }

    if (get_flag(cpu, FLAG_INTERRUPT)) return;

    uint8_t pending = cpu_read8(cpu, ICU_IRQ_PENDING_ADDR);
    uint8_t enabled = cpu_read8(cpu, ICU_IRQ_ENABLE_ADDR);
    uint8_t active = pending & enabled;
    if (!active) return;
    for (int i = 0; i < IVT_IRQ_COUNT; i++) {
        int bit = (1 << i);
        if (active & bit) {
#ifdef DEBUG
            printf("[DEBUG][CPU] raised irq %d\n", i);
#endif
            cpu_write8(cpu, ICU_IRQ_PENDING_ADDR, pending & ~bit);
            enter_interrupt(cpu, false, IVT_IRQ_BASE_ADDR, i);
            break;
        }
    }
}

emulator emu_init(uint8_t *rom, uint8_t *disk, bool debug_mode) {
    emulator emu = {
        .dma = {.bus = &emu.bus, .icu = &emu.icu},
        .vpu = {.frame_ptr = VPU_BUFFER0_ADDR, .source = emu.vpu.buffers[1], .bus = &emu.bus},
        .fdc = {.disk_data = disk, .icu = &emu.icu, .dma = &emu.dma},
        .bus = {
            .ram = my_malloc(RAM_SIZE), .rom = rom,
            .devices = {
                [DEVICE_ICU] = (bus_device){.device = &emu.icu, .access = &icu_access},
                [DEVICE_DMA] = (bus_device){.device = &emu.dma, .access = &dma_access},
                [DEVICE_FDC] = (bus_device){.device = &emu.fdc, .access = &fdc_access},
                [DEVICE_VPU] = (bus_device){.device = &emu.vpu, .access = &vpu_access}
            }
        },
        .cpu = {
            .bus = &emu.bus, .debug_mode = debug_mode,
            .ip = 0, .flags = 1 << FLAG_INTERRUPT,
            .nmi_pending = false,
        }
    };
    return emu;
}

void cpu_step(emu_cpu *cpu) {
    poll_interrupts(cpu);
    uint8_t instr = fetch8(cpu);

    uint8_t block = (instr >> 4) & 0b1111; // 4 bit
    uint8_t arg1 = (instr >> 2) & 0b11;
    uint8_t arg2 = instr & 0b11;

    switch (block) {
    case BLOCK_SYSTEM:
        switch (arg1) {
        case 0:
            switch (arg2) {
            case 0: break; // NOP
            case 1: { // HALT
                set_flag(cpu, FLAG_HALTED, true);
                break;
            }
            case 2: push(cpu, cpu->flags); break; // PUSHF
            case 3: cpu->flags = pop(cpu); break;  // POPF
            }
            break;
        case 1:
            switch (arg2) {
            case 0: set_flag(cpu, FLAG_CARRY, true);      break; // STC
            case 1: set_flag(cpu, FLAG_CARRY, false);     break; // CLC
            case 2: set_flag(cpu, FLAG_INTERRUPT, true);  break; // STI
            case 3: set_flag(cpu, FLAG_INTERRUPT, false); break; // CLI
            }
            break;
        case 2: {
            uint32_t value = (int32_t)(int8_t)fetch8(cpu);
            switch (arg2) {
            case 0: op_add(cpu, value); break;
            case 1: op_adc(cpu, value); break;
            case 2: op_sub(cpu, value); break;
            case 3: op_sbc(cpu, value); break;
            }
            break;
        }
        case 3:
            if (arg2 == 0) {
                // INT imm8
                enter_interrupt(cpu, false, IVT_INT_START, fetch8(cpu));
                break;
            }
            enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION);
            break;
        }
        break;
    case BLOCK_FLOW:
        switch (arg2) {
        case 0:
            switch (arg1) {
            case 0: { // JMP imm32
                uint32_t addr = fetch32(cpu);
                set_ip(cpu, addr);
                break;
            }
            case 1: { // CALL imm32
                uint32_t addr = fetch32(cpu);
                push(cpu, cpu->ip);
                set_ip(cpu, addr);
                break;
            }
            case 2: set_ip(cpu, pop(cpu)); break; // RTS
            case 3: // RTI
                cpu->flags = pop(cpu);
                set_ip(cpu, pop(cpu));
                cpu->in_exception = false;
                break;
            }
            break;
        case 1:
            set_ip(cpu, cpu->registers[arg1]); // JMP reg
            break;
        case 2: { // Jcond imm32
            uint32_t addr = fetch32(cpu);
            if (get_flag(cpu, arg1)) set_ip(cpu, addr);
            break;
        }
        case 3: { // Jncond imm32
            uint32_t addr = fetch32(cpu);
            if (!get_flag(cpu, arg1)) set_ip(cpu, addr);
            break;
        }
        }
        break;
    case BLOCK_MOVE:
        cpu->registers[arg1] = cpu->registers[arg2];
        break;
    case BLOCK_LOAD_IMM: { // MOV Rn, imm
        uint32_t value = 0;
        switch (arg2) {
        case 0: value = (int32_t)(int8_t)fetch8(cpu);  break;
        case 1: value = (int32_t)(int16_t)fetch16(cpu); break;
        case 2: value = fetch32(cpu); break;
        case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
        }
        cpu->registers[arg1] = value;
        break;
    }
    case BLOCK_LOAD_IDX: { // MOV Rn, [I + disp8]
        uint32_t addr = cpu->registers[REG_I] + fetch8(cpu);
        uint32_t value = 0;
        switch (arg2) {
        case 0: value = (int32_t)(int8_t)cpu_read8(cpu, addr);  break;
        case 1: value = (int32_t)(int16_t)cpu_read16(cpu, addr); break;
        case 2: value = cpu_read32(cpu, addr); break;
        case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
        }
        cpu->registers[arg1] = value;
        break;
    }
    case BLOCK_STORE_IDX: { // MOV [I + disp8], Rn
        uint32_t addr = cpu->registers[REG_I] + fetch8(cpu);
        switch (arg2) {
        case 0: cpu_write8(cpu, addr, cpu->registers[arg1]);  break;
        case 1: cpu_write16(cpu, addr, cpu->registers[arg1]); break;
        case 2: cpu_write32(cpu, addr, cpu->registers[arg1]); break;
        case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
        }
        break;
    }
    case BLOCK_LOAD_IND: { // MOV Rn, [I]
        uint32_t addr = cpu->registers[REG_I];
        uint32_t value = 0;
        switch (arg2) {
        case 0: value = (int32_t)(int8_t)cpu_read8(cpu, addr);  break;
        case 1: value = (int32_t)(int16_t)cpu_read16(cpu, addr); break;
        case 2: value = cpu_read32(cpu, addr); break;
        case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
        }
        cpu->registers[arg1] = value; // Register indirect load
        break;
    }
    case BLOCK_STORE_IND: { // MOV [I], Rn
        uint32_t addr = cpu->registers[REG_I];
        switch (arg1) {
        case 0: cpu_write8(cpu, addr, cpu->registers[arg2]);  break;
        case 1: cpu_write16(cpu, addr, cpu->registers[arg2]); break;
        case 2: cpu_write32(cpu, addr, cpu->registers[arg2]); break;
        case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
        }
        break;
    }
    case BLOCK_ARITH: {
        uint32_t value = cpu->registers[arg2];
        switch (arg1) {
        case 0: op_add(cpu, value); break;
        case 1: op_adc(cpu, value); break;
        case 2: op_sub(cpu, value); break;
        case 3: op_sbc(cpu, value); break;
        }
        break;
    }
    case BLOCK_MULDIV: {
        uint32_t value = cpu->registers[arg2];
        switch (arg1) {
        case 0: op_mul(cpu, value);  break;
        case 1: op_imul(cpu, value); break;
        case 2: op_div(cpu, value);  break;
        case 3: op_idiv(cpu, value); break;
        }
        break;
    }
    case BLOCK_LOGIC:
        switch (arg1) {
        case 0: op_and(cpu, cpu->registers[arg2]); break;
        case 1: op_or(cpu,  cpu->registers[arg2]); break;
        case 2: op_xor(cpu, cpu->registers[arg2]); break;
        case 3: {
            uint32_t value = fetch8(cpu);
            switch (arg2) {
            case 0: op_and(cpu, value); break;
            case 1: op_or(cpu, value);  break;
            case 2: op_xor(cpu, value); break;
            case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
            }
            break;
        }
        }
        break;
    case BLOCK_SHIFT: {
        uint32_t value = cpu->registers[arg2];
        switch (arg1) {
        case 0: op_shl(cpu, value); break;
        case 1: op_shr(cpu, value); break;
        case 2: op_rol(cpu, value); break;
        case 3: op_ror(cpu, value); break;
        }
        break;
    }
    case BLOCK_BIT:
        switch (arg1) {
        case 0: { // BIT Rn, imm8
            uint8_t bit = fetch8(cpu) & 31;
            uint32_t mask = 1 << bit;
            set_flag(cpu, FLAG_ZERO, (cpu->registers[arg2] & mask) == 0);
            break;
        }
        case 1: { // SET Rn, imm8
            uint8_t bit = fetch8(cpu) & 31;
            cpu->registers[arg2] |= (1 << bit);
            break;
        }
        case 2: { // RES Rn, imm8
            uint8_t bit = fetch8(cpu) & 31;
            cpu->registers[arg2] &= ~(1 << bit);
            break;
        }
        case 3: {
            // Shift/Rotate imm8
            uint32_t value = fetch8(cpu);
            switch (arg2) {
            case 0: op_shl(cpu, value); break;
            case 1: op_shr(cpu, value); break;
            case 2: op_rol(cpu, value); break;
            case 3: op_ror(cpu, value); break;
            }
            break;
        }
        }
        break;
    case BLOCK_UNARY:
        switch (arg1) {
        case 0: op_inc(cpu, &cpu->registers[arg2]); break;
        case 1: op_dec(cpu, &cpu->registers[arg2]); break;
        case 2: op_not(cpu, arg2); break;
        case 3: op_neg(cpu, arg2); break;
        }
        break;
    case BLOCK_STACK: {
        switch (arg1) {
        case 0: { // MOV SP, Rn
            cpu->sp = cpu->registers[arg2];
            cpu->cycles++;
            break;
        }
        case 1: { // MOV Rn, SP
            cpu->registers[arg2] = cpu->sp;
            cpu->cycles++;
            break;
        }
        case 2: push(cpu, cpu->registers[arg2]); break; // PUSH Rn
        case 3: cpu->registers[arg2] = pop(cpu); break; // POP Rn
        }
        break;
    }
    case BLOCK_CMP:
        switch (arg1) {
        case 0:
            op_cmp(cpu, REG_A, cpu->registers[arg2]);
            break;
        case 1:
            op_cmp(cpu, REG_I, cpu->registers[arg2]);
            break;
        case 2: { // CMP A, imm
            uint32_t value = 0;
            switch (arg2) {
            case 0: value = (int32_t)(int8_t)fetch8(cpu);  break;
            case 1: value = (int32_t)(int16_t)fetch16(cpu); break;
            case 2: value = fetch32(cpu); break;
            case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
            }
            op_cmp(cpu, REG_A, value);
            break;
        }
        case 3: { // CMP I, imm
            uint32_t value = 0;
            switch (arg2) {
            case 0: value = (int32_t)(int8_t)fetch8(cpu);  break;
            case 1: value = (int32_t)(int16_t)fetch16(cpu); break;
            case 2: value = fetch32(cpu); break;
            case 3: enter_exception(cpu, IVT_EXC_ILLEGAL_INSTRUCTION); break;
            }
            op_cmp(cpu, REG_I, value);
            break;
        }
        }
        break;
    }

    if (!cpu->debug_mode) return;
    printf("[DEBUG][CPU] ip=%u sp=%u | %-10s %d %d | ",
            cpu->ip, cpu->sp, opcode_block_str[block], arg1, arg2);
    for (int i = 0; i < REG_COUNT; i++) {
        printf("r%u=%u ", i, cpu->registers[i]);
    }
    putchar('\n');
}