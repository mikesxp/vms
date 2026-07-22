#include "backend.h"
#include "emulator.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *mnemonic_str[MN_COUNT] = {
    [MN_NOP]  = "nop",
    [MN_HLT]  = "hlt",
    [MN_RET]  = "ret",
    [MN_IRET]  = "iret",
    [MN_INT]  = "int",
    [MN_STC]  = "stc",
    [MN_CLC]  = "clc",
    [MN_STI]  = "sti",
    [MN_CLI]  = "cli",
    [MN_JMP]  = "jmp",
    [MN_JZ]   = "jz",
    [MN_JNZ]  = "jnz",
    [MN_JC]   = "jc",
    [MN_JNC]  = "jnc",
    [MN_JO]   = "jo",
    [MN_JNO]  = "jno",
    [MN_JS]   = "js",
    [MN_JNS]  = "jns",
    [MN_CALL] = "call",
    [MN_MOV]   = "mov",
    [MN_ADD]  = "add",
    [MN_ADC]  = "adc",
    [MN_SUB]  = "sub",
    [MN_SBC]  = "sbc",
    [MN_MUL]  = "mul",
    [MN_DIV]  = "div",
    [MN_AND]  = "and",
    [MN_OR]   = "or",
    [MN_XOR]  = "xor",
    [MN_SHL]  = "shl",
    [MN_SHR]  = "shr",
    [MN_ROL]  = "rol",
    [MN_ROR]  = "ror",
    [MN_BIT]  = "bit",
    [MN_SET]  = "set",
    [MN_RES]  = "res",
    [MN_INC]  = "inc",
    [MN_DEC]  = "dec",
    [MN_NOT]  = "not",
    [MN_NEG]  = "neg",
    [MN_CMP]  = "cmp",
    [MN_PUSH] = "push",
    [MN_POP]  = "pop",
    [MN_ERR]  = "err",
};

// Used when a register represents a value to be stored (example: store at 0x200 the first 2 bytes of A == mov [0x200], (hword)a)
#define REG_WITH_SIZE_VALUE(r, sz) (value){.kind = VAL_REG, .reg.index = r, .reg.sign = SIGN_NONE, .is_addr = false, .size = sz}
#define REG_WITH_SIGN_VALUE(r, isaddr, s) (value){.kind = VAL_REG, .reg.index = r, .reg.sign = s, .is_addr = isaddr, .size = SIZE_NONE}
#define REG_VALUE(r, isaddr) REG_WITH_SIGN_VALUE(r, isaddr, SIGN_NONE)
#define IMM_VALUE(sz)  (value){.kind = VAL_EXPR, .is_addr = false, .size = sz}
#define IMM_ADDR_VALUE (value){.kind = VAL_EXPR, .is_addr = true, .size = SIZE_WORD}
#define SP_VALUE (value){.kind = VAL_SP}

#define I_IMM8_ADDR_VALUE (value) {\
    .kind = VAL_REG, .reg = {SIGN_NONE, REG_I}, .size = SIZE_NONE, .is_addr = true,\
    .op_type = OPERATOR_ADD, .operand = &IMM_VALUE(SIZE_BYTE),\
}
#define SIMPLE_VALUE(k) (value){.kind = k, .is_addr = false, .size = SIZE_NONE}

const value none_value = SIMPLE_VALUE(VAL_NONE);

const instruction_form instruction_forms[] = {
    { MN_NOP,  BLOCK_SYSTEM, 0, 0, none_value,  none_value },
    { MN_HLT,  BLOCK_SYSTEM, 0, 1, none_value,  none_value },

    { MN_STC,  BLOCK_SYSTEM, 1, 0, none_value,  none_value },
    { MN_CLC,  BLOCK_SYSTEM, 1, 1, none_value,  none_value },
    { MN_STI,  BLOCK_SYSTEM, 1, 2, none_value,  none_value },
    { MN_CLI,  BLOCK_SYSTEM, 1, 3, none_value,  none_value },

    { MN_INT,  BLOCK_SYSTEM, 3, 0, IMM_VALUE(SIZE_BYTE),    none_value },

    { MN_CALL, BLOCK_FLOW,   1, 0, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_RET,  BLOCK_FLOW,   2, 0, none_value,  none_value },
    { MN_IRET,  BLOCK_FLOW,   3, 0, none_value,  none_value },

    { MN_JMP,  BLOCK_FLOW,   0, 0, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JMP,  BLOCK_FLOW,   0, 1, REG_VALUE(REG_A, false), none_value },
    { MN_JMP,  BLOCK_FLOW,   1, 1, REG_VALUE(REG_X, false), none_value },
    { MN_JMP,  BLOCK_FLOW,   2, 1, REG_VALUE(REG_Y, false), none_value },
    { MN_JMP,  BLOCK_FLOW,   3, 1, REG_VALUE(REG_I, false), none_value },

    { MN_JZ,   BLOCK_FLOW,   0, 2, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JS,   BLOCK_FLOW,   1, 2, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JO,   BLOCK_FLOW,   2, 2, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JC,   BLOCK_FLOW,   3, 2, IMM_VALUE(SIZE_WORD),    none_value },

    { MN_JNZ,  BLOCK_FLOW,   0, 3, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JNS,  BLOCK_FLOW,   1, 3, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JNO,  BLOCK_FLOW,   2, 3, IMM_VALUE(SIZE_WORD),    none_value },
    { MN_JNC,  BLOCK_FLOW,   3, 3, IMM_VALUE(SIZE_WORD),    none_value },

    { MN_MOV,   BLOCK_MOVE,   0, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_MOV,   BLOCK_MOVE,   0, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_MOV,   BLOCK_MOVE,   0, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_MOV,   BLOCK_MOVE,   0, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_MOV,   BLOCK_MOVE,   1, 0, REG_VALUE(REG_X, false), REG_VALUE(REG_A, false) },
    { MN_MOV,   BLOCK_MOVE,   1, 1, REG_VALUE(REG_X, false), REG_VALUE(REG_X, false) },
    { MN_MOV,   BLOCK_MOVE,   1, 2, REG_VALUE(REG_X, false), REG_VALUE(REG_Y, false) },
    { MN_MOV,   BLOCK_MOVE,   1, 3, REG_VALUE(REG_X, false), REG_VALUE(REG_I, false) },
    { MN_MOV,   BLOCK_MOVE,   2, 0, REG_VALUE(REG_Y, false), REG_VALUE(REG_A, false) },
    { MN_MOV,   BLOCK_MOVE,   2, 1, REG_VALUE(REG_Y, false), REG_VALUE(REG_X, false) },
    { MN_MOV,   BLOCK_MOVE,   2, 2, REG_VALUE(REG_Y, false), REG_VALUE(REG_Y, false) },
    { MN_MOV,   BLOCK_MOVE,   2, 3, REG_VALUE(REG_Y, false), REG_VALUE(REG_I, false) },
    { MN_MOV,   BLOCK_MOVE,   3, 0, REG_VALUE(REG_I, false), REG_VALUE(REG_A, false) },
    { MN_MOV,   BLOCK_MOVE,   3, 1, REG_VALUE(REG_I, false), REG_VALUE(REG_X, false) },
    { MN_MOV,   BLOCK_MOVE,   3, 2, REG_VALUE(REG_I, false), REG_VALUE(REG_Y, false) },
    { MN_MOV,   BLOCK_MOVE,   3, 3, REG_VALUE(REG_I, false), REG_VALUE(REG_I, false) },

    { MN_MOV,   BLOCK_LOAD_IMM, 0, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 0, 1, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_HWORD) },
    { MN_MOV,   BLOCK_LOAD_IMM, 0, 2, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_WORD)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 1, 0, REG_VALUE(REG_X, false), IMM_VALUE(SIZE_BYTE)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 1, 1, REG_VALUE(REG_X, false), IMM_VALUE(SIZE_HWORD) },
    { MN_MOV,   BLOCK_LOAD_IMM, 1, 2, REG_VALUE(REG_X, false), IMM_VALUE(SIZE_WORD)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 2, 0, REG_VALUE(REG_Y, false), IMM_VALUE(SIZE_BYTE)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 2, 1, REG_VALUE(REG_Y, false), IMM_VALUE(SIZE_HWORD) },
    { MN_MOV,   BLOCK_LOAD_IMM, 2, 2, REG_VALUE(REG_Y, false), IMM_VALUE(SIZE_WORD)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 3, 0, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_BYTE)  },
    { MN_MOV,   BLOCK_LOAD_IMM, 3, 1, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_HWORD) },
    { MN_MOV,   BLOCK_LOAD_IMM, 3, 2, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_WORD)  },

    { MN_MOV,   BLOCK_LOAD_IDX, 0, 0, REG_WITH_SIZE_VALUE(REG_A, SIZE_BYTE),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 0, 1, REG_WITH_SIZE_VALUE(REG_A, SIZE_HWORD), I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 0, 2, REG_WITH_SIZE_VALUE(REG_A, SIZE_WORD),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 1, 0, REG_WITH_SIZE_VALUE(REG_X, SIZE_BYTE),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 1, 1, REG_WITH_SIZE_VALUE(REG_X, SIZE_HWORD), I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 1, 2, REG_WITH_SIZE_VALUE(REG_X, SIZE_WORD),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 2, 0, REG_WITH_SIZE_VALUE(REG_Y, SIZE_BYTE),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 2, 1, REG_WITH_SIZE_VALUE(REG_Y, SIZE_HWORD), I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 2, 2, REG_WITH_SIZE_VALUE(REG_Y, SIZE_WORD),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 3, 0, REG_WITH_SIZE_VALUE(REG_I, SIZE_BYTE),  I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 3, 1, REG_WITH_SIZE_VALUE(REG_I, SIZE_HWORD), I_IMM8_ADDR_VALUE },
    { MN_MOV,   BLOCK_LOAD_IDX, 3, 2, REG_WITH_SIZE_VALUE(REG_I, SIZE_WORD),  I_IMM8_ADDR_VALUE },

    { MN_MOV,   BLOCK_STORE_IDX, 0, 0, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_A, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IDX, 0, 1, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_A, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IDX, 0, 2, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_A, SIZE_WORD)  },
    { MN_MOV,   BLOCK_STORE_IDX, 1, 0, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_X, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IDX, 1, 1, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_X, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IDX, 1, 2, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_X, SIZE_WORD)  },
    { MN_MOV,   BLOCK_STORE_IDX, 2, 0, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_Y, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IDX, 2, 1, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_Y, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IDX, 2, 2, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_Y, SIZE_WORD)  },
    { MN_MOV,   BLOCK_STORE_IDX, 3, 0, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_I, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IDX, 3, 1, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_I, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IDX, 3, 2, I_IMM8_ADDR_VALUE, REG_WITH_SIZE_VALUE(REG_I, SIZE_WORD)  },

    { MN_MOV,   BLOCK_LOAD_IND, 0, 0, REG_WITH_SIZE_VALUE(REG_A, SIZE_BYTE),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 0, 1, REG_WITH_SIZE_VALUE(REG_A, SIZE_HWORD), REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 0, 2, REG_WITH_SIZE_VALUE(REG_A, SIZE_WORD),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 1, 0, REG_WITH_SIZE_VALUE(REG_X, SIZE_BYTE),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 1, 1, REG_WITH_SIZE_VALUE(REG_X, SIZE_HWORD), REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 1, 2, REG_WITH_SIZE_VALUE(REG_X, SIZE_WORD),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 2, 0, REG_WITH_SIZE_VALUE(REG_Y, SIZE_BYTE),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 2, 1, REG_WITH_SIZE_VALUE(REG_Y, SIZE_HWORD), REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 2, 2, REG_WITH_SIZE_VALUE(REG_Y, SIZE_WORD),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 3, 0, REG_WITH_SIZE_VALUE(REG_I, SIZE_BYTE),  REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 3, 1, REG_WITH_SIZE_VALUE(REG_I, SIZE_HWORD), REG_VALUE(REG_I, true) },
    { MN_MOV,   BLOCK_LOAD_IND, 3, 2, REG_WITH_SIZE_VALUE(REG_I, SIZE_WORD),  REG_VALUE(REG_I, true) },

    { MN_MOV,   BLOCK_STORE_IND, 0, 0, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_A, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IND, 0, 1, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_X, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IND, 0, 2, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_Y, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IND, 0, 3, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_I, SIZE_BYTE)  },
    { MN_MOV,   BLOCK_STORE_IND, 1, 0, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_A, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IND, 1, 1, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_X, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IND, 1, 2, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_Y, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IND, 1, 3, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_I, SIZE_HWORD) },
    { MN_MOV,   BLOCK_STORE_IND, 2, 0, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_A, SIZE_WORD)  },
    { MN_MOV,   BLOCK_STORE_IND, 2, 1, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_X, SIZE_WORD)  },
    { MN_MOV,   BLOCK_STORE_IND, 2, 2, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_Y, SIZE_WORD)  },
    { MN_MOV,   BLOCK_STORE_IND, 2, 3, REG_VALUE(REG_I, true), REG_WITH_SIZE_VALUE(REG_I, SIZE_WORD)  },

    { MN_MOV,   BLOCK_STACK,  0, 0, SP_VALUE, REG_VALUE(REG_A, false) }, // MOV SP, Rn
    { MN_MOV,   BLOCK_STACK,  0, 1, SP_VALUE, REG_VALUE(REG_X, false) },
    { MN_MOV,   BLOCK_STACK,  0, 2, SP_VALUE, REG_VALUE(REG_Y, false) },
    { MN_MOV,   BLOCK_STACK,  0, 3, SP_VALUE, REG_VALUE(REG_I, false) },
    { MN_MOV,   BLOCK_STACK,  1, 0, REG_VALUE(REG_A, false), SP_VALUE }, // MOV Rn, SP
    { MN_MOV,   BLOCK_STACK,  1, 1, REG_VALUE(REG_X, false), SP_VALUE },
    { MN_MOV,   BLOCK_STACK,  1, 2, REG_VALUE(REG_Y, false), SP_VALUE },
    { MN_MOV,   BLOCK_STACK,  1, 3, REG_VALUE(REG_I, false), SP_VALUE },

    { MN_PUSH,  BLOCK_STACK,  2, 0, REG_VALUE(REG_A, false) }, // PUSH Rn
    { MN_PUSH,  BLOCK_STACK,  2, 1, REG_VALUE(REG_X, false) },
    { MN_PUSH,  BLOCK_STACK,  2, 2, REG_VALUE(REG_Y, false) },
    { MN_PUSH,  BLOCK_STACK,  2, 3, REG_VALUE(REG_I, false) },
    { MN_PUSH,  BLOCK_SYSTEM, 0, 2, SIMPLE_VALUE(VAL_FLAGS), none_value }, // PUSHF
    { MN_POP,   BLOCK_STACK,  3, 0, REG_VALUE(REG_A, false) }, // POP Rn
    { MN_POP,   BLOCK_STACK,  3, 1, REG_VALUE(REG_X, false) },
    { MN_POP,   BLOCK_STACK,  3, 2, REG_VALUE(REG_Y, false) },
    { MN_POP,   BLOCK_STACK,  3, 3, REG_VALUE(REG_I, false) },
    { MN_POP,   BLOCK_SYSTEM, 0, 3, SIMPLE_VALUE(VAL_FLAGS), none_value }, // POPF

    { MN_ADD,  BLOCK_ARITH,  0, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_ADD,  BLOCK_ARITH,  0, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_ADD,  BLOCK_ARITH,  0, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_ADD,  BLOCK_ARITH,  0, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_ADD,  BLOCK_SYSTEM,  2, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_ADC,  BLOCK_ARITH,  1, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_ADC,  BLOCK_ARITH,  1, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_ADC,  BLOCK_ARITH,  1, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_ADC,  BLOCK_ARITH,  1, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_ADC,  BLOCK_SYSTEM,  2, 1, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_SUB,  BLOCK_ARITH,  2, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_SUB,  BLOCK_ARITH,  2, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_SUB,  BLOCK_ARITH,  2, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_SUB,  BLOCK_ARITH,  2, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_SUB,  BLOCK_SYSTEM,  2, 2, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },
    { MN_SBC,  BLOCK_ARITH,  3, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_SBC,  BLOCK_ARITH,  3, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_SBC,  BLOCK_ARITH,  3, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_SBC,  BLOCK_ARITH,  3, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_SBC,  BLOCK_SYSTEM,  2, 3, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_MUL,  BLOCK_MULDIV, 0, 0, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 0, 1, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_X, false, SIGN_UNSIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 0, 2, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_Y, false, SIGN_UNSIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 0, 3, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_I, false, SIGN_UNSIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 1, 0, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 1, 1, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_X, false, SIGN_SIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 1, 2, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_Y, false, SIGN_SIGNED) },
    { MN_MUL,  BLOCK_MULDIV, 1, 3, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_I, false, SIGN_SIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 2, 0, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 2, 1, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_X, false, SIGN_UNSIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 2, 2, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_Y, false, SIGN_UNSIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 2, 3, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_UNSIGNED), REG_WITH_SIGN_VALUE(REG_I, false, SIGN_UNSIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 3, 0, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 3, 1, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_X, false, SIGN_SIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 3, 2, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_Y, false, SIGN_SIGNED) },
    { MN_DIV,  BLOCK_MULDIV, 3, 3, REG_WITH_SIGN_VALUE(REG_A, false, SIGN_SIGNED),   REG_WITH_SIGN_VALUE(REG_I, false, SIGN_SIGNED) },

    { MN_AND, BLOCK_LOGIC,  0, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_AND, BLOCK_LOGIC,  0, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_AND, BLOCK_LOGIC,  0, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_AND, BLOCK_LOGIC,  0, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_AND, BLOCK_LOGIC,  3, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_OR,  BLOCK_LOGIC,  1, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_OR,  BLOCK_LOGIC,  1, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_OR,  BLOCK_LOGIC,  1, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_OR,  BLOCK_LOGIC,  1, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_OR,  BLOCK_LOGIC,  3, 1, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_XOR, BLOCK_LOGIC,  2, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_XOR, BLOCK_LOGIC,  2, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_XOR, BLOCK_LOGIC,  2, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_XOR, BLOCK_LOGIC,  2, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_XOR, BLOCK_LOGIC,  3, 2, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_ROL,  BLOCK_SHIFT,  2, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_ROL,  BLOCK_SHIFT,  2, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_ROL,  BLOCK_SHIFT,  2, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_ROL,  BLOCK_SHIFT,  2, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_ROL,  BLOCK_BIT,    3, 2, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_ROR,  BLOCK_SHIFT,  3, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_ROR,  BLOCK_SHIFT,  3, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_ROR,  BLOCK_SHIFT,  3, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_ROR,  BLOCK_SHIFT,  3, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_ROR,  BLOCK_BIT,    3, 3, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_BIT,  BLOCK_BIT,    0, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) }, // BIT A, imm8
    { MN_BIT,  BLOCK_BIT,    0, 1, REG_VALUE(REG_X, false), IMM_VALUE(SIZE_BYTE) },
    { MN_BIT,  BLOCK_BIT,    0, 2, REG_VALUE(REG_Y, false), IMM_VALUE(SIZE_BYTE) },
    { MN_BIT,  BLOCK_BIT,    0, 3, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_BYTE) },
    { MN_SET,  BLOCK_BIT,    1, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) }, // SET A, imm8
    { MN_SET,  BLOCK_BIT,    1, 1, REG_VALUE(REG_X, false), IMM_VALUE(SIZE_BYTE) },
    { MN_SET,  BLOCK_BIT,    1, 2, REG_VALUE(REG_Y, false), IMM_VALUE(SIZE_BYTE) },
    { MN_SET,  BLOCK_BIT,    1, 3, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_BYTE) },
    { MN_RES,  BLOCK_BIT,    2, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) }, // RES A, imm8
    { MN_RES,  BLOCK_BIT,    2, 1, REG_VALUE(REG_X, false), IMM_VALUE(SIZE_BYTE) },
    { MN_RES,  BLOCK_BIT,    2, 2, REG_VALUE(REG_Y, false), IMM_VALUE(SIZE_BYTE) },
    { MN_RES,  BLOCK_BIT,    2, 3, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_BYTE) },

    { MN_SHL,  BLOCK_SHIFT,  0, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_SHL,  BLOCK_SHIFT,  0, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_SHL,  BLOCK_SHIFT,  0, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_SHL,  BLOCK_SHIFT,  0, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_SHL,  BLOCK_BIT,    3, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },

    { MN_SHR,  BLOCK_BIT,    3, 1, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE) },
    { MN_SHR,  BLOCK_SHIFT,  1, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) },
    { MN_SHR,  BLOCK_SHIFT,  1, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_SHR,  BLOCK_SHIFT,  1, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_SHR,  BLOCK_SHIFT,  1, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },

    { MN_INC,  BLOCK_UNARY,  0, 0, REG_VALUE(REG_A, false), none_value },
    { MN_INC,  BLOCK_UNARY,  0, 1, REG_VALUE(REG_X, false), none_value },
    { MN_INC,  BLOCK_UNARY,  0, 2, REG_VALUE(REG_Y, false), none_value },
    { MN_INC,  BLOCK_UNARY,  0, 3, REG_VALUE(REG_I, false), none_value },
    { MN_DEC,  BLOCK_UNARY,  1, 0, REG_VALUE(REG_A, false), none_value },
    { MN_DEC,  BLOCK_UNARY,  1, 1, REG_VALUE(REG_X, false), none_value },
    { MN_DEC,  BLOCK_UNARY,  1, 2, REG_VALUE(REG_Y, false), none_value },
    { MN_DEC,  BLOCK_UNARY,  1, 3, REG_VALUE(REG_I, false), none_value },
    { MN_NOT,  BLOCK_UNARY,  2, 0, REG_VALUE(REG_A, false), none_value },
    { MN_NOT,  BLOCK_UNARY,  2, 1, REG_VALUE(REG_X, false), none_value },
    { MN_NOT,  BLOCK_UNARY,  2, 2, REG_VALUE(REG_Y, false), none_value },
    { MN_NOT,  BLOCK_UNARY,  2, 3, REG_VALUE(REG_I, false), none_value },
    { MN_NEG,  BLOCK_UNARY,  3, 0, REG_VALUE(REG_A, false), none_value },
    { MN_NEG,  BLOCK_UNARY,  3, 1, REG_VALUE(REG_X, false), none_value },
    { MN_NEG,  BLOCK_UNARY,  3, 2, REG_VALUE(REG_Y, false), none_value },
    { MN_NEG,  BLOCK_UNARY,  3, 3, REG_VALUE(REG_I, false), none_value },

    { MN_CMP,  BLOCK_CMP,    0, 0, REG_VALUE(REG_A, false), REG_VALUE(REG_A, false) }, // CMP A, Rn
    { MN_CMP,  BLOCK_CMP,    0, 1, REG_VALUE(REG_A, false), REG_VALUE(REG_X, false) },
    { MN_CMP,  BLOCK_CMP,    0, 2, REG_VALUE(REG_A, false), REG_VALUE(REG_Y, false) },
    { MN_CMP,  BLOCK_CMP,    0, 3, REG_VALUE(REG_A, false), REG_VALUE(REG_I, false) },
    { MN_CMP,  BLOCK_CMP,    1, 0, REG_VALUE(REG_I, false), REG_VALUE(REG_A, false) }, // CMP I, Rn
    { MN_CMP,  BLOCK_CMP,    1, 1, REG_VALUE(REG_I, false), REG_VALUE(REG_X, false) },
    { MN_CMP,  BLOCK_CMP,    1, 2, REG_VALUE(REG_I, false), REG_VALUE(REG_Y, false) },
    { MN_CMP,  BLOCK_CMP,    1, 3, REG_VALUE(REG_I, false), REG_VALUE(REG_I, false) },
    { MN_CMP,  BLOCK_CMP,    2, 0, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_BYTE)  }, // CMP A, imm
    { MN_CMP,  BLOCK_CMP,    2, 1, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_HWORD) },
    { MN_CMP,  BLOCK_CMP,    2, 2, REG_VALUE(REG_A, false), IMM_VALUE(SIZE_WORD)  },
    { MN_CMP,  BLOCK_CMP,    3, 0, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_BYTE)  }, // CMP I, imm
    { MN_CMP,  BLOCK_CMP,    3, 1, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_HWORD) },
    { MN_CMP,  BLOCK_CMP,    3, 2, REG_VALUE(REG_I, false), IMM_VALUE(SIZE_WORD)  },
};

const size_t instruction_forms_count = ARRAYLEN(instruction_forms);
void sprk32_emit(codegen_ctx *c, int64_t value, prim_size size) {
    sprk32_emitter *emitter = c->backend->emitter;
    uint32_t addr = c->current_ip - emitter->offset;
    if (addr + size >= emitter->out_size) {
        printf("error: storage limit exceeded (max is 0x%X)\n", emitter->out_size);
        c->stream->error_occured = true;
        return;
    }
    switch (size) {
    case SIZE_BYTE:  emitter->out[addr] = value; break;
    case SIZE_HWORD: write16(emitter->out, addr, value); break;
    case SIZE_NONE:
    case SIZE_WORD:  write32(emitter->out, addr, value); break;
    case SIZE_DWORD: write64(emitter->out, addr, value); break;
    }
    c->current_ip += size;
}

static void emit_instr(codegen_ctx *c, const mnemonic mnemonic, const value *value1, const value *value2, token *op_tok) {
    const instruction_form *f = NULL;

    sprk32_emitter *emitter = c->backend->emitter;
    mnemonic_range r = emitter->mnemonic_ranges[mnemonic];
    for (int i = r.start; i <= r.end; i++) {
        const instruction_form *currform = &instruction_forms[i];
        if (value_matches(value1, &currform->value1) &&
            value_matches(value2, &currform->value2)) {
            f = currform;
            break;
        }
    }
    if (!f) {
        ERROR_AT(op_tok, c->stream,
                "no matching instruction variant for mnemonic '%s'", mnemonic_str[mnemonic]);
        printf("variant found: ");
        print_values(stdout, value1, value2, false);
        printf("variants available:\n");
        for (int i = r.start; i <= r.end; i++) {
            const instruction_form *currform = &instruction_forms[i];
            if (currform->mn == mnemonic) {
                printf(" - ");
                print_values(stdout, &currform->value1, &currform->value2, false);
            }
        }
        return;
    }

    uint32_t ip = c->current_ip;
    uint8_t instr = 0;
    instr |= (f->block & 0b1111) << 4;
    instr |= (f->arg1 & 0b11) << 2;
    instr |= (f->arg2 & 0b11);
    sprk32_emit(c, instr, SIZE_BYTE);
    emit_value(c, value1, f->value1.size);
    emit_value(c, value2, f->value2.size);
    if (emitter->max_ip != 0 && c->current_ip >= emitter->max_ip) {
        ERROR_AT(op_tok, c->stream,
            "memory limit exceeded (max is 0x%X, use 'limit' with a layout field to set it)",
            emitter->max_ip);
    }
}
static void emit_add_sub(codegen_ctx *c) {
    value *lhs = &c->instr->bin.lhs;
    value *rhs = &c->instr->bin.rhs;
    token *tok = c->instr->bin.tok;
    eval_value(c, lhs);
    eval_value(c, rhs);
    bool is_add = c->instr->kind == INSTR_ADD;
    if (value_is_number(rhs, 1)) {
        mnemonic mn = MN_DEC;
        if (is_add) {
            ir_instr *next_instr = get_next_instr(c);
            // sp += 1
            // r0 = [sp]
            if (value_is_simple(lhs, VAL_SP) && next_instr->kind == INSTR_LOAD &&
                value_is_addr(&next_instr->bin.rhs, VAL_SP))
            {
                emit_instr(c, MN_POP, &next_instr->bin.lhs, &none_value, tok);
                c->instr_index++;
                return;
            }
            mn = MN_INC;
        }
        emit_instr(c, mn, lhs, &none_value, tok);
        return;
    }

    operator_type op_type = is_add ? OPERATOR_ADD : OPERATOR_SUB;
    if (rhs->op_type == op_type && rhs->operand->kind == VAL_FLAGS && 
        rhs->operand->op_type == OPERATOR_BIT_AND && rhs->operand->operand->kind == VAL_EXPR &&
        eval_expr(c, rhs->operand->operand->expr) == 1<<FLAG_CARRY)
    {
        value rhs1 = *rhs;
        rhs1.op_type = OPERATOR_NONE;
        rhs1.operand = NULL;
        emit_instr(c, is_add ? MN_ADC : MN_SBC, lhs, &rhs1, tok);
        return;
    }
    if (lhs->op_type != OPERATOR_NONE || rhs->op_type != OPERATOR_NONE) {
        ERROR_AT(tok, c->stream, 
                "+/- operations can only be used in the second operand with normal"
                " or '+/- flags & carry' form (example: r0 += r1 + flags & flag@carry)");
        return;
    }

    emit_instr(c, is_add ? MN_ADD : MN_SUB, lhs, rhs, tok);
}

static bool is_power_of_two(int n, int *shift) {
    if (n <= 0 || (n & (n - 1)) != 0) return false; // Is not negative and is a power of two
    *shift = 0;
    while (n > 1) {
        n >>= 1;
        (*shift)++;
    }
    return true;
}
static void emit_mul_div(codegen_ctx *c) {
    eval_value(c, &c->instr->bin.lhs);
    eval_value(c, &c->instr->bin.rhs);

    bool is_mul = c->instr->kind == INSTR_MUL;
    if (c->instr->bin.rhs.kind == VAL_EXPR) {
        int64_t *result = &c->instr->bin.rhs.expr->result;
        if (*result == 1) {
            report(c->instr->bin.tok, DIAGNOSTIC_WARNING, false, c->stream,
                "multiplying or dividing by 1 has no observable effect and will be optimized away");
            return;
        }
        if (is_mul && *result == 2) {
            emit_instr(c, MN_ADD, &c->instr->bin.lhs, &c->instr->bin.lhs, c->instr->bin.tok);
            return;
        }
        if (is_power_of_two(*result, (int*)result)) {
            c->instr->bin.rhs.size = SIZE_BYTE;
            emit_instr(c, is_mul ? MN_SHL : MN_SHR,
                &c->instr->bin.lhs, &c->instr->bin.rhs, c->instr->bin.tok);
            return;
        }
    }
    emit_instr(c, is_mul ? MN_MUL : MN_DIV,
        &c->instr->bin.lhs, &c->instr->bin.rhs, c->instr->bin.tok);
}

static inline void emit_binary(codegen_ctx *c, mnemonic mnemonic) {
    eval_value(c, &c->instr->bin.lhs);
    eval_value(c, &c->instr->bin.rhs);
    emit_instr(c, mnemonic, &c->instr->bin.lhs, &c->instr->bin.rhs, c->instr->bin.tok);
}

static inline void emit_branch(codegen_ctx *c, mnemonic mnemonic, value *addr) {
    eval_value(c, addr);
    emit_instr(c, mnemonic, addr, &none_value, NULL);
}
static int64_t emit_blank_jmp(codegen_ctx *c, mnemonic mnemonic) {
    expr_node expr = {.kind = EXPR_NUMBER, .number = 0};
    value v = {.kind = VAL_EXPR, .size = SIZE_WORD, .expr = &expr};
    emit_instr(c, mnemonic, &v, &none_value, NULL);
    return c->current_ip - v.size;
}
static void resolve_blank_jmps(codegen_ctx *c, int64_t *addrs, int count) {
    uint32_t label_addr = c->current_ip;
    for (int i = 0; i < count; i++) {
        // Emit current ip at the addrs[i] ip
        c->current_ip = addrs[i];
        sprk32_emit(c, label_addr, SIZE_WORD);
    }
    c->current_ip = label_addr;
}

static bool single_bit_position(uint64_t x, int64_t *pos) {
    if (x == 0 || (x & (x - 1)) != 0) return false;

    *pos = __builtin_ctzll(x);
    return true;
}
static void emit_and_or(codegen_ctx *c) {
    bool is_and = c->instr->kind == INSTR_AND;
    value *lhs_val = &c->instr->bin.lhs;
    value *rhs_val = &c->instr->bin.rhs;
    token *tok = c->instr->bin.tok;
    bool rhs_is_expr = value_is_simple(rhs_val, VAL_EXPR);

    if (value_is_simple(lhs_val, VAL_FLAGS) && rhs_is_expr) {
        int64_t result = eval_expr(c, rhs_val->expr);
        if (is_and) result = ~result;
        if (result == 1<<FLAG_INTERRUPT) {
            emit_instr(c, is_and ? MN_CLI : MN_STI, &none_value, &none_value, tok);
            return;
        }
        if (result == 1<<FLAG_CARRY) {
            emit_instr(c, is_and ? MN_CLC : MN_STC, &none_value, &none_value, tok);
            return;
        }
        ERROR_AT(tok, c->stream, "invalid flag index for flag %s", is_and ? "clear" : "set");
        return;
    }

    eval_value(c, lhs_val);
    eval_value(c, rhs_val);

    mnemonic mn = is_and ? MN_AND : MN_OR;
    if (rhs_val->kind == VAL_EXPR) {
        uint64_t rhs_result = rhs_val->expr->result;
        if (single_bit_position(rhs_result, &rhs_val->expr->result)) {
            rhs_val->size = SIZE_BYTE;
            mn = is_and ? MN_RES : MN_SET;
        }
    }
    emit_instr(c, mn, lhs_val, rhs_val, tok);
}
static bool updates_zero_flag(ir_instr *instr) {
    switch(instr->kind) {
    case INSTR_ADD: case INSTR_SUB: case INSTR_MUL: case INSTR_DIV: case INSTR_AND: 
    case INSTR_OR:  case INSTR_XOR: case INSTR_NOT: case INSTR_SHL: case INSTR_SHR: 
    case INSTR_ROL: case INSTR_ROR: case INSTR_CMP: return true;
    default: return false;
    }
}
void sprk32_gen_instr(codegen_ctx *c) {
    switch (c->instr->kind) {
    case INSTR_LIMIT: {
        sprk32_emitter *emitter = c->backend->emitter;
        emitter->offset = eval_expr(c, c->instr->limit.start);
        emitter->max_ip = emitter->offset + eval_expr(c, c->instr->limit.end);
        break;
    }
    case INSTR_RESERVE: {
        uint64_t size = eval_expr(c, c->instr->reserve.expr);
        c->current_ip += size;
        break;
    }
    case INSTR_EMIT: {
        for (int i = 0; i < c->instr->emit.values.count; i++) {
            expr_node *expr = vector_get(&c->instr->emit.values, i);
            int64_t result = eval_expr(c, expr);
            sprk32_emit(c, result, c->instr->emit.size);
        }
        break;
    }
    case INSTR_NOP:  emit_instr(c, MN_NOP, &none_value, &none_value, NULL); break;
    case INSTR_HALT: emit_instr(c, MN_HLT, &none_value, &none_value, NULL); break;
    case INSTR_CALL:
        if (c->instr->branch.type != BRANCH_NONE) {
            ERROR_AT(c->instr->branch.tok, c->stream,
                "call can only be used without flags");
            break;
        }
        if (value_is_interrupt(&c->instr->branch.addr)) {
            emit_branch(c, MN_INT, &c->instr->branch.addr);
            break;
        }
        emit_branch(c, MN_CALL, &c->instr->branch.addr);
        break;
    case INSTR_INT: emit_branch(c, MN_INT, &c->instr->branch.addr); break;
    case INSTR_JMP: {
        mnemonic mnemonic;

        value *branch_addr = &c->instr->branch.addr;
        switch (c->instr->branch.type) {
        case BRANCH_NONE: mnemonic = MN_JMP; break;
        case BRANCH_EQ:   mnemonic = MN_JZ;  break;
        case BRANCH_NEQ:  mnemonic = MN_JNZ; break;
        case BRANCH_LT_U: mnemonic = MN_JNC; break;
        case BRANCH_GE_U: mnemonic = MN_JC;  break;
        case BRANCH_LE_U:
            emit_branch(c, MN_JZ,  branch_addr);
            emit_branch(c, MN_JNC, branch_addr);
            return;
        case BRANCH_GT_U: {
            int64_t end_jumps[2] = {emit_blank_jmp(c, MN_JNC), emit_blank_jmp(c, MN_JZ)};

            emit_branch(c, MN_JMP, branch_addr);
            resolve_blank_jmps(c, end_jumps, ARRAYLEN(end_jumps));
            return;
        }
        case BRANCH_GT_S: {
            /*
            JZ end
            JS sign_setted

            JNO target
            JMP end

            sign_setted:
            JO target

            end:
            */
            int64_t end_jumps[2];
            end_jumps[0] = emit_blank_jmp(c, MN_JZ);

            int64_t sign_setted_jumps[1];
            sign_setted_jumps[0] = emit_blank_jmp(c, MN_JS);

            emit_branch(c, MN_JNO, branch_addr);
            end_jumps[1] = emit_blank_jmp(c, MN_JMP);

            resolve_blank_jmps(c, sign_setted_jumps, ARRAYLEN(sign_setted_jumps));
            emit_branch(c, MN_JO, branch_addr);

            resolve_blank_jmps(c, end_jumps, ARRAYLEN(end_jumps));
            return;
        }
        case BRANCH_GE_S: {
            /*
            JS sign_setted

            JNO target
            JMP end

            sign_setted:
            JO target

            end:
            */
            int64_t sign_setted_jumps[1];
            sign_setted_jumps[0] = emit_blank_jmp(c, MN_JS);

            emit_branch(c, MN_JNO, branch_addr);

            int64_t end_jumps[1];
            end_jumps[0] = emit_blank_jmp(c, MN_JMP);

            resolve_blank_jmps(c, sign_setted_jumps, ARRAYLEN(sign_setted_jumps));
            emit_branch(c, MN_JO, branch_addr);

            resolve_blank_jmps(c, end_jumps, ARRAYLEN(end_jumps));
            return;
        }
        case BRANCH_LT_S: {
            /*
            JS sign_setted

            JO target
            JMP end

            sign_setted:
            JNO target

            end:
            */
            int64_t sign_setted_jumps[1];
            sign_setted_jumps[0] = emit_blank_jmp(c, MN_JS);

            emit_branch(c, MN_JO, branch_addr);
            int64_t end_jumps[1];
            end_jumps[0] = emit_blank_jmp(c, MN_JMP);

            resolve_blank_jmps(c, sign_setted_jumps, ARRAYLEN(sign_setted_jumps));
            emit_branch(c, MN_JNO, branch_addr);

            resolve_blank_jmps(c, end_jumps, ARRAYLEN(end_jumps));
            return;
        }
        case BRANCH_LE_S: {
            /*
            JZ target

            JS sign_setted

            JO target
            JMP end

            sign_setted:
            JNO target

            end:
            */
            emit_branch(c, MN_JZ, branch_addr);
            int64_t sign_setted_jumps[1];
            sign_setted_jumps[0] = emit_blank_jmp(c, MN_JS);

            emit_branch(c, MN_JO, branch_addr);

            int64_t end_jumps[1];
            end_jumps[0] = emit_blank_jmp(c, MN_JMP);

            resolve_blank_jmps(c, sign_setted_jumps, ARRAYLEN(sign_setted_jumps));
            emit_branch(c, MN_JNO, branch_addr);

            resolve_blank_jmps(c, end_jumps, ARRAYLEN(end_jumps));
            return;
        }
        }
        emit_branch(c, mnemonic, branch_addr);
        break;
    }
    case INSTR_RET:  emit_instr(c, MN_RET, &none_value, &none_value, NULL); break;
    case INSTR_RETI: emit_instr(c, MN_IRET, &none_value, &none_value, NULL); break;
    case INSTR_LOAD: {
        ir_instr *next_instr = get_next_instr(c);
        // [sp] = r0
        // sp -= 1
        if (value_is_addr(&c->instr->bin.lhs, VAL_SP) && next_instr->kind == INSTR_SUB &&
            value_is_simple(&next_instr->bin.lhs, VAL_SP) && 
            value_is_simple(&next_instr->bin.rhs, VAL_EXPR) &&
            eval_expr(c, next_instr->bin.rhs.expr) == 1)
        {
            emit_instr(c, MN_PUSH, &c->instr->bin.rhs, &none_value, NULL);
            c->instr_index++;
            break;
        }

        emit_binary(c, MN_MOV);
        break;
    }
    case INSTR_ADD: case INSTR_SUB: emit_add_sub(c); break;
    case INSTR_MUL: case INSTR_DIV: emit_mul_div(c); break;
    case INSTR_AND: case INSTR_OR:  emit_and_or(c);  break;
    case INSTR_XOR: emit_binary(c, MN_XOR); break;
    case INSTR_SHL: emit_binary(c, MN_SHL); break;
    case INSTR_SHR: emit_binary(c, MN_SHR); break;
    case INSTR_ROL: emit_binary(c, MN_ROL); break;
    case INSTR_ROR: emit_binary(c, MN_ROR); break;
    case INSTR_NOT:
        eval_value(c, &c->instr->unary.operand);
        emit_instr(c, MN_NOT, &c->instr->unary.operand, NULL, c->instr->unary.tok);
        break;
    case INSTR_CMP: {
        value *lhs = &c->instr->bin.lhs;
        if (lhs->op_type == OPERATOR_BIT_AND) {
            int next_jmp_index = c->instr_index+1;
            ir_instr *next_jmp = vector_get(c->instrs, next_jmp_index);
            while (next_jmp->kind != INSTR_JMP) {
                if (next_jmp_index >= c->instrs->count) goto emit_cmp;
                next_jmp = vector_get(c->instrs, next_jmp_index++);
            }
            if (next_jmp->branch.type != BRANCH_EQ && next_jmp->branch.type != BRANCH_NEQ) {
                ERROR_AT(c->instr->bin.tok, c->stream, "'&' can only be used with '== 0' or '!= 0'");
                break;
            }
            eval_value(c, &c->instr->bin.rhs);
            if (!value_is_number(&c->instr->bin.rhs, 0)) {
                ERROR_AT(c->instr->bin.tok, c->stream, "the value after '==' or '!=' must be 0");
                break;
            }

            value bit_lhs = *lhs;
            bit_lhs.op_type = OPERATOR_NONE;
            bit_lhs.operand = NULL;

            eval_value(c, &bit_lhs);
            eval_value(c, lhs->operand);
            emit_instr(c, MN_BIT, &bit_lhs, lhs->operand, c->instr->bin.tok);
            break;
        }
        if (lhs->op_type != OPERATOR_NONE) {
            ERROR_AT(c->instr->bin.tok, c->stream, 
                "invalid cmp operation (the only avaiable is '&')");
            break;
        }
    emit_cmp:
        emit_binary(c, MN_CMP);
        break;
    }
    case INSTR_REM:
        emit_instr(c, MN_ERR, &c->instr->bin.lhs, &c->instr->bin.rhs, NULL); 
        break;
    case INSTR_LABEL: case INSTR_COUNT: break;
    }
}

static void build_mnemonic_ranges(mnemonic_range mnemonic_ranges[MN_COUNT]) {
    for (int i = 0; i < MN_COUNT; i++) {
        mnemonic_ranges[i].start = -1;
        mnemonic_ranges[i].end = 0;
    }

    for (int i = 0; i < instruction_forms_count; i++) {
        mnemonic mn = instruction_forms[i].mn;

        if (mnemonic_ranges[mn].start == -1)
            mnemonic_ranges[mn].start = i;

        mnemonic_ranges[mn].end = i;
    }
}
static inline void sprk32_backend_reset(codegen_ctx *c) {
    sprk32_emitter *emitter = c->backend->emitter;
    c->current_ip = 0;
    emitter->offset = 0;
    emitter->max_ip = 0;
}

void sprk32_backend_init(arch_backend *backend, sprk32_emitter *emitter) {
    build_mnemonic_ranges(emitter->mnemonic_ranges);

    // Create backend
    *backend = (arch_backend){
        .register_count = REG_COUNT,
        .max_value_size = SIZE_WORD,
        .emitter = emitter,
        .alignment = {SIZE_BYTE, SIZE_HWORD, SIZE_HWORD, SIZE_HWORD},
        .reset = &sprk32_backend_reset,
        .gen_instr = &sprk32_gen_instr,
        .emit = &sprk32_emit,
    };
}

typedef struct {
    const uint8_t *buffer;
    size_t buffer_size;
    FILE *out;
    size_t index;
} disassembler;
static value *disassemble_value(disassembler *disasm, const value *v) {
    if (!v) return NULL;
    value *out = my_malloc(sizeof *out);
    *out = *v;
    if (out->kind == VAL_EXPR) {
        out->expr = my_malloc(sizeof *out->expr);
        out->expr->kind = EXPR_NUMBER;
        out->expr->number = v->size == SIZE_BYTE ? disasm->buffer[disasm->index] :
                            v->size == SIZE_HWORD ? read16(disasm->buffer, disasm->index) :
                            v->size == SIZE_WORD ? read32(disasm->buffer, disasm->index) : 0;
        out->expr->result = out->expr->number;
        disasm->index += v->size;
    }
    if (v->operand) out->operand = disassemble_value(disasm, v->operand);
    return out;
}
static size_t count_expr_bytes(const value *v) {
    if (!v) return 0;
    uint32_t bytes = (v->kind == VAL_EXPR ? v->size : 0);
    if (v->operand) bytes += count_expr_bytes(v->operand);
    return bytes;
}

#define DISASM_REPEAT_THRESHOLD 5
static inline void check_instr_repeated(disassembler *disasm, int *count) {
    if (*count > 0)
        fprintf(disasm->out, "[warning] last is repeated for '%d' times\n", *count - DISASM_REPEAT_THRESHOLD + 1);
    *count = 0;
}
static void disasm_print(disassembler *disasm, const instruction_form *form, size_t instr_start, size_t total_bytes) {
    value *v1 = disassemble_value(disasm, &form->value1);
    value *v2 = disassemble_value(disasm, &form->value2);
    fprintf(disasm->out, "%08zu  ", instr_start);
    for (int i = 0; i < total_bytes; i++)
        fprintf(disasm->out, "%02X ", disasm->buffer[instr_start + i]);
    for (int i = total_bytes; i < 8; i++) fprintf(disasm->out, "   ");
    fprintf(disasm->out, "%-8s", mnemonic_str[form->mn]);
    print_values(disasm->out, v1, v2, true);

    value_free(v1);
    my_free(v1);
    value_free(v2);
    my_free(v2);
}
bool sprk32_disassemble(const char *file_name, FILE *out_file, const uint8_t *buffer, const size_t buffer_size) {
    disassembler disasm = { .buffer = buffer, .out = out_file, .index = 0 };
    size_t last_instr_start = 0;
    int consecutive_count = 0;
    bool skipping_repeated = false;

    while (disasm.index < buffer_size) {
        size_t instr_start = disasm.index++;
        uint8_t instr = buffer[instr_start];
        uint8_t block = (instr >> 4) & 0x0F;
        uint8_t arg1 = (instr >> 2) & 0x03;
        uint8_t arg2 = instr & 0x03;

        const instruction_form *form = NULL;
        for (int x = 0; x < instruction_forms_count; x++) {
            const instruction_form *f = &instruction_forms[x];
            if (f->block == block && f->arg1 == arg1 && f->arg2 == arg2) {
                form = &instruction_forms[x];
                break;
            }
        }
        if (!form) {
            printf("[Disassembler] error: instruction 0x%X of block '%s' with args %d;%d not found\n", instr, opcode_block_str[block], arg1, arg2);
            return false;
        }

        size_t bytes1 = count_expr_bytes(&form->value1);
        size_t bytes2 = count_expr_bytes(&form->value2);
        size_t total_bytes = 1 + bytes1 + bytes2;
        if (instr_start > 0 && !memcmp(&buffer[instr_start], &buffer[last_instr_start], total_bytes)) {
            last_instr_start = instr_start;
            consecutive_count++;
            if (consecutive_count < DISASM_REPEAT_THRESHOLD) {
                disasm_print(&disasm, form, instr_start, total_bytes);
                continue;
            }
            disasm.index += total_bytes - 1;
            skipping_repeated = true;
            continue;
        }

        if (skipping_repeated) {
            check_instr_repeated(&disasm, &consecutive_count);
            skipping_repeated = false;
        }
        consecutive_count = 0;
        last_instr_start = instr_start;
        disasm_print(&disasm, form, instr_start, total_bytes);
    }

    check_instr_repeated(&disasm, &consecutive_count);
    return true;
}