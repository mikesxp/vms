#include "compiler.h"
#include "preproc.h"
#include "frontend.h"
#include "lexer.h"
#include <stdio.h>

static int calculate_field_offset(codegen_ctx *c, layout *layout, layout_field *field) {
    int offset = 0;
    int padding_size = 0;
    for (int i = 0; i < layout->fields.count; i++) {
        layout_field *current_field = vector_get(&layout->fields, i);

        int64_t element_size = eval_expr(c, current_field->element_size);
        int64_t alignment_size = element_size; // The size for the alignment

        int64_t total_size = element_size * eval_expr(c, current_field->elements_count);
        if (layout->aligned) {
            // Example:
            // layout MY_LAYOUT aligned
            //      A sizeof LAYOUT -> Align A from the layout type
            if (current_field->element_size->kind == EXPR_LAYOUT &&
                current_field->element_size->layout.is_sizeof &&
                !current_field->element_size->layout.field &&
                !current_field->element_size->layout.index)
            {
                layout_field *first_field = vector_get(&current_field->element_size->layout.value->fields, 0);
                alignment_size = eval_expr(c, first_field->element_size);
            }
            if (alignment_size <= SIZE_DWORD && alignment_size % 2 == 0) {
                uint64_t index = 64 - __builtin_clzll(alignment_size % SIZE_DWORD);
                uint64_t alignment = c->backend->alignment[index];
                offset = ((offset + alignment - 1) / alignment) * alignment;
            }
        }
        if (tokens_equal(current_field->name, field->name)) break;
        if (token_is_str(current_field->name, "padding")) padding_size = total_size;
        offset += total_size;
    }
    offset -= padding_size;
    return offset;
}
int64_t eval_expr(codegen_ctx *c, expr_node *expr) {
    if (!expr) return 0;
    switch (expr->kind) {
    case EXPR_NUMBER: expr->result = expr->number; break;
    case EXPR_LABEL:  expr->result = expr->label->instr->addr; break;
    case EXPR_BITSET: {
        int64_t result = 0;
        if (!expr->bitset.bit->index) {
            vector *bits = &expr->bitset.set->bits;
            for (int i = 0; i < bits->count; i++) {
                bitdef *def = vector_get(bits, i);
                if (def == expr->bitset.bit) break;
                if (def->index) result = eval_expr(c, def->index);
                else result += 1;
            }
        } else result = eval_expr(c, expr->bitset.bit->index);
        expr->result = 1 << result;
        break;
    }
    case EXPR_LAYOUT: {
        layout *layout = expr->layout.value;
        layout_field *field = expr->layout.field;
        expr->result = 0;
        if (field) {
            int64_t count = eval_expr(c, field->elements_count);
            int64_t size = eval_expr(c, field->element_size);
            if (expr->layout.is_sizeof) {
                expr->result = size * count;
                break;
            }

            if (expr->layout.index) {
                if (count == 1) {
                    ERROR_AT(expr->tok, c->stream,
                        "layout field of '%.*s' does not support indexing (it only has one element)",
                            expr->tok->len, expr->tok->start);
                    return false;
                }

                int64_t index = eval_expr(c, expr->layout.index);
                if (index >= count) {
                    ERROR_AT(expr->tok, c->stream, "array index out of bounds");
                    return false;
                }
                expr->result += size * index;
            }
            expr->result += calculate_field_offset(c, layout, field);
            break;
        }

        if (expr->layout.is_sizeof) {
            field = vector_get(&layout->fields, layout->fields.count - 1);

            int64_t size = eval_expr(c, field->element_size);
            int64_t count = eval_expr(c, field->elements_count);
            expr->result = calculate_field_offset(c, layout, field) + size * count;
            break;
        }

        for (int i = 0; i < layout->fields.count; i++) {
            field = vector_get(&layout->fields, i);
            if (token_is_str(field->name, "padding")) {
                expr->result += eval_expr(c, field->element_size) *
                                eval_expr(c, field->elements_count);
                break;
            }
        }
        break;
    }
    case EXPR_UNARY: {
        int64_t val = eval_expr(c, expr->unary.expr);
        switch (expr->unary.op) {
        case OPERATOR_UNARY_MINUS: val = -val; break;
        case OPERATOR_BIT_NOT:     val = ~val; break;
        case OPERATOR_LOGIC_NOT:   val = !val; break;
        case OPERATOR_SIN:         val = (int64_t)sin((double)val); break;
        case OPERATOR_COS:         val = (int64_t)cos((double)val); break;
        case OPERATOR_TAN:         val = (int64_t)tan((double)val); break;
        case OPERATOR_EXP:         val = (int64_t)exp((double)val); break;
        case OPERATOR_LOG:         val = (int64_t)log((double)val); break;
        case OPERATOR_ABS:         val = labs(val); break;
        case OPERATOR_SQRT:        val = (int64_t)sqrt((double)val); break;
        case OPERATOR_SIGN:        val = (val > 0) - (val < 0); break;
        case OPERATOR_CEIL:        val = (int64_t)ceil((double)val); break;
        case OPERATOR_FLOOR:       val = (int64_t)floor((double)val); break;
        default: break;
        }
        expr->result = val;
        break;
    }
    case EXPR_BINARY: {
        int64_t lhs = eval_expr(c, expr->binary.lhs);
        int64_t rhs = eval_expr(c, expr->binary.rhs);
        switch (expr->binary.op) {
        case OPERATOR_ADD: lhs += rhs;  break;
        case OPERATOR_SUB: lhs -= rhs;  break;
        case OPERATOR_MUL: lhs *= rhs;  break;
        case OPERATOR_DIV:
            if (rhs == 0) {
                ERROR_AT(expr->binary.op_tok, c->stream, "division by zero");
                break;
            }
            lhs /= rhs;
            break;
        case OPERATOR_MOD:
            if (rhs == 0) {
                ERROR_AT(expr->binary.op_tok, c->stream, "modulo by zero");
                return false;
            }
            lhs %= rhs;
            break;
        case OPERATOR_BIT_AND: lhs &= rhs; break;
        case OPERATOR_BIT_OR:  lhs |= rhs; break;
        case OPERATOR_BIT_XOR: lhs ^= rhs; break;
        case OPERATOR_LSHIFT:  lhs <<= rhs; break;
        case OPERATOR_RSHIFT:  lhs >>= rhs; break;
        case OPERATOR_POW: lhs = (int64_t)pow((double)lhs, (double)rhs); break;
        case OPERATOR_MIN: lhs = (lhs < rhs) ? lhs : rhs; break;
        case OPERATOR_MAX: lhs = (lhs > rhs) ? lhs : rhs; break;
        case OPERATOR_LT:  lhs = (lhs < rhs); break;
        case OPERATOR_LE:  lhs = (lhs <= rhs); break;
        case OPERATOR_EQ:  lhs = (lhs == rhs); break;
        case OPERATOR_NEQ: lhs = (lhs != rhs); break;
        case OPERATOR_GE:  lhs = (lhs >= rhs); break;
        case OPERATOR_GT:  lhs = (lhs > rhs); break;
        case OPERATOR_LOGIC_AND: lhs = (lhs && rhs); break;
        case OPERATOR_LOGIC_OR:  lhs = (lhs || rhs); break;
        default: break;
        }
        expr->result = lhs;
        break;
    }
    }
    return expr->result;
}
void emit_value(codegen_ctx *c, const value *val, prim_size size) {
    if (!val) return;
    if (val->kind == VAL_EXPR) c->backend->emit(c, val->expr->result, size);
    if (val->operand) emit_value(c, val->operand, val->operand->size);
}
void eval_value(codegen_ctx *c, value *val) {
    if (!val) return;
    if (val->kind == VAL_EXPR) {
        eval_expr(c, val->expr);
        val->size = get_primitive_size(val->expr->result);
    }
    while (val->operand) {
        val = val->operand;
        eval_value(c, val);
    }
}
bool value_matches(const value *actual, const value *expected) {
    if (!actual && !expected) return true;
    if (!actual || !expected || actual->kind != expected->kind || actual->is_addr != expected->is_addr ||
        actual->op_type != expected->op_type || !value_matches(actual->operand, expected->operand))
        return false;

    switch (actual->kind) {
    case VAL_REG:
        return actual->reg.index == expected->reg.index &&
            (actual->size == SIZE_NONE || expected->size == SIZE_NONE ||
            actual->size == expected->size) &&
            (actual->reg.sign == SIGN_NONE || expected->reg.sign == SIGN_NONE ||
            actual->reg.sign == expected->reg.sign);
    case VAL_EXPR: return actual->size <= expected->size;
    case VAL_FLAGS:
    case VAL_SP:
    case VAL_NONE: return true;
    }
}

// Prints a value without '\n', 'show imm' shows the immediate value if the value kind is an expression.
void print_value(FILE *out, const value *val, bool show_imm) {
    if (!val) return;
    if (val->is_addr) fprintf(out, "[");
    switch (val->kind) {
    case VAL_REG: {
        bool size_none = val->size == SIZE_NONE || val->size == SIZE_WORD;
        if (val->reg.sign == SIGN_NONE && size_none) {
            fprintf(out, "r%d", val->reg.index);
            break;
        }
        const char *sign = val->reg.sign == SIGN_SIGNED ? "int" :
                    val->reg.sign == SIGN_UNSIGNED ? "uint" : "";
        if (size_none) {
            fprintf(out, "r%d(%s)", val->reg.index, sign);
            break;
        }
        fprintf(out, "r%d(%s%d)", val->reg.index, sign, val->size * 8);
        break;
    }
    case VAL_EXPR: {
        uint8_t size = val->size * 8;
        if (show_imm) {
            fprintf(out, "%lld(imm%d)", val->expr->result, size);
            break;
        }
        fprintf(out, "imm%d", size);
        break;
    }
    case VAL_FLAGS: fprintf(out, "flags"); break;
    case VAL_SP:    fprintf(out, "sp");    break;
    case VAL_NONE:  break;
    }
    if (val->operand) {
        const char op_char[][3] = {
            [OPERATOR_ADD]     = "+", [OPERATOR_SUB]     = "-",  [OPERATOR_MUL]    = "*",
            [OPERATOR_DIV]     = "/", [OPERATOR_BIT_AND] = "&",  [OPERATOR_BIT_OR] = "|", 
            [OPERATOR_BIT_XOR] = "^", [OPERATOR_LSHIFT]  = "<<", [OPERATOR_RSHIFT] = ">>",
        };
        fprintf(out, " %s ", op_char[val->op_type]);
        print_value(out, val->operand, show_imm);
    }
    if (val->is_addr) fprintf(out, "]");
}
void print_values(FILE *out, const value *val1, const value *val2, bool show_imm) {
    print_value(out, val1, show_imm);
    if (val2->kind != VAL_NONE) {
        fprintf(out, ", ");
        print_value(out, val2, show_imm);
    }
    fputc('\n', out);
}

void codegen(arch_backend *backend, vector *instrs, token_stream *stream) {
    codegen_ctx c = {stream, backend, instrs};
    bool changed = true;

    while (changed && !c.stream->error_occured) {
        if (backend->reset) backend->reset(&c);
        c.current_ip = 0;

        uint64_t current_address = c.current_ip;
        for (int i = 0; i < instrs->count; i++) {
            ir_instr *instr = vector_get(instrs, i);
            if (instr->kind == INSTR_LIMIT) {
                uint64_t limit_addr = eval_expr(&c, instr->limit.start);
                current_address = limit_addr;
            }

            // Recalculate the addresses of all instructions
            instr->addr = current_address;
            current_address += instr->current_size;
        }

        changed = false;
        for (c.instr_index = 0; c.instr_index < instrs->count && !c.stream->error_occured; c.instr_index++) {
            c.instr = vector_get(instrs, c.instr_index);

            uint64_t start_ip = c.current_ip;
            c.backend->gen_instr(&c);

            if (c.instr->kind == INSTR_LIMIT) {
                expr_node *last_limit = c.instr->limit.start;
                uint64_t new_limit = eval_expr(&c, last_limit);

                if (new_limit != last_limit->result) {
                    last_limit->result = new_limit;
                    changed = true;
                }

                current_address = last_limit->result;
                continue;
            }

            uint64_t size = c.current_ip - start_ip;
            if (c.instr->current_size < size) {
                c.instr->current_size = size;
                changed = true;
            }
        }
    }
}

void labels_free(vector *labels) {
    for (int i = 0; i < labels->count; i++) {
        label *lb = vector_get(labels, i);
        if (lb->type == LABEL_NAMED) my_free(lb->name);
        my_free(lb->instr);
        my_free(lb);
    }
    vector_free(labels);
}

bool compile(vector *files, arch_backend *backend, vector *shared_labels) {
    if (files->count == 0) return false;
    source_file *file = vector_get(files, files->count - 1);
    if (!file->content) return false;

    token_stream raw_stream = {0};
    if (!lex(file, &raw_stream.tokens)) {
        stream_free(&raw_stream);
        return false;
    }

    token_stream token_stream = {0};
    if (!preprocess(files, &raw_stream, &token_stream.tokens)) {
        vector_free(&token_stream.tokens);
        stream_free(&raw_stream);
        return false;
    }
    frontend_shared shared = {
        .backend = backend,
        .stream = &token_stream,
        .labels = shared_labels,
    };
    vector_init(&shared.instrs);
    vector_init(&shared.deleted_layouts);
    hashmap_init(&shared.bitsets);
    hashmap_init(&shared.layouts);

    if (irgen(&shared))
        codegen(backend, &shared.instrs, &token_stream);

    for (int i = 0; i < token_stream.tokens.count; i++) {
        token *t = vector_get(&token_stream.tokens, i);
        if (t->generated) { // The tok is created by the preprocessor
            my_free(t->start);
            my_free(t);
        }
    }
    vector_free(&token_stream.tokens);
    stream_free(&raw_stream);

    frontend_shared_free(&shared);
    return !token_stream.error_occured;
}