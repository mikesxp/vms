#ifndef COMPILER_H
#define COMPILER_H

#include "lexer.h"
#include "../utils.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct codegen_ctx codegen_ctx;
typedef struct {
    uint8_t register_count;
    prim_size max_value_size;
    prim_size alignment[SIZES_COUNT];
    void *emitter;
    void (*reset)(struct codegen_ctx *c);
    void (*gen_instr)(struct codegen_ctx *c);
    void (*emit)(struct codegen_ctx *c, int64_t value, prim_size size);
} arch_backend;
struct ir_instr;
struct codegen_ctx {
    token_stream *stream;
    arch_backend *backend;
    vector *instrs;
    struct ir_instr *instr;
    size_t instr_index;
    uint64_t current_ip;
};
static inline struct ir_instr *get_next_instr(codegen_ctx *c) {
    return vector_get(c->instrs, c->instr_index+1);
}

struct value;
bool value_matches(const struct value *actual, const struct value *expected);
void print_value(FILE *out, const struct value *value, bool show_imm);
void print_values(FILE *out, const struct value *value1, const struct value *value2, bool show_imm);

void emit_value(codegen_ctx *c, const struct value *value, prim_size size);
void eval_value(codegen_ctx *c, struct value *value);
struct expr_node;
int64_t eval_expr(codegen_ctx *c, struct expr_node *expr);

bool compile(vector *files, arch_backend *backend, vector *shared_labels);
void labels_free(vector *labels);

#endif