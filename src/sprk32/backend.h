#ifndef BACKEND_H
#define BACKEND_H

#include "../compiler/frontend.h"
#include "../compiler/compiler.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MN_NOP,
    MN_HLT,
    MN_RET,
    MN_IRET,
    MN_INT,

    MN_STC,
    MN_CLC,
    MN_STI,
    MN_CLI,

    MN_CALL,
    MN_JMP,
    MN_JZ,
    MN_JNZ,
    MN_JC,
    MN_JNC,
    MN_JO,
    MN_JNO,
    MN_JS,
    MN_JNS,

    MN_MOV,

    MN_ADD,
    MN_ADC,
    MN_SUB,
    MN_SBC,

    MN_MUL,
    MN_DIV,

    MN_AND,
    MN_OR,
    MN_XOR,

    MN_SHL,
    MN_SHR,
    MN_ROL,
    MN_ROR,

    MN_BIT,
    MN_SET,
    MN_RES,

    MN_INC,
    MN_DEC,
    MN_NOT,
    MN_NEG,

    MN_PUSH,
    MN_POP,
    MN_ERR,

    MN_CMP,
    MN_COUNT,
} mnemonic;
typedef struct {
    int start;
    int end;
} mnemonic_range;
typedef struct {
    mnemonic_range mnemonic_ranges[MN_COUNT];
    uint8_t *out;
    uint32_t out_size;
    uint32_t offset, max_ip;
} sprk32_emitter;
void sprk32_emit(codegen_ctx *c, int64_t value, prim_size size);
void sprk32_backend_init(arch_backend *backend, sprk32_emitter *emitter);
bool sprk32_disassemble(const char *file_name, FILE *out_file, const uint8_t *buffer, const size_t buffer_size);

typedef struct {
    mnemonic mn;

    uint8_t block;
    uint8_t arg1;
    uint8_t arg2;

    value value1;
    value value2;
} instruction_form;
extern const instruction_form instruction_forms[];
extern const char *mnemonic_str[MN_COUNT];
extern const size_t instruction_forms_count;

#endif