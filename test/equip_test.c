/* Equipment/item integration test using only liboot's public ABI. */
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/liboot.h"

static void crash_handler(int sig) {
    void* frames[32];
    int count = backtrace(frames, 32);
    fprintf(stderr, "\n*** signal %d ***\n", sig);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(139);
}

static void debug_print(const char *message) {
    if (message != NULL) {
        puts(message);
    }
}

static int read_file(const char *path, uint8_t **outData, size_t *outSize) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;

    *outData = NULL;
    *outSize = 0u;
    if (file == NULL) {
        perror("rom");
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        (uintmax_t)length > SIZE_MAX || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    data = malloc((size_t)length);
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        free(data);
        return 0;
    }
    *outData = data;
    *outSize = (size_t)length;
    return 1;
}

static void print_state(const char* label, const struct OoTLinkState* state) {
    printf("%s: held=%d melee=%u f1=%08x f2=%08x anim=%.1f\n", label,
           state->heldItemAction, state->meleeWeaponState, state->stateFlags1,
           state->stateFlags2, state->animFrame);
}

static void tick_many(int32_t link, struct OoTLinkInputs* inputs, struct OoTLinkState* state, int count) {
    int i;
    for (i = 0; i < count; i++) {
        oot_link_tick(link, inputs, state, NULL);
    }
}

int main(int argc, char** argv) {
    size_t romSize = 0u;
    uint8_t* rom = NULL;
    int32_t link = -1;
    int i;
    int swordSwingSeen = 0;
    int initialized = 0;
    int exitCode = 1;
    struct OoTLinkInputs inputs = { 0 };
    struct OoTLinkState state = { 0 };
    struct OoTSurface ground[2] = {
        { 0, { { -1000, 0, -1000 }, { -1000, 0, 1000 }, { 1000, 0, 1000 } } },
        { 0, { { -1000, 0, -1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 } } },
    };

    signal(SIGSEGV, crash_handler);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <oot-rom.z64>\n", argv[0]);
        return 1;
    }

    if (!read_file(argv[1], &rom, &romSize)) {
        fprintf(stderr, "failed to read ROM\n");
        return 1;
    }

    oot_set_debug_print_function(debug_print);
    oot_global_init(rom, romSize, NULL);
    initialized = 1;
    free(rom);
    rom = NULL;
    oot_static_surfaces_load(ground, 2);
    link = oot_link_create(0, 0, 0);
    if (link < 0) {
        fprintf(stderr, "link_create failed\n");
        goto done;
    }

    inputs.camLookZ = 1.0f;
    oot_link_set_equipment(link, OOT_SWORD_MASTER, OOT_SHIELD_HYLIAN,
                           OOT_TUNIC_KOKIRI, OOT_BOOTS_KOKIRI);
    tick_many(link, &inputs, &state, 20);
    print_state("equipped", &state);

    inputs.buttonB = 1;
    oot_link_tick(link, &inputs, &state, NULL);
    print_state("sword-enter", &state);
    inputs.buttonB = 0;
    for (i = 0; i < 40; i++) {
        oot_link_tick(link, &inputs, &state, NULL);
        if (!swordSwingSeen && state.meleeWeaponState != 0) {
            swordSwingSeen = 1;
            printf("sword hit window entered at post-B tick %d\n", i + 1);
            print_state("sword-swing", &state);
        }
    }
    print_state("sword-done", &state);
    if (!swordSwingSeen) {
        fprintf(stderr, "sword hit window was not entered\n");
        exitCode = 2;
        goto done;
    }

    oot_link_use_item(link, OOT_ITEM_OCARINA);
    tick_many(link, &inputs, &state, 40);
    print_state("ocarina-hold", &state);

    oot_link_use_item(link, OOT_ITEM_NONE);
    tick_many(link, &inputs, &state, 20);
    print_state("ocarina-exit", &state);

    puts("done");
    exitCode = 0;

done:
    if (link >= 0) {
        oot_link_delete(link);
    }
    if (initialized) {
        oot_global_terminate();
    }
    free(rom);
    return exitCode;
}
