#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sprk32/backend.h"
#include "sprk32/emulator.h"
#include "utils.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

typedef enum {
    CMD_UNKNOWN,
    CMD_COMPILE,
    CMD_EMULATE,
    CMD_DEBUG,
    CMD_SET,
    CMD_OPEN,
    CMD_DISASM,
    CMD_ISOK, // Checks if there is an error and if so stops the program
    CMD_EXIT,
} cmd_type;
typedef struct {
    cmd_type type;
    char *lhs;
    char *rhs;
    uint32_t number;
    bool error_occured;
} command;

static command parse_line(const char *line) {
    command cmd = {CMD_UNKNOWN};

    char *line_copy = string_duplicate(line, strlen(line));
    if (!line_copy) return cmd;

    line_copy[strcspn(line_copy, "\r\n")] = 0;

    char **tokens = NULL;
    int token_count = 0;

    char *saveptr = NULL;
    char *token = string_tokenize(line_copy, " ", &saveptr);
    bool in_comment = false;
    while (token != NULL) {
        if (!strncmp(token, "//", 2)) break;

        tokens = my_realloc(tokens, sizeof(char*) * (token_count + 1));
        tokens[token_count] = string_duplicate(token, strlen(token));
        token_count++;

        token = string_tokenize(NULL, " ", &saveptr);
    }

    if (token_count == 0) {
        my_free(line_copy);
        return cmd;
    }

    if (token_count == 1) {
        if (!strcmp(tokens[0], "emulate")) {
            cmd.type = CMD_EMULATE;
            goto cleanup;
        }
        if (!strcmp(tokens[0], "debug")) {
            cmd.type = CMD_DEBUG;
            goto cleanup;
        }
        if (!strcmp(tokens[0], "isok?")) {
            cmd.type = CMD_ISOK;
            goto cleanup;
        }
        if (!strcmp(tokens[0], "exit")) {
            cmd.type = CMD_EXIT;
            goto cleanup;
        }
    }
    if (token_count == 2 && !strcmp(tokens[0], "open")) {
        cmd.type = CMD_OPEN;
        cmd.lhs = string_duplicate(tokens[1], strlen(tokens[1]));
        goto cleanup;
    }
    if (token_count == 3 && !strcmp(tokens[1], "=")) {
        cmd.type = CMD_SET;
        cmd.lhs = string_duplicate(tokens[0], strlen(tokens[0]));
        cmd.rhs = string_duplicate(tokens[2], strlen(tokens[2]));
        goto cleanup;
    }
    if (token_count >= 2) {
        if (!strcmp(tokens[0], "compile")) {
            cmd.type = CMD_COMPILE;
            cmd.lhs = string_duplicate(tokens[1], strlen(tokens[1]));

            int last_token = 2;
            if (token_count >= 4 && !strcmp(tokens[2], "to")) {
                cmd.rhs = string_duplicate(tokens[3], strlen(tokens[3]));

                if (token_count == 6 && !strcmp(tokens[4], "as")) {
                    char *endptr = NULL;
                    cmd.number = strtoul(tokens[5], &endptr, 0);
                    if (endptr != tokens[5] + strlen(tokens[5])) {
                        printf("[SPRK32] error: '%s' is not a valid number\n", tokens[5]);
                    }
                    last_token = 6;
                } else {
                    cmd.number = KB;
                    last_token = 4;
                }
            }
            if (token_count != last_token) {
                printf("[SPRK32] error: invalid 'compile' syntax\n");
                cmd.error_occured = true;
            }
            goto cleanup;
        }

        if (token_count <= 3 && !strcmp(tokens[0], "disasm")) {
            cmd.type = CMD_DISASM;
            cmd.lhs = string_duplicate(tokens[1], strlen(tokens[1]));
            if (token_count == 3)
                cmd.rhs = string_duplicate(tokens[2], strlen(tokens[2]));
            goto cleanup;
        }
    }

    printf("[SPRK32] error: unknown or invalid command syntax.\n");
    cmd.error_occured = true;
cleanup:
    for (int i = 0; i < token_count; i++) {
        my_free(tokens[i]);
    }
    my_free(tokens);
    my_free(line_copy);
    return cmd;
}

typedef struct {
    vector labels;
    sprk32_emitter emitter;
    arch_backend backend;
    bool running;
    bool error;

    char *rom_path;
    char *disk_path;
} sprk32_cli;

static int emulator_run(const char *rom_path, const char *disk_path, bool debug_mode);
static void spawn_cli(const char *file_path);
static void execute_command(sprk32_cli *cli, command *cmd) {
    if (cmd->error_occured) {
        my_free(cmd->lhs);
        my_free(cmd->rhs);
        return;
    }

    bool debug_mode = false;
    switch (cmd->type) {
    case CMD_EXIT: cli->running = false; break;
    case CMD_OPEN: {
        printf("[SPRK32] opening '%s'\n", cmd->lhs);
        spawn_cli(cmd->lhs);
        break;
    }
    case CMD_DISASM: {
        printf("[SPRK32] disassembling '%s'\n", cmd->lhs);
        size_t size;
        uint8_t *buffer = file_to_buffer(cmd->lhs, &size);

        FILE *out_file = (cmd->rhs) ? file_open(cmd->rhs, "w") : stdout;
        if (buffer && out_file && !sprk32_disassemble(cmd->lhs, out_file, buffer, size))
            cli->error = true;
        my_free(cmd->lhs);
        my_free(cmd->rhs);
        my_free(buffer);

        if (cmd->rhs) fclose(out_file);
        break;
    }
    case CMD_ISOK:
        printf("[SPRK32] is ok? %s\n", !cli->error ? "yes" : "no");
        if (cli->error) cli->running = false;
        break;
    case CMD_COMPILE: {
        printf("[SPRK32] compiling '%s'", cmd->lhs);
        if (cmd->rhs) printf(" to '%s'\n", cmd->rhs);
        else {
            printf(" -> (no destination specified, using default)\n");
            // dst = [source_name - .ext].bin
            const char *source_dot = strchr(cmd->lhs, '.');
            size_t len = (source_dot != NULL) ? (size_t)(source_dot - cmd->lhs) : strlen(cmd->lhs);

            cmd->rhs = my_malloc(len + strlen(".bin") + 1);
            memcpy(cmd->rhs, cmd->lhs, len);
            cmd->rhs[len] = '\0';
            strcat(cmd->rhs, ".bin");
        }

        // Compile source
        vector files; vector_init(&files);
        source_file *source = file_new(&files, cmd->lhs);

        cli->emitter.out_size = cmd->number;
        cli->emitter.out = my_malloc(cli->emitter.out_size);
        if (!compile(&files, &cli->backend, &cli->labels))
            cli->error = true;

        FILE *out_file = file_open(cmd->rhs, "w");
        my_free(cmd->rhs);
        if (out_file) {
            fwrite(cli->emitter.out, 1, cli->emitter.out_size, out_file);
            fclose(out_file);
        }
        my_free(cli->emitter.out);
        // Free files
        for (int i = 0; i < files.count; i++) {
            source_file *file = vector_get(&files, i);
            source_free(file);
        }
        vector_free(&files);
        break;
    }
    case CMD_DEBUG:
        debug_mode = true;
    case CMD_EMULATE:
        printf("[SPRK32] starting emulation...\n");
        emulator_run(cli->rom_path, cli->disk_path, debug_mode);
        cli->running = false;
        break;
    case CMD_SET: {
        struct { const char *name; char **path; } map[] = {
            { "rom",  &cli->rom_path },
            { "disk", &cli->disk_path },
        };

        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (!strcmp(cmd->lhs, map[i].name)) {
                printf("[SPRK32] setting %s to '%s'\n", cmd->lhs, cmd->rhs);
                my_free(cmd->lhs);

                my_free(*map[i].path); // Free old path
                *map[i].path = cmd->rhs;
                return;
            }
        }

        cli->error = true;
        printf("[SPRK32] error: setting '%s' not found\n", cmd->lhs);
        break;
    }
    case CMD_UNKNOWN: cli->error = true; break;
    }
}

static const char *default_rom_path = "rom.bin";
static const char *default_disk_path = "disk.bin";

static void execute_file(sprk32_cli *cli, FILE *file) {
    if (!file) return;
    while (cli->running) {
        char *line = file_read_line(file);
        if (!line) break;

        command cmd = parse_line(line);
        execute_command(cli, &cmd);
        my_free(line);
    }
}
static void spawn_cli(const char *file_path) {
    size_t rom_path_len = strlen(default_rom_path) + 1;
    size_t disk_path_len = strlen(default_disk_path) + 1;
    sprk32_cli cli = {
        .rom_path = my_malloc(rom_path_len),
        .disk_path = my_malloc(disk_path_len),
        .running = true,
        .error = false,
    };

    string_copy(cli.rom_path, default_rom_path, rom_path_len);
    string_copy(cli.disk_path, default_disk_path, disk_path_len);
    vector_init(&cli.labels);
    sprk32_backend_init(&cli.backend, &cli.emitter);

    FILE *f = fopen(file_path ? file_path : "sprk32.cfg", "r");
    execute_file(&cli, f);
    if (file_path) goto cleanup;

    while (cli.running) {
        printf("sprk32> ");
        fflush(stdout);

        char *input = read_line();
        if (strlen(input) > 0) {
            command cmd = parse_line(input);
            execute_command(&cli, &cmd);
        } else cli.running = false;

        my_free(input);
    }

cleanup:
    my_free(cli.disk_path);
    my_free(cli.rom_path);
    labels_free(&cli.labels);
}

typedef struct {
    SDL_FRect screen_rect;
    SDL_Texture *screen;
    SDL_Window *window;
    SDL_Renderer *renderer;
} emu_ctx;

#define COLOR_CHANNELS 3
#define TOTAL_COLORS 256
static void render(emu_ctx *ctx, emu_vpu *vpu) {
    // Present the screen buffer
    void *pixels;
    int pitch;

    if (SDL_LockTexture(ctx->screen, NULL, &pixels, &pitch)) {
        for (int i = 0; i < SCREEN_SIZE; i++) {
            uint8_t color = vpu->source[i];

            uint8_t rgb[COLOR_CHANNELS];
            for (int i = 0; i < COLOR_CHANNELS; i++) {
                rgb[i] = color;
            }
            memcpy(pixels + (i * COLOR_CHANNELS), rgb, COLOR_CHANNELS);
        }

        SDL_UnlockTexture(ctx->screen);
    }

    // Get the window size
    int win_w, win_h;
    SDL_GetWindowSize(ctx->window, &win_w, &win_h);

    // Calculate the target rectangle for the texture
    int scale_x = win_w / SCREEN_WIDTH;
    int scale_y = win_h / SCREEN_HEIGHT;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1) scale = 1;

    int dest_w = SCREEN_WIDTH * scale;
    int dest_h = SCREEN_HEIGHT * scale;
    int offset_x = (win_w - dest_w) / 2;
    int offset_y = (win_h - dest_h) / 2;

    ctx->screen_rect.x = offset_x;
    ctx->screen_rect.y = offset_y;
    ctx->screen_rect.w = dest_w;
    ctx->screen_rect.h = dest_h;

    // Clear the renderer
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);
    SDL_RenderClear(ctx->renderer);

    // Render the texture
    SDL_RenderTexture(ctx->renderer, ctx->screen, NULL, &ctx->screen_rect);
    SDL_RenderPresent(ctx->renderer);
}
static int emulator_run(const char *rom_path, const char *disk_path, bool debug_mode) {
    // Init rom
    size_t rom_size = 0;
    uint8_t *rom = file_to_buffer(rom_path, &rom_size);
    if (rom_size != ROM_SIZE) {
        printf("Error: '%s' does not satisfy the expected size (found %lu, expected %d bytes)\n",
            rom_path, rom_size, ROM_SIZE);
        my_free(rom);
        return EXIT_FAILURE;
    }

    // Init floppy disk
    uint8_t *disk = my_malloc(FDC_DISK_SIZE);
    uint8_t *dirty_disk = my_malloc(FDC_DISK_SIZE);
    FILE *disk_file = fopen(disk_path, "wb");

    emulator sprk32 = emu_init(rom, disk, debug_mode);

    // Init app
    emu_ctx ctx = {};
    SDL_SetAppMetadata("SPRK32", "1.0", "com.retrogic.sprk32");

    // Init video
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return EXIT_FAILURE;
    }

    // Create the window
    if (!SDL_CreateWindowAndRenderer("SPRK32", SCREEN_WIDTH * 4,
                    SCREEN_HEIGHT * 4, SDL_WINDOW_RESIZABLE,
                    &ctx.window, &ctx.renderer)) {
        SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
        return EXIT_FAILURE;
    }

    // Create the texture
    ctx.screen = SDL_CreateTexture(ctx.renderer, SDL_PIXELFORMAT_RGB24,
                    SDL_TEXTUREACCESS_STREAMING,
                    SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!ctx.screen) {
        SDL_Log("Couldn't create the screen texture: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    SDL_SetTextureScaleMode(ctx.screen, SDL_SCALEMODE_NEAREST);

    uint64_t last_time = now_ns();
    double cycle_accumulator = 0.0;

    uint64_t sec_cycles = 0;
    uint64_t sec_time = 0;
    uint64_t instr_per_sec = 0;

    uint64_t next_frame_time = now_ns() + NS_PER_FRAME;
    uint64_t frame_counter = 0;

    bool run = true;
    uint64_t cpu_exec_time_ns = 0;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_KEY_UP:
                switch (e.key.key) {
                case SDLK_W: case SDLK_UP: break;
                case SDLK_S: case SDLK_DOWN: break;
                case SDLK_A: case SDLK_LEFT: break;
                case SDLK_D: case SDLK_RIGHT: break;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                switch (e.key.key) {
                case SDLK_ESCAPE:
                    set_flag(&sprk32.cpu, FLAG_HALTED, true);
                    run = false;
                    break;
                default: break;
                }
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_QUIT:
                set_flag(&sprk32.cpu, FLAG_HALTED, true);
                run = false;
                break;
            default: break;
            }
        }

        uint64_t now = now_ns();
        uint64_t delta_ns = now - last_time;
        if (delta_ns > NS_PER_FRAME) delta_ns = NS_PER_FRAME;
        last_time = now;

        double cycles_to_run = (double)delta_ns / NS_PER_SEC * CPU_HZ;
        cycle_accumulator += cycles_to_run;

        while (cycle_accumulator >= 1.0) {
            uint64_t start = now_ns();

            uint64_t start_cycles = sprk32.cpu.cycles;
            if (!dma_step(&sprk32.dma, &sprk32.cpu.cycles))
                cpu_step(&sprk32.cpu);

            uint64_t used = sprk32.cpu.cycles - start_cycles;
            cpu_exec_time_ns += now_ns() - start;

            cycle_accumulator -= used;
            sec_cycles += used;
            instr_per_sec++;

            vpu_tick(&sprk32.vpu, used);

            if (get_flag(&sprk32.cpu, FLAG_HALTED)) {
                run = false;
                break;
            }
        }

        if (sprk32.vpu.frame_completed) {
            sprk32.vpu.frame_completed = false;
            render(&ctx, &sprk32.vpu);
            frame_counter++;

            uint64_t time_now = now_ns();
            if (time_now < next_frame_time) {
                uint64_t time_to_wait = next_frame_time - time_now;
                wait_ns(time_to_wait);
            }
            next_frame_time += NS_PER_FRAME;
        }

        sec_time += delta_ns;
        if (sec_time >= NS_PER_SEC) {
            for (int i = 0; i < FDC_DISK_SIZE; i++) {
                if (dirty_disk[i]) {
                    fseek(disk_file, i, SEEK_SET);
                    fwrite(&disk[i], 1, 1, disk_file);
                    dirty_disk[i] = 0;
                }
            }
            fflush(disk_file);

            printf("[CPU] fps: %llu | instr/sec: %llu | clock: %.4f MHz\n", frame_counter, instr_per_sec, (double)sec_cycles / HZ_PER_MHZ);
            instr_per_sec = 0;
            sec_cycles = 0;
            sec_time -= NS_PER_SEC;
            frame_counter = 0;
        }
    }
    printf("[CPU] execution time: %.6f s (%llu ns)\n",
            (double)cpu_exec_time_ns / NS_PER_SEC,
            (unsigned long long)cpu_exec_time_ns);

    my_free(disk);
    my_free(dirty_disk);

    // Free emulator
    emu_free(&sprk32);
    SDL_DestroyTexture(ctx.screen);
    SDL_DestroyRenderer(ctx.renderer);
    SDL_DestroyWindow(ctx.window);

    SDL_Quit();
    return true;
}

int main() {
    spawn_cli(NULL);

    size_t allocations = get_allocations();
    if (allocations > 0) printf("Memory leaks: %zu\n", allocations);
    return 0;
}