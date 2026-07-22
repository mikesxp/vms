#include "lexer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
    const char *name;
    token_type type;
} keyword;
static const keyword keywords[] = {
    { "if",        TOK_IF      },
    { "as",        TOK_AS      },
    { "sp",        TOK_SP      },
    { "jmp",       TOK_JMP     },
    { "use",       TOK_USE     },
    { "int",       TOK_INT     },
    { "all",       TOK_ALL     },
    { "nop",       TOK_NOP     },
    { "else",      TOK_ELSE    },
    { "call",      TOK_CALL    },
    { "hlt",      TOK_HLT      },
    { "unuse",    TOK_UNUSE    },
    { "limit",    TOK_LIMIT    },
    { "flags",    TOK_FLAGS    },
    { "return",   TOK_RETURN   },
    { "res",      TOK_RESERVE  },
    { "delete",   TOK_DELETE   },
    { "sizeof",   TOK_SIZEOF   },
    { "layout",   TOK_LAYOUT   },
    { "aligned",  TOK_ALIGNED  },
    { "bitset",   TOK_BITSET   }
};
static void lexer_error(char *msg, lexer_ctx *lexer) {
    char *line_start = lexer->currtok->location.line_start;

    // Calculate line end
    char *line_end = line_start;
    while (*line_end && *line_end != '\n') {
        line_end++;
    }

    // Print error
    printf("[%s:%d] lexer error: %s\n%d | %.*s\n", lexer->currtok->location.path_name,
        lexer->location.line_number, msg, lexer->location.line_number, 
        (int)(line_end - line_start), line_start);
    
    // Skip line number spaces
    int line_width = snprintf(NULL, 0, "%d", lexer->currtok->location.line_number);
    printf("%*s | ", line_width, "");

    for (char *c = line_start; c < lexer->currch; ++c) {
        printf(" ");
    }
    printf("^\n");

    lexer->currtok->type = TOK_ERR;
    lexer->error_occured = true;
    lexer->run = false;
}

static inline void update_newline(lexer_ctx *lexer) {
    lexer->location.new_line = true;
    lexer->location.line_number++;
    lexer->location.line_start = lexer->currch + 1; // Skip '\n'
}

static void skip_whitespace(lexer_ctx *lexer) {
    lexer->location.new_line = lexer->currch == lexer->source; // Reset newline if currch != start
    for (;;) {
        switch (*lexer->currch) {
        case '\n':
            update_newline(lexer);
        case ' ':
        case '\r':
        case '\t':
            lexer->currch++;
            break;
        case '/':
            if (lexer->currch[1] == '/') {
                // Skip comment
                while (*lexer->currch != '\0' && *lexer->currch != '\n')
                    lexer->currch++;
                break;
            }
            
            // Skip multi-line comment
            if (lexer->currch[1] != '*') return;
            lexer->currch += 2;
            for (;;) {
                if (*lexer->currch == '*') {
                    if (lexer->currch[1] == '/') {
                        lexer->currch += 2;
                        break;
                    }
                    if (lexer->currch[-1] == '/') {
                        lexer_error("nested multiline comments are not allowed", lexer);
                        break;
                    }
                }
                if (*lexer->currch == '\0') {
                    lexer_error("unclosed comment", lexer);
                    break;
                };
                if (*lexer->currch == '\n') update_newline(lexer);
                lexer->currch++;
            }
            break;
        default: return;
        }
    }
}

static bool match_next_chr(char **src, char chr) {
    if (**src != '\0' && *(*src + 1) == chr) {
        (*src)++;
        return true;
    }
    return false;
}

static bool is_alpha(char chr) {
    return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || chr == '_';
}

source_file *file_new(vector *files, char *path_name) {
    char *content = file_to_string(path_name);
    if (!content) {
        my_free(path_name);
        return NULL;
    }
    source_file *file = my_malloc(sizeof *file);
    file->path_name = path_name;
    file->content = content;
    vector_add(files, file);
    return file;
}

bool lex_number(lexer_ctx *lexer, int64_t *number) {
    if (!lexer->currch || *lexer->currch == '\0') return false;
    const char *p = lexer->currch;
    if (*p == '+' || *p == '-') p++;
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        int sign = 1;
        if (*lexer->currch == '-') { sign = -1; lexer->currch++; }
        else if (*lexer->currch == '+') { lexer->currch++; }

        char *end = NULL;
        *number = sign * strtol(lexer->currch + 2, &end, 2);
        lexer->currch = end;
        return true;
    }

    bool is_hex = (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'));
    bool is_float = false;
    while (*p) {
        if (*p == '.') {
            is_float = true;
            break;
        }
        if (is_hex && (*p == 'p' || *p == 'P')) {
            is_float = true;
            break;
        }
        if (!is_hex && (*p == 'e' || *p == 'E')) {
            is_float = true;
            break;
        }
        p++;
    }
    char *end;
    if (is_float) *number = strtod(lexer->currch, &end);
    else *number = strtol(lexer->currch, &end, 0);    

    bool result = end != lexer->currch;
    lexer->currch = end;
    return result;
}
static inline void lexer_accept_2chr(lexer_ctx *lexer, token_type type) {
    lexer->currtok->type = type;
    lexer->currtok->len++;
    lexer->currch++;
}
static inline void lexer_accept_3chr(lexer_ctx *lexer, token_type type) {
    lexer->currtok->type = type;
    lexer->currtok->len += 2;
    lexer->currch += 2;
}

bool lex(source_file *file, vector *tokens) {
    if (!file->content) return false;
    lexer_ctx lexer = {
        .source = file->content,
        .currch = lexer.source,
        .currtok = NULL,
        .location = {
            .new_line = true, .line_number = 1, .line_start = lexer.source, 
            .path_name = file->path_name,
        },
        .error_occured = false,
        .run = true,
    };
    vector_init(tokens);

    while (lexer.run) {
        skip_whitespace(&lexer);

        lexer.currtok = my_malloc(sizeof(token));
        *lexer.currtok = (token){ 
            .start = lexer.currch, .location = lexer.location, .len = 1
        };

        char c = *lexer.currch;
        char *next = lexer.currch + 1;
        switch (c) {
        case '\0': 
            lexer.currtok->type = TOK_EOF;
            lexer.run = false;
            break;
        case '$':  lexer.currtok->type = TOK_DOLLAR; break;
        case '[':  lexer.currtok->type = TOK_LSQUARE; break;
        case ']':  lexer.currtok->type = TOK_RSQUARE; break;
        case '(':  lexer.currtok->type = TOK_LPAREN; break;
        case ')':  lexer.currtok->type = TOK_RPAREN; break;
        case '{':  lexer.currtok->type = TOK_LBRACE; break;
        case '}':  lexer.currtok->type = TOK_RBRACE; break;
        case ';':  lexer.currtok->type = TOK_SEMICOLON; break;
        case ',':  lexer.currtok->type = TOK_COMMA; break;
        case '.':
            if (lexer.currch[1] == '.' && lexer.currch[2] == '.') { 
                lexer_accept_3chr(&lexer, TOK_DOTS3);
                break;
            }
            lexer.currtok->type = TOK_DOT; 
            break;
        case '#': 
            lexer.currtok->type = TOK_HASHTAG;
            if (*next == '#') {
                lexer_accept_2chr(&lexer, TOK_HASHTAG_HASHTAG);
                break;
            }
            break;
        case ':':
            lexer.currtok->type = TOK_COLON;
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_COLON_EQUAL);
                break;
            }
            break;
        case '+':
            if (*next == '=') { 
                lexer_accept_2chr(&lexer, TOK_PLUS_EQUAL);
                break;
            }
            if (*next == '+') { 
                lexer_accept_2chr(&lexer, TOK_PLUS_PLUS);
                break;
            }
            lexer.currtok->type = TOK_PLUS;
            break;
        case '-':
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_MINUS_EQUAL);
                break;
            } 
            if (*next == '-') { 
                lexer_accept_2chr(&lexer, TOK_MINUS_MINUS);
                break;
            }
            lexer.currtok->type = TOK_MINUS;
            break;
        case '*':
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_STAR_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_STAR;
            break;
        case '/':
            if (*next == '=') { 
                lexer_accept_2chr(&lexer, TOK_SLASH_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_SLASH;
            break;
        case '&':
            if (*next == '&') {
                lexer_accept_2chr(&lexer, TOK_AND_AND);
                break;
            }
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_AND_EQUAL);
                break; 
            }
            lexer.currtok->type = TOK_AND;
            break;
        case '|':
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_OR_EQUAL);
                break;
            }
            if (*next == '|') { 
                lexer_accept_2chr(&lexer, TOK_OR_OR);
                break;
            }
            lexer.currtok->type = TOK_OR;
            break;
        case '~':
            lexer.currtok->type = TOK_TILDE;
            break;
        case '>':
            if (*next == '>') {
                if (lexer.currch[2] == '=') {
                    lexer_accept_3chr(&lexer, TOK_SHR_EQUAL);
                    break;
                }
                if (lexer.currch[2] == '>' && lexer.currch[3] == '=') {
                    if (lexer.currch[3] == '=') {
                        lexer.currtok->type = TOK_ROR_EQUAL;
                        lexer.currtok->len = 4;
                        lexer.currch += 3;
                    }
                    lexer_accept_3chr(&lexer, TOK_ROR);
                    break;
                }
                lexer_accept_2chr(&lexer, TOK_SHR);
                break;
            }
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_GREATER_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_GREATER;
            break;
        case '<':
            if (*next == '<') {
                if (lexer.currch[2] == '=') {
                    lexer_accept_3chr(&lexer, TOK_SHL_EQUAL);
                    break;
                }
                if (lexer.currch[2] == '<') {
                    if (lexer.currch[3] == '=') {
                        lexer.currtok->type = TOK_ROL_EQUAL;
                        lexer.currtok->len = 4;
                        lexer.currch += 3;
                    }
                    lexer_accept_3chr(&lexer, TOK_ROL);
                    break;
                }
                lexer_accept_2chr(&lexer, TOK_SHL);
                break;
            }
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_LESS_EQUAL);
                break;
            } 
            lexer.currtok->type = TOK_LESS;
            break;
        case '^':
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_XOR_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_XOR;
            break;
        case '!': {
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_BANG_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_BANG;
            break;
        }
        case '=':
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_EQUAL_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_EQUAL;
            break;
        case '%':
            if (*next == '=') {
                lexer_accept_2chr(&lexer, TOK_PERCENT_EQUAL);
                break;
            }
            lexer.currtok->type = TOK_PERCENT;
            break;
        case '@':
            lexer.currtok->type = TOK_COMMERCIAL_AT;
            break;
        default: {
            const char *start = lexer.currch;
            if (lex_number(&lexer, &lexer.currtok->number)) {
                lexer.currtok->len = (int)(lexer.currch - start);
                lexer.currtok->type = TOK_NUM;
                vector_add(tokens, lexer.currtok);
                continue;
            }
            while (isalnum(*lexer.currch) || *lexer.currch == '_') lexer.currch++;
            lexer.currtok->len = (int)(lexer.currch - start);

            if (lexer.currtok->len == 0) {
                lexer_error("invalid tok", &lexer);
                my_free(lexer.currtok);
                continue;
            }

            lexer.currtok->type = TOK_IDENT;
            for (size_t i = 0; i < ARRAYLEN(keywords); i++) {
                if (token_is_str(lexer.currtok, keywords[i].name)) {
                    lexer.currtok->type = keywords[i].type;
                    break;
                }
            }
            vector_add(tokens, lexer.currtok);
            continue;
        }
        }

        // Single char tok
        lexer.currch++;
        vector_add(tokens, lexer.currtok);
    }
    return !lexer.error_occured;
}

void print_token_line(token *tok, bool underline_carret, token_stream *stream) {    
    if (!tok) return;
    char *linestart = tok->location.line_start;
    
    // Calculate line end
    char *line_end = tok->start;
    while (*line_end != '\n' && *line_end != '\0') line_end++;

    printf("%d | %.*s\n", tok->location.line_number, (int)(line_end - linestart), linestart);
    
    // Skip line number spaces and add '|'
    int line_width = snprintf(NULL, 0, "%d", tok->location.line_number);
    printf("%*s | ", line_width, "");
    
    // Skip line spaces
    char *skip_end = tok->start;
    int underline_len = tok->len;
    if (underline_carret) {
        skip_end += tok->len;
        underline_len = 1;
    }
    for (char *c = linestart; c < skip_end; ++c) {
        putchar(' ');
    }
    
    // Underline current tok
    for (int c = 0; c < underline_len; c++) putchar('^');
    putchar('\n');

    if (!tok->src || tok->src->location.line_start == tok->location.line_start) return;
    report(tok->src, DIAGNOSTIC_NOTE, false, stream, 
        "expanded from macro '%.*s' at this location", tok->src->len, tok->src->start);
}

void report(token *tok, diagnostic_level level, bool underline_carret, 
            token_stream *stream, char *fmt, ...)  {
    char buffer[1024];

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    char report_string[8];
    switch (level) {
    case DIAGNOSTIC_NOTE:    strcpy(report_string, "note");    break;
    case DIAGNOSTIC_WARNING: strcpy(report_string, "warning"); break;
    case DIAGNOSTIC_ERROR:  
        if (stream->error_occured) return;
        strcpy(report_string, "error");  
        stream->error_occured = true; 
        break;
    }

    if (!stream->last_location ||
        (&tok->location != stream->last_location && 
        stream->last_location->path_name != tok->location.path_name))
    {
        if (tok) printf("[%s:%d] ", tok->location.path_name, tok->location.line_number);
        stream->last_location = &tok->location;
    }
    printf("%s: %s\n", report_string, buffer);
    print_token_line(tok, underline_carret, stream);
}

token *advance_token(token_stream *stream) {
    if (!at_end(stream)) {
        stream->pos++;
    }
    return current_token(stream);
}

// Consume the current tok if currtok.type == type. Returns the consumed tok.
token *consume_token(token_type type, char *errmsg, token_stream *stream) {
    token *tok = current_token(stream);
    if (tok->type == type) advance_token(stream);
    else if (stream->pos > 0) {
        token *prevtok = prev_token(stream);
        report(prevtok, DIAGNOSTIC_ERROR, true, stream, "%s after '%.*s'", errmsg, prevtok->len, prevtok->start);
    } else ERROR(stream, errmsg);
    return tok;
}

// If currtok type == type -> advance
token *match_token(token_type type, token_stream *stream) {
    token *tok = current_token(stream);
    if (tok && tok->type == type) {
        advance_token(stream);
        return tok;
    }

    return NULL;
}

void stream_print(token_stream *stream) {
    for (int i = 0; i < stream->tokens.count; i++) {
        token *t = vector_get(&stream->tokens, i);
        printf("T%d = %.*s\n", i, t->len, t->start);
    }
}

void stream_free(token_stream *stream) {
    for (int i = 0; i < stream->tokens.count; i++) {
        token *t = vector_get(&stream->tokens, i);
        my_free(t);
    }
    vector_free(&stream->tokens);
}