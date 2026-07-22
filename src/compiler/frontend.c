#include "lexer.h"
#include "compiler.h"
#include "frontend.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
    prim_size size;
    register_sign sign;
} register_type;
typedef struct {
    register_type type;
    bool used;
} register_state;
typedef struct {
    register_state *register_states;
    label *current_label;
    frontend_shared *shared;

    uint64_t label_count;
    enum { PARSE_NONE, PARSE_ROUTINE, PARSE_LABEL } parse_mode;
    bool parsing_relop; // Relational operators (only in if stmt)
    vector unresolved_labels; // Vector of unresolved_label
} frontend_ctx;

static inline ir_instr *instr_new(ir_instr_kind kind, frontend_ctx *ctx) {
    ir_instr *instr = my_malloc(sizeof *instr);
    *instr = (ir_instr){kind};
    return instr;
}
static inline expr_node *expr_new(expr_kind kind) {
    expr_node *n = my_malloc(sizeof(*n));
    n->kind = kind;
    return n;
}
static inline expr_node *expr_number(int value, token *tok) {
    expr_node *n = expr_new(EXPR_NUMBER);
    n->number = value;
    n->tok = tok;
    return n;
}
static ir_instr *label_jump_new(label *label, branch_type type, frontend_ctx *ctx) {
    ir_instr *instr = instr_new(INSTR_JMP, ctx);
    instr->branch.type = type;
    instr->branch.addr = (value){.kind = VAL_EXPR, .expr = expr_new(EXPR_LABEL)};
    instr->branch.addr.expr->label = label;
    return instr;
}
static ir_instr *indexed_label_new(frontend_ctx *ctx) {
    ir_instr *instr = instr_new(INSTR_LABEL, ctx);
    instr->label = my_malloc(sizeof *instr->label);
    instr->label->index = ctx->label_count++;
    instr->label->type = LABEL_INDEXED;
    instr->label->instr = instr;
    return instr;
}

static inline void unexpected_token(frontend_ctx *ctx) {
    token *tok = current_token(ctx->shared->stream);
    const char *kind = "tok";
    if (tok->type == TOK_IDENT)    kind = "identifier";
    else if (tok->type == TOK_NUM) kind = "number";
    else if (tok->type >= TOK_KEYWORD_START && tok->type < TOK_KEYWORD_END)
        kind = "keyword";
    else if (tok->type >= TOK_OPERATOR_START && tok->type < TOK_OPERATOR_END)
        kind = "operator";
    else if (tok->type >= TOK_SEPARATOR_START && tok->type < TOK_SEPARATOR_END)
        kind = "separator";
    else if (tok->type >= TOK_SYMBOL_START && tok->type < TOK_SYMBOL_END)
        kind = "symbol";
    else if (tok->type >= TOK_SPECIAL_START && tok->type < TOK_SPECIAL_END)
        kind = "special";
    token *prev = prev_token(ctx->shared->stream);
    ERROR(ctx->shared->stream, "unexpected %s after '%.*s'", kind, prev->len, prev->start);
}

bool token_is_register(uint8_t *index, token *tok, frontend_ctx *ctx) {
    if (tok->type != TOK_IDENT) return false;
    for (uint8_t i = 0; i < ctx->shared->backend->register_count; i++) {
        char reg_name[5];
        snprintf(reg_name, ARRAYLEN(reg_name), "r%u", i);

        if (token_is_str(tok, reg_name)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

static prim_size size_from_string(char *str, size_t strlen) {
    if (strlen == 0) return SIZE_NONE;
    if (strlen == 1 && str[0] == '8') return SIZE_BYTE;
    if (!strncmp(str, "16", strlen)) return SIZE_HWORD;
    if (!strncmp(str, "32", strlen)) return SIZE_WORD;
    if (!strncmp(str, "64", strlen)) return SIZE_DWORD;
    return -1;
}

static token *parse_type(register_type *type, frontend_ctx *ctx) {
    token *type_tok = current_token(ctx->shared->stream);
    if (type_tok->type != TOK_IDENT && type_tok->type != TOK_NUM) return NULL;

    register_sign sign = (*type_tok->start == 's') ? SIGN_SIGNED :
            (*type_tok->start == 'u') ? SIGN_UNSIGNED : SIGN_NONE;
    bool has_prefix = sign != SIGN_NONE;
    prim_size size =
        size_from_string(type_tok->start + has_prefix, type_tok->len - has_prefix);
    if (size == -1) return NULL;

    *type = (register_type){size, sign};
    advance_token(ctx->shared->stream);
    return type_tok;
}

static inline void reset_registers(frontend_ctx *ctx) {
    for (uint8_t i = 0; i < ctx->shared->backend->register_count; i++) {
        ctx->register_states[i].type = (register_type){.sign = SIGN_NONE, .size = SIZE_NONE};
        ctx->register_states[i].used = false;
    }
}
static bool parse_use(frontend_ctx *ctx) {
    bool is_unuse = match_token(TOK_UNUSE, ctx->shared->stream);
    if (!is_unuse && !match_token(TOK_USE, ctx->shared->stream)) return false;
    if (ctx->parse_mode == PARSE_NONE) {
        ERROR_AT(prev_token(ctx->shared->stream), ctx->shared->stream, "'use'/'unuse' can only be used under a label");
        return true;
    }

    ctx->current_label->is_word = true;
    ctx->parse_mode = PARSE_ROUTINE;
    if (match_token(TOK_ALL, ctx->shared->stream)) {
        if (is_unuse) {
            ctx->parse_mode = PARSE_NONE;
            reset_registers(ctx);
            return true;
        }
        register_type type = {SIZE_NONE, SIGN_NONE};
        parse_type(&type, ctx);
        for (int i = 0; i < ctx->shared->backend->register_count; i++) {
            ctx->register_states[i].used = true;
            ctx->register_states[i].type = type;
        }
        return true;
    }

    for (;;) {
        uint8_t reg = 0;
        token *tok = current_token(ctx->shared->stream);
        if (!token_is_register(&reg, tok, ctx)) {
            ERROR_AT(tok, ctx->shared->stream, "'%.*s' is not a register", tok->len, tok->start);
            return true;
        }
        advance_token(ctx->shared->stream);
        ctx->register_states[reg].used = !is_unuse;
        if (!is_unuse) parse_type(&ctx->register_states[reg].type, ctx);
        if (!match_token(TOK_COMMA, ctx->shared->stream)) break;
    }
    return true;
}

static inline bool consume_expression(expr_node **out, frontend_ctx *ctx);
static inline layout_field *check_layout_index(int64_t index, token *field_tok, token *layoutok, layout *layout, frontend_ctx *ctx) {
    if (index < layout->fields.count) return vector_get(&layout->fields, index);
    ERROR_AT(field_tok, ctx->shared->stream,
        "field index '%.*s' out of bounds for layout '%.*s' (%d field available)",
            field_tok->len, field_tok->start, layoutok->len, layoutok->start,
            layout->fields.count);
    return NULL;
}
static layout_field *find_layout_field(frontend_ctx *ctx, token *field_tok, token *layoutok, layout *layout) {
    if (!field_tok) return NULL;
    if (field_tok->type == TOK_NUM)
        return check_layout_index(field_tok->number, field_tok, layoutok, layout, ctx);
    for (int i = 0; i < layout->fields.count; i++) {
        layout_field *field = vector_get(&layout->fields, i);
        if (tokens_equal(field->name, field_tok)) return field;
    }
    ERROR_AT(field_tok, ctx->shared->stream,
        "no field named '%.*s' in layout '%.*s'",
        field_tok->len, field_tok->start,
        layoutok->len, layoutok->start);
    return NULL;
}
static bool parse_expr(expr_node **out, int min_prec, frontend_ctx *ctx);
static inline bool resolve_expression(expr_node **out, frontend_ctx *ctx) {
    return parse_expr(out, 0, ctx);
}
static bool read_layout_value(expr_node **out, frontend_ctx *ctx) {
    token *sizeof_tok = match_token(TOK_SIZEOF, ctx->shared->stream);

    token *tok = current_token(ctx->shared->stream);
    if (tok->type != TOK_IDENT) {
        if (sizeof_tok) EXPECT(ctx->shared->stream, "expected layout name after 'sizeof'");
        return false;
    }
    layout *layout = hashmap_get(&ctx->shared->layouts, tok->start, tok->len);
    if (!layout) {
        if (sizeof_tok)
            ERROR_AT(tok, ctx->shared->stream, "'%.*s' is not a valid layout name", tok->len, tok->start);
        return false;
    }
    advance_token(ctx->shared->stream);

    token *field_tok = NULL;
    if (match_token(TOK_COMMERCIAL_AT, ctx->shared->stream)) {
        field_tok = current_token(ctx->shared->stream);
        if (field_tok->type != TOK_IDENT && field_tok->type != TOK_NUM) {
            EXPECT_PREV(ctx->shared->stream, "expected field name or index");
            return true;
        }
        advance_token(ctx->shared->stream);
    }
    expr_node *n = expr_new(EXPR_LAYOUT);
    n->layout.field = find_layout_field(ctx, field_tok, tok, layout);
    n->layout.is_sizeof = sizeof_tok;
    n->layout.value = layout;
    n->layout.index = NULL;
    n->tok = tok;
    *out = n;

    if (match_token(TOK_LSQUARE, ctx->shared->stream)) {
        if (!resolve_expression(&n->layout.index, ctx)) {
            ctx->shared->stream->pos--;
            return true;
        }
        if (field_tok) {
            consume_token(TOK_RSQUARE, "expected ']'", ctx->shared->stream);
            return true;
        }

        ERROR_AT(prev_token(ctx->shared->stream), ctx->shared->stream, "indexing can only be used with a layout field");
    }
    return true;
}
static bool read_bitset_value(expr_node **out, frontend_ctx *ctx) {
    token *setname = current_token(ctx->shared->stream);
    if (setname->type != TOK_IDENT) return false;
    bitset *bitset = hashmap_get(&ctx->shared->bitsets, setname->start, setname->len);
    if (!bitset) return false;
    advance_token(ctx->shared->stream);
    consume_token(TOK_COMMERCIAL_AT, "expected '@'", ctx->shared->stream);
    token *bit_name = consume_token(TOK_IDENT, "expected bit name", ctx->shared->stream);
    for (int i = 0; i < bitset->bits.count; i++) {
        bitdef *def = vector_get(&bitset->bits, i);
        if (tokens_equal(bit_name, def->name)) {
            expr_node *n = expr_new(EXPR_BITSET);
            n->bitset.set = bitset;
            n->bitset.bit = def;
            n->tok = setname;
            *out = n;
            return true;
        }
    }
    ERROR(ctx->shared->stream, "no field named '%.*s' in bitset '%.*s'", bit_name->len, bit_name->start, setname->len, setname->start);
    return true;
}
static bool parse_expr_factor(expr_node **out, frontend_ctx *ctx) {
    if (match_token(TOK_LPAREN, ctx->shared->stream))
        return parse_expr(out, 0, ctx) && consume_token(TOK_RPAREN, "expected ')'", ctx->shared->stream);
    token *num_tok = match_token(TOK_NUM, ctx->shared->stream);
    if (num_tok) {
        *out = expr_number(num_tok->number, num_tok);
        return true;
    }
    if (read_layout_value(out, ctx) || read_bitset_value(out, ctx)) return true;
    if (next_token(ctx->shared->stream)->type == TOK_COMMERCIAL_AT) {
        token *layout_name = current_token(ctx->shared->stream);
        ERROR_AT(layout_name, ctx->shared->stream, "layout/bitset '%.*s' is undefined", layout_name->len, layout_name->start);
        return false;
    }
    token *label_name = current_token(ctx->shared->stream);
    if (token_is_register(NULL, label_name, ctx)) return false;
    if (label_name->type == TOK_IDENT) {
        advance_token(ctx->shared->stream);

        *out = expr_new(EXPR_LABEL);
        (*out)->tok = label_name;
        for (int i = 0; i < ctx->shared->labels->count; i++) {
            label *label = vector_get(ctx->shared->labels, i);
            if (label->type == LABEL_NAMED && token_is_str(label_name, label->name)) {
                (*out)->label = label;
                return true;
            }
        }
        unresolved_label *unresolved_lb = my_malloc(sizeof *unresolved_lb);
        *unresolved_lb = (unresolved_label){
            .dest = &(*out)->label, .parent = ctx->current_label, .tok = label_name
        };
        vector_add(&ctx->unresolved_labels, unresolved_lb);
        return true;
    }
    return false;
}
static bool parse_expr_unary(expr_node **out, frontend_ctx *ctx) {
    *out = NULL;
    token *tok = current_token(ctx->shared->stream);
    operator_type op = OPERATOR_NONE;
    switch (tok->type) {
        case TOK_MINUS: op = OPERATOR_UNARY_MINUS; break;
        case TOK_TILDE: op = OPERATOR_BIT_NOT; break;
        case TOK_BANG:  op = OPERATOR_LOGIC_NOT; break;
        default:
            if (token_is_str(tok, "sin")) op = OPERATOR_SIN;
            else if (token_is_str(tok, "cos")) op = OPERATOR_COS;
            else if (token_is_str(tok, "tan")) op = OPERATOR_TAN;
            else if (token_is_str(tok, "exp")) op = OPERATOR_EXP;
            else if (token_is_str(tok, "log")) op = OPERATOR_LOG;
            else if (token_is_str(tok, "abs")) op = OPERATOR_ABS;
            else if (token_is_str(tok, "sqrt")) op = OPERATOR_SQRT;
            else if (token_is_str(tok, "sign")) op = OPERATOR_SIGN;
            else if (token_is_str(tok, "ceil")) op = OPERATOR_CEIL;
            else if (token_is_str(tok, "floor")) op = OPERATOR_FLOOR;
            else return parse_expr_factor(out, ctx);
    }
    advance_token(ctx->shared->stream);
    expr_node *expr;
    if (!parse_expr_unary(&expr, ctx)) return false;
    *out = expr_new(EXPR_UNARY);
    (*out)->unary.expr = expr;
    (*out)->unary.op = op;
    return true;
}
static int precedence(operator_type t) {
    switch (t) {
        case OPERATOR_POW: return 12;
        case OPERATOR_MUL:
        case OPERATOR_DIV:
        case OPERATOR_MOD: return 11;
        case OPERATOR_ADD:
        case OPERATOR_SUB: return 10;
        case OPERATOR_LSHIFT:
        case OPERATOR_RSHIFT: return 9;
        case OPERATOR_LT:
        case OPERATOR_LE:
        case OPERATOR_GT:
        case OPERATOR_GE: return 8;
        case OPERATOR_EQ:
        case OPERATOR_NEQ: return 7;
        case OPERATOR_BIT_AND: return 6;
        case OPERATOR_BIT_XOR: return 5;
        case OPERATOR_BIT_OR: return 4;
        case OPERATOR_LOGIC_AND: return 3;
        case OPERATOR_LOGIC_OR: return 2;
        case OPERATOR_MIN:
        case OPERATOR_MAX: return 1;
        default: return -1;
    }
}
static inline operator_type token_to_basic_binop(token *tok) {
    switch (tok->type) {
    case TOK_PLUS:  return OPERATOR_ADD;
    case TOK_MINUS: return OPERATOR_SUB;
    case TOK_STAR:  return OPERATOR_MUL;
    case TOK_SLASH: return OPERATOR_DIV;
    case TOK_AND:   return OPERATOR_BIT_AND;
    case TOK_OR:    return OPERATOR_BIT_OR;
    case TOK_XOR:   return OPERATOR_BIT_XOR;
    case TOK_SHL:   return OPERATOR_LSHIFT;
    case TOK_SHR:   return OPERATOR_RSHIFT;
    default:        return OPERATOR_NONE;
    }
}
static inline operator_type token_to_binop(token *tok, frontend_ctx *ctx) {
    operator_type op = token_to_basic_binop(tok);
    if (op != OPERATOR_NONE) return op;
    if (!ctx->parsing_relop) {
        switch (tok->type) {
        case TOK_LESS:          return OPERATOR_LT;
        case TOK_LESS_EQUAL:    return OPERATOR_LE;
        case TOK_EQUAL_EQUAL:   return OPERATOR_EQ;
        case TOK_BANG_EQUAL:    return OPERATOR_NEQ;
        case TOK_GREATER:       return OPERATOR_GT;
        case TOK_GREATER_EQUAL: return OPERATOR_GE;
        default: break;
        }
    }
    if (tok->type == TOK_AND_AND) return OPERATOR_LOGIC_AND;
    if (tok->type == TOK_OR_OR)   return OPERATOR_LOGIC_OR;
    if (token_is_str(tok, "pow")) return OPERATOR_POW;
    if (token_is_str(tok, "min")) return OPERATOR_MIN;
    if (token_is_str(tok, "max")) return OPERATOR_MAX;
    if (token_is_str(tok, "mod")) return OPERATOR_MOD;
    return OPERATOR_NONE;
}
static bool parse_expr(expr_node **out, int min_prec, frontend_ctx *ctx) {
    if (!parse_expr_unary(out, ctx)) return false;
    for (;;) {
        token *op_tok = current_token(ctx->shared->stream);
        operator_type op = token_to_binop(op_tok, ctx);

        int prec = precedence(op);
        if (prec < min_prec) return true;
        advance_token(ctx->shared->stream);

        expr_node *rhs = NULL;
        if (!parse_expr(&rhs, prec + 1, ctx)) return false;

        expr_node *lhs = *out;
        *out = expr_new(EXPR_BINARY);
        (*out)->binary.lhs = lhs;
        (*out)->binary.rhs = rhs;
        (*out)->binary.op = op;
    }
}
static inline bool consume_expression(expr_node **out, frontend_ctx *ctx) {
    if (resolve_expression(out, ctx)) return true;
    expr_free(*out);
    *out = NULL;
    token *currtok = current_token(ctx->shared->stream);
    EXPECT_PREV(ctx->shared->stream, "expected expression before '%.*s'", currtok->len, currtok->start);
    return false;
}

static inline bool consume_value(value *val, frontend_ctx *ctx);
static bool parse_raw_value(value *val, frontend_ctx *ctx) {
    token_stream *stream = ctx->shared->stream;
    val->op_type = OPERATOR_NONE;
    val->operand = NULL;
    if (match_token(TOK_FLAGS, stream)) {
        val->kind = VAL_FLAGS;
        return true;
    }
    if (match_token(TOK_SP, stream)) {
        val->kind = VAL_SP;
        return true;
    }
    if (token_is_register(&val->reg.index, current_token(stream), ctx)) {
        advance_token(stream);
        if (!ctx->register_states[val->reg.index].used) {
            ERROR_AT(prev_token(stream), stream,
                "'r%d' is missing a declaration", val->reg.index);
            return true;
        }

        register_type t = ctx->register_states[val->reg.index].type;
        val->kind = VAL_REG;
        val->reg.sign = t.sign;
        val->size = t.size;
        return true;
    }
    if (resolve_expression(&val->expr, ctx)) {
        val->kind = VAL_EXPR;
        return true;
    }
    if (val->is_addr) EXPECT(stream, "expected an address expression after '['");
    return false;
}
static bool parse_value(value *val, frontend_ctx *ctx) {
    val->is_addr = match_token(TOK_LSQUARE, ctx->shared->stream);
    if (!parse_raw_value(val, ctx)) return false;

    if (match_token(TOK_AS, ctx->shared->stream)) {
        if (val->kind != VAL_REG) {
            ERROR_AT(prev_token(ctx->shared->stream), ctx->shared->stream,
                "'as' can only be used with registers", ctx->shared->stream);
            return true;
        }
        register_type type = {SIZE_NONE, SIGN_NONE};
        if (!parse_type(&type, ctx)) {
            EXPECT_PREV(ctx->shared->stream, "expected type after 'as'", ctx->shared->stream);
            return true;
        }
        val->reg.sign = type.sign;
        val->size = type.size;
    }
    token *tok = current_token(ctx->shared->stream);
    operator_type op = token_to_basic_binop(tok);
    if (op != OPERATOR_NONE) {
        advance_token(ctx->shared->stream);
        val->operand = my_malloc(sizeof *val->operand);
        val->op_type = op;
        consume_value(val->operand, ctx);
    }
    if (val->is_addr)
        consume_token(TOK_RSQUARE, "expected ']'", ctx->shared->stream);
    return true;
}
static inline bool consume_value(value *val, frontend_ctx *ctx) {
    if (parse_value(val, ctx)) return true;
    ERROR(ctx->shared->stream, "expected a value");
    return false;
}
static void parse_instr(frontend_ctx *ctx);
static inline bool report_unclosed_block(frontend_ctx *ctx, token *start_tok) {
    if (ctx->shared->stream->error_occured) return true;
    bool result = at_end(ctx->shared->stream);
    if (result) {
        ERROR(ctx->shared->stream, "%s", "unclosed block");
        report(start_tok, DIAGNOSTIC_NOTE, false, ctx->shared->stream, "block starts here");
    }
    return result;
}
static bool parse_if(frontend_ctx *ctx) {
    token *if_tok = match_token(TOK_IF, ctx->shared->stream);
    if (!if_tok) return false;
    if (ctx->parse_mode != PARSE_ROUTINE) {
        ERROR_AT(prev_token(ctx->shared->stream), ctx->shared->stream, "'if' can only be used under routine");
        return true;
    }

    ctx->parsing_relop = true;
    ir_instr *cmp_instr = instr_new(INSTR_CMP, ctx);
    consume_value(&cmp_instr->bin.lhs, ctx);
    ctx->parsing_relop = false;

    token *value_tok = prev_token(ctx->shared->stream);
    token *operator_tok = take_token(ctx->shared->stream);
    cmp_instr->bin.tok = operator_tok;
    vector_add(&ctx->shared->instrs, cmp_instr);

    branch_type btype = BRANCH_NONE;
    bool is_signed = cmp_instr->bin.lhs.reg.sign == SIGN_SIGNED;
    switch (operator_tok->type) {
    case TOK_EQUAL_EQUAL:   btype = BRANCH_EQ;  break;
    case TOK_BANG_EQUAL:    btype = BRANCH_NEQ; break;
    case TOK_GREATER:       btype = is_signed ? BRANCH_GT_S : BRANCH_GT_U; break;
    case TOK_GREATER_EQUAL: btype = is_signed ? BRANCH_GE_S : BRANCH_GE_U; break;
    case TOK_LESS:          btype = is_signed ? BRANCH_LT_S : BRANCH_LT_U; break;
    case TOK_LESS_EQUAL:    btype = is_signed ? BRANCH_LE_S : BRANCH_LE_U; break;
    default: 
        EXPECT_AT(value_tok, ctx->shared->stream, "unrecognized relational operator after '%.*s'",
                        value_tok->len, value_tok->start);
        return true;
    }

    consume_value(&cmp_instr->bin.rhs, ctx);
    if (value_is_simple(&cmp_instr->bin.lhs, VAL_REG) &&
        value_is_simple(&cmp_instr->bin.rhs, VAL_REG) &&
        cmp_instr->bin.lhs.reg.sign != cmp_instr->bin.rhs.reg.sign)
        ERROR_AT(operator_tok, ctx->shared->stream, "comparison between different registers sign");

    while (!match_token(TOK_LBRACE, ctx->shared->stream)) {
        token *eof = match_token(TOK_EOF, ctx->shared->stream);
        if (eof) {
            ERROR_AT(eof, ctx->shared->stream, "expected '{' in if");
            report(if_tok, DIAGNOSTIC_NOTE, false, ctx->shared->stream, "if starts here");
            break;
        }
        parse_instr(ctx);
    }
    
    token *branch_tok = current_token(ctx->shared->stream);
    bool is_jmp = branch_tok->type == TOK_JMP;
    if (is_jmp || branch_tok->type == TOK_CALL) {
        advance_token(ctx->shared->stream);

        // if operand == operand jmp/call operand
        ir_instr *jmp_instr = instr_new(is_jmp ? INSTR_JMP : INSTR_CALL, ctx);
        parse_value(&jmp_instr->branch.addr, ctx);
        jmp_instr->branch.type = btype;
        jmp_instr->branch.tok = branch_tok;

        vector_add(&ctx->shared->instrs, jmp_instr);
        consume_token(TOK_RBRACE, "expected '}' in if-jmp statement", ctx->shared->stream);
        return true;
    }

    const branch_type inverted_branches[] = {
        [BRANCH_NONE] = BRANCH_NONE, [BRANCH_EQ]   = BRANCH_NEQ,  [BRANCH_NEQ]  = BRANCH_EQ,
        [BRANCH_GE_U] = BRANCH_LT_U, [BRANCH_LE_U] = BRANCH_GT_U, [BRANCH_GT_U] = BRANCH_LE_U,
        [BRANCH_LT_U] = BRANCH_GE_U, [BRANCH_GE_S] = BRANCH_LT_S, [BRANCH_LE_S] = BRANCH_GT_S,
        [BRANCH_GT_S] = BRANCH_LE_S, [BRANCH_LT_S] = BRANCH_GE_S,
    };
    ir_instr *else_label = indexed_label_new(ctx);
    vector_add(&ctx->shared->instrs, label_jump_new(else_label->label, inverted_branches[btype], ctx));
    // Jcond else
    // A
    // jmp end
    // else:
    // B
    // end:
    while (!match_token(TOK_RBRACE, ctx->shared->stream) && !report_unclosed_block(ctx, if_tok)) {
        parse_instr(ctx);
    }

    token *else_tok = match_token(TOK_ELSE, ctx->shared->stream);
    if (else_tok) {
        consume_token(TOK_LBRACE, "expected '{'", ctx->shared->stream);

        // JMP end
        ir_instr *end_label = indexed_label_new(ctx);
        vector_add(&ctx->shared->instrs, label_jump_new(end_label->label, BRANCH_NONE, ctx));

        // Else label
        vector_add(&ctx->shared->instrs, else_label);

        while (!match_token(TOK_RBRACE, ctx->shared->stream) && !report_unclosed_block(ctx, else_tok)) {
            parse_instr(ctx);
        }

        // End label
        vector_add(&ctx->shared->instrs, end_label);
        return true;
    }

    vector_add(&ctx->shared->instrs, else_label);
    return true;
}
static bool parse_labeldef(frontend_ctx *ctx) {
    bool is_interrupt = match_token(TOK_INT, ctx->shared->stream) != NULL;
    if (!is_interrupt && !match_token(TOK_COLON, ctx->shared->stream)) return false;

    token *lb_tok = consume_token(TOK_IDENT, "expected label name", ctx->shared->stream);
    if (!lb_tok) return true;

    // Check if label already exists
    char *lb_name = string_duplicate(lb_tok->start, lb_tok->len);
    for (int i = 0; i < ctx->shared->labels->count; i++) {
        label *defined_label = vector_get(ctx->shared->labels, i);

        // The current label is undefined label owner
        if (defined_label->owner != NULL && defined_label->owner != ctx->current_label) continue;

        if (strcmp(lb_name, defined_label->name) == 0) {
            ERROR_AT(lb_tok, ctx->shared->stream,
                "label '%.*s' is already defined", lb_tok->len, lb_tok->start);
            report(defined_label->tok, DIAGNOSTIC_NOTE, false,
                    ctx->shared->stream, "previous definition is here");
            my_free(lb_name);
            return true;
        }
    }

    // Add instruction
    ir_instr *instr = instr_new(INSTR_LABEL, ctx);
    instr->label = my_malloc(sizeof *instr->label);
    *instr->label = (label) {
        .type = LABEL_NAMED, .name = lb_name,
        .tok = lb_tok, .is_word = false,
        .is_interrupt = is_interrupt, .instr = instr
    };
    switch (ctx->parse_mode) {
    case PARSE_NONE: ctx->parse_mode = PARSE_LABEL;
    case PARSE_LABEL:
        ctx->current_label = instr->label;
        instr->label->owner = NULL;
        break;
    case PARSE_ROUTINE: instr->label->owner = ctx->current_label; break;
    }
    vector_add(ctx->shared->labels, instr->label);
    vector_add(&ctx->shared->instrs, instr);
    return true;
}
static bool parse_return(frontend_ctx *ctx) {
    bool is_semicolon = match_token(TOK_SEMICOLON, ctx->shared->stream);
    if (!is_semicolon && !match_token(TOK_RETURN, ctx->shared->stream)) return false;
    if (ctx->parse_mode == PARSE_NONE) {
        ERROR_AT(prev_token(ctx->shared->stream), ctx->shared->stream,
            "return can only be used under a routine or a label");
        return true;
    }

    ir_instr *instr = instr_new(ctx->current_label->is_interrupt ? INSTR_RETI : INSTR_RET, ctx);
    vector_add(&ctx->shared->instrs, instr);

    if (is_semicolon) {
        reset_registers(ctx);
        ctx->current_label = NULL;
        ctx->parse_mode = PARSE_NONE;
    }
    return true;
}

static bool parse_unary(frontend_ctx *ctx) {
    token *operator_tok = match_token(TOK_TILDE, ctx->shared->stream);
    if (!operator_tok) return false;
    value operand = {0};
    if (!parse_value(&operand, ctx)) return false;

    ir_instr *instr = instr_new(INSTR_NOT, ctx);
    instr->unary.operand = operand;
    instr->unary.tok = operator_tok;
    vector_add(&ctx->shared->instrs, instr);
    return true;
}
static bool parse_binary(frontend_ctx *ctx) {
    value lhs = {0};
    if (!parse_value(&lhs, ctx)) return false;

    token *value_tok = prev_token(ctx->shared->stream);
    token *operator_tok = take_token(ctx->shared->stream);
    ir_instr_kind kind = INSTR_LOAD;
    bool is_assign = true;
    switch (operator_tok->type) {
    case TOK_EQUAL:         break;
    case TOK_PLUS_EQUAL:    kind = INSTR_ADD; break;
    case TOK_MINUS_EQUAL:   kind = INSTR_SUB; break;
    case TOK_STAR_EQUAL:    kind = INSTR_MUL; break;
    case TOK_SLASH_EQUAL:   kind = INSTR_DIV; break;
    case TOK_AND_EQUAL:     kind = INSTR_AND; break;
    case TOK_OR_EQUAL:      kind = INSTR_OR;  break;
    case TOK_XOR_EQUAL:     kind = INSTR_XOR; break;
    case TOK_SHL_EQUAL:     kind = INSTR_SHL; break;
    case TOK_SHR_EQUAL:     kind = INSTR_SHR; break;
    case TOK_ROL_EQUAL:     kind = INSTR_ROL; break;
    case TOK_ROR_EQUAL:     kind = INSTR_ROR; break;
    case TOK_PERCENT_EQUAL: kind = INSTR_REM; break;
    default:
        EXPECT_AT(value_tok, ctx->shared->stream,
            "unrecognized operator after '%.*s'", value_tok->len, value_tok->start);
        goto error_before_rhs;
    }
    if (ctx->parse_mode != PARSE_ROUTINE) {
        ERROR_AT(operator_tok, ctx->shared->stream, "binary operations can only be used under a routine");
        goto error_before_rhs;
    }

    value rhs = {0};
    consume_value(&rhs, ctx);

    ir_instr *instr = instr_new(kind, ctx);
    instr->bin.lhs = lhs;
    instr->bin.rhs = rhs;
    instr->bin.tok = operator_tok;

    vector_add(&ctx->shared->instrs, instr);
    return true;
error_before_rhs:
    value_free(&lhs);
    return true;
}

static bool parse_branch(frontend_ctx *ctx) {
    token *branch_tok = current_token(ctx->shared->stream);
    bool is_jmp = branch_tok->type == TOK_JMP;
    if (!is_jmp && branch_tok->type != TOK_CALL) return false;

    advance_token(ctx->shared->stream);

    ir_instr *instr = instr_new(is_jmp ? INSTR_JMP : INSTR_CALL, ctx);
    instr->branch.type = BRANCH_NONE;
    instr->branch.tok = branch_tok;
    vector_add(&ctx->shared->instrs, instr);

    return consume_value(&instr->branch.addr, ctx);
}

static inline token *consume_label(char **name, frontend_ctx *ctx) {
    token *lb_tok = take_token(ctx->shared->stream);
    if (lb_tok->type != TOK_IDENT) {
        ERROR_AT(lb_tok,  ctx->shared->stream, "'%.*s' is not a valid label name", lb_tok->len, lb_tok->start);
        return NULL;
    }
    *name = string_duplicate(lb_tok->start, lb_tok->len);
    return lb_tok;
}
void layout_free(layout *layout) {
    for (int i = 0; i < layout->fields.count; i++) {
        layout_field *field = vector_get(&layout->fields, i);
        expr_free(field->element_size);
        expr_free(field->elements_count);
        my_free(field);
    }
    vector_free(&layout->fields);
    my_free(layout);
}
static bool parse_data_directive(frontend_ctx *ctx) {
    token *tok = current_token(ctx->shared->stream);
    if (tok->type != TOK_IDENT || tok->start[0] != 'd') return false;

    prim_size size = size_from_string(tok->start + 1, tok->len - 1);
    if (size == SIZE_NONE || size == -1) return false;

    advance_token(ctx->shared->stream);

    ir_instr *instr = instr_new(INSTR_EMIT, ctx);
    instr->emit.size = size;
    vector_init(&instr->emit.values);

    for (;;) {
        expr_node *expr = NULL;
        consume_expression(&expr, ctx);
        vector_add(&instr->emit.values, expr);
        if (!match_token(TOK_COMMA, ctx->shared->stream)) break;
    }

    vector_add(&ctx->shared->instrs, instr);
    return true;
}
static token *find_symbol(frontend_ctx *ctx, token *symbol) {
    layout *layout = hashmap_get(&ctx->shared->layouts, symbol->start, symbol->len);
    if (layout) return layout->tok;
    bitset *bitset = hashmap_get(&ctx->shared->bitsets, symbol->start, symbol->len);
    if (bitset) return bitset->tok;
    for (int i = 0; i < ctx->shared->labels->count; i++) {
        label *lb = vector_get(ctx->shared->labels, i);
        if (token_is_str(symbol, lb->name)) return lb->tok;
    }
    return NULL;
}
static bool check_symbol(token *symbol, frontend_ctx *ctx) {
    token *symtok = find_symbol(ctx, symbol);
    if (!symtok) return false;
    ERROR_AT(symbol, ctx->shared->stream, "'%.*s' is already defined", symbol->len, symbol->start);
    report(symtok, DIAGNOSTIC_NOTE, false, ctx->shared->stream, "definition is here");
    return true;
}
bool parse_declaration(frontend_ctx *ctx) {
    token_stream *stream = ctx->shared->stream;
    bool is_bitset = match_token(TOK_BITSET, stream);
    if (!is_bitset && !match_token(TOK_LAYOUT, stream)) return false;

    token *name = consume_token(TOK_IDENT, "expected identifier", stream);
    if (!name || check_symbol(name, ctx))  return true;

    bool is_aligned = !is_bitset && match_token(TOK_ALIGNED, stream);
    if (!consume_token(TOK_LBRACE, "expected '{'", stream)) return true;

    vector *fields;
    if (is_bitset) {
        bitset *bitset = my_malloc(sizeof *bitset);
        fields = &bitset->bits;
        hashmap_put(&ctx->shared->bitsets, name->start, name->len, bitset);
    } else {
        layout *layout = my_malloc(sizeof *layout);
        layout->aligned = is_aligned;
        fields = &layout->fields;
        hashmap_put(&ctx->shared->layouts, name->start, name->len, layout);
    }
    vector_init(fields);
    while (!stream->error_occured) {
        token *item_name = consume_token(TOK_IDENT, "expected identifier", stream);
        if (!item_name) break;

        bool duplicate = false;
        for (size_t i = 0; i < fields->count; i++) {
            void *item = vector_get(fields, i);
            token *tok = is_bitset ? ((bitdef *)item)->name : ((layout_field *)item)->name;

            if (tokens_equal(item_name, tok)) {
                ERROR_AT(item_name, stream, "'%.*s' already exists in '%.*s'",
                         item_name->len, item_name->start, name->len, name->start);
                break;
            }
        }
        if (is_bitset) {
            bitdef *bit = my_malloc(sizeof *bit);
            bit->name = item_name;
            bit->index = NULL;
            if (match_token(TOK_EQUAL, stream)) consume_expression(&bit->index, ctx);
            vector_add(fields, bit);
        } else {
            layout_field *field = my_malloc(sizeof *field);
            field->name = item_name;
            if (match_token(TOK_LSQUARE, stream)) {
                consume_expression(&field->elements_count, ctx);
                consume_token(TOK_RSQUARE, "expected ']'", stream);
            } else {
                field->elements_count = expr_new(EXPR_NUMBER);
                field->elements_count->number = 1;
            }
            if (!resolve_expression(&field->element_size, ctx)) {
                field->element_size = expr_new(EXPR_NUMBER);
                field->element_size->number = 1;
            }
            vector_add(fields, field);
        }
        if (match_token(TOK_RBRACE, stream)) break;
        if (!consume_token(TOK_COMMA, "expected ',' or '}'", stream)) break;
        if (match_token(TOK_RBRACE, stream)) break;
    }
    return true;
}
static void parse_instr(frontend_ctx *ctx) {
    if (match_token(TOK_LIMIT, ctx->shared->stream)) {
        ir_instr *instr = instr_new(INSTR_LIMIT, ctx);
        consume_expression(&instr->limit.start, ctx);
        consume_expression(&instr->limit.end, ctx);
        vector_add(&ctx->shared->instrs, instr);
        return;
    }
    if (match_token(TOK_RESERVE, ctx->shared->stream)) {
        ir_instr *instr = instr_new(INSTR_RESERVE, ctx);
        consume_expression(&instr->reserve.expr, ctx);
        vector_add(&ctx->shared->instrs, instr);
        return;
    }
    if (match_token(TOK_DELETE, ctx->shared->stream)) {
        token *name = consume_token(TOK_IDENT, "expected layout name", ctx->shared->stream);
        if (!name) return;

        layout *layout = hashmap_get(&ctx->shared->layouts, name->start, name->len);
        if (!layout) {
            ERROR_AT(name, ctx->shared->stream, "layout '%.*s' is undefined", name->len, name->start);
            return;
        }
        vector_add(&ctx->shared->deleted_layouts, layout);
        hashmap_remove(&ctx->shared->layouts, name->start, name->len);
        return;
    }
    if (match_token(TOK_NOP, ctx->shared->stream)) {
        ir_instr *instr = instr_new(INSTR_NOP, ctx);
        vector_add(&ctx->shared->instrs, instr);
        return;
    }
    if (match_token(TOK_HLT, ctx->shared->stream)) {
        ir_instr *instr = instr_new(INSTR_HALT, ctx);
        vector_add(&ctx->shared->instrs, instr);
        return;
    }
    if (parse_declaration(ctx) || parse_data_directive(ctx) || parse_labeldef(ctx) || parse_return(ctx) ||
        parse_use(ctx) || parse_binary(ctx) || parse_if(ctx) || parse_branch(ctx) ||
        parse_unary(ctx)) return;

    unexpected_token(ctx);
}
bool irgen(frontend_shared *shared) {
    if (shared->stream->tokens.count == 0) return true;

    frontend_ctx ctx = {
        .register_states = my_malloc(shared->backend->register_count * sizeof(register_state)),
        .parse_mode = PARSE_NONE,
        .current_label = NULL,
        .shared = shared,
    };

    vector_init(&ctx.unresolved_labels);
    reset_registers(&ctx);

    while (!ctx.shared->stream->error_occured && !at_end(ctx.shared->stream)) parse_instr(&ctx);

    // Resolve labels
    for (int i = 0; i < ctx.unresolved_labels.count; i++) {
        unresolved_label *unresolved_label = vector_get(&ctx.unresolved_labels, i);

        bool found = false;
        for (int j = 0; j < ctx.shared->labels->count; j++) {
            label *target_label = vector_get(ctx.shared->labels, j);

            if (target_label->owner && unresolved_label->parent != target_label->owner) continue;
            if (token_is_str(unresolved_label->tok, target_label->name)) {
                *unresolved_label->dest = target_label;
                found = true;
                break;
            }
        }

        if (!found)
            ERROR_AT(unresolved_label->tok, ctx.shared->stream,
                    "label '%.*s' is undefined", unresolved_label->tok->len,
                    unresolved_label->tok->start);
        my_free(unresolved_label);
    }
    vector_free(&ctx.unresolved_labels);
    my_free(ctx.register_states);
    return !ctx.shared->stream->error_occured;
}
void frontend_shared_free(frontend_shared *shared) {
    hashmap_iter bitset_iter = hashmap_iter_init(&shared->bitsets);
    bitset *bitset;
    while (hashmap_next(&bitset_iter, NULL, (void**)&bitset)) {
        for (int i = 0; i < bitset->bits.count; i++) {
            bitdef *def = vector_get(&bitset->bits, i);
            expr_free(def->index);
            my_free(def);
        }
        vector_free(&bitset->bits);
        my_free(bitset);
    }
    hashmap_free(&shared->bitsets);

    hashmap_iter layout_iter = hashmap_iter_init(&shared->layouts);
    layout *layout;
    while (hashmap_next(&layout_iter, NULL, (void**)&layout)) {
        layout_free(layout);
    }
    hashmap_free(&shared->layouts);
    for (int i = 0; i < shared->deleted_layouts.count; i++) {
        layout = vector_get(&shared->deleted_layouts, i);
        layout_free(layout);
    }
    vector_free(&shared->deleted_layouts);

    for (int i = 0; i < shared->instrs.count; i++) {
        ir_instr *instr = vector_get(&shared->instrs, i);
        switch (instr->kind) {
        case INSTR_LIMIT:
            expr_free(instr->limit.start);
            expr_free(instr->limit.end);
            break;
        case INSTR_RESERVE:
            expr_free(instr->reserve.expr);
            break;
        case INSTR_LABEL:
            if (instr->label->type == LABEL_NAMED) continue;
            my_free(instr->label);
            break;
        case INSTR_EMIT:
            for (int i = 0; i < instr->emit.values.count; i++) {
                expr_node *value = vector_get(&instr->emit.values, i);
                expr_free(value);
            }
            vector_free(&instr->emit.values);
            break;
        case INSTR_NOT:
            value_free(&instr->unary.operand);
            break;
        case INSTR_CALL: case INSTR_INT: case INSTR_JMP:
            value_free(&instr->branch.addr);
            break;
        case INSTR_LOAD: case INSTR_SUB: case INSTR_AND: case INSTR_ADD: case INSTR_MUL:
        case INSTR_DIV:  case INSTR_REM: case INSTR_OR:  case INSTR_XOR: case INSTR_SHL:
        case INSTR_SHR:  case INSTR_ROL: case INSTR_ROR: case INSTR_CMP:
            value_free(&instr->bin.lhs);
            value_free(&instr->bin.rhs);
            break;
        case INSTR_NOP:  case INSTR_HALT:  case INSTR_RET:
        case INSTR_RETI: case INSTR_COUNT: break;
        }
        my_free(instr);
    }
    vector_free(&shared->instrs);
}
void value_free(value *val) {
    if (val->kind == VAL_EXPR) expr_free(val->expr);

    if (val->operand) {
        value_free(val->operand);
        my_free(val->operand);
    }
}
void expr_free(expr_node *expr) {
    if (!expr) return;
    switch (expr->kind) {
    case EXPR_NUMBER: case EXPR_LABEL: case EXPR_BITSET: break;
    case EXPR_LAYOUT:
        expr_free(expr->layout.index);
        break;
    case EXPR_BINARY:
        expr_free(expr->binary.lhs);
        expr_free(expr->binary.rhs);
        break;
    case EXPR_UNARY:
        expr_free(expr->unary.expr);
        break;
    }
    my_free(expr);
}