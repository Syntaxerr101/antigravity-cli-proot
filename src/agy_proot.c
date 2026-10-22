#include <asm/hwcap.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#endif

#ifndef AGY_VERSION
#define AGY_VERSION "1.0.2"
#endif

enum update_check_mode {
    UPDATE_CHECK_EXPLICIT,
    UPDATE_CHECK_STARTUP,
};

/* ------------------------------------------------------------------------- *
 * Runtime environment detection
 *
 * This bootstrapper is the PRoot-only build of the Antigravity CLI standalone
 * port.  It targets a glibc userland:
 *
 *   - a PRoot Debian/Ubuntu root running on top of Termux on Android, or
 *   - any native aarch64 glibc Linux host.
 *
 * The engine is an upstream Google binary that assumes a 48-bit virtual
 * address space.  Android userland only provides 39 bits, so the VA39 memory
 * patches are applied to the engine before it is installed.
 * ------------------------------------------------------------------------- */

enum agy_runtime {
    AGY_RUNTIME_UNKNOWN = 0,
    AGY_RUNTIME_PROOT,
};

struct agy_environment {
    enum agy_runtime runtime;
    const char *label;
    char loader[PATH_MAX];
    char lib_path[PATH_MAX];
    char cert_path[PATH_MAX];
    char resolv_path[PATH_MAX];
    char qemu_path[PATH_MAX];
};

/* Official upstream manifest used by the PRoot self-updater.  The URL can be
   overridden at compile time so the update path can be tested against a
   mirror. */
#ifndef AGY_MANIFEST_URL
#define AGY_MANIFEST_URL                                                                           \
    "https://antigravity-cli-auto-updater-974169037036.us-central1.run.app/manifests/"             \
    "linux_arm64.json"
#endif

static const char *const AGY_PROOT_LOADER_CANDIDATES[] = {
    "/lib/ld-linux-aarch64.so.1",
    "/lib64/ld-linux-aarch64.so.1",
    "/usr/lib/ld-linux-aarch64.so.1",
    "/usr/lib64/ld-linux-aarch64.so.1",
    NULL,
};

static const char *const AGY_PROOT_LIB_CANDIDATES[] = {
    "/usr/lib/aarch64-linux-gnu",
    "/lib/aarch64-linux-gnu",
    "/usr/lib64",
    "/lib64",
    "/usr/lib",
    "/lib",
    NULL,
};

static const char *const AGY_PROOT_CERT_CANDIDATES[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/certs/ca-bundle.crt",
    "/etc/ssl/cert.pem",
    NULL,
};

static const char *const AGY_PROOT_QEMU_CANDIDATES[] = {
    "/usr/bin/qemu-aarch64",
    "/usr/local/bin/qemu-aarch64",
    "/bin/qemu-aarch64",
    NULL,
};

static int path_exists(const char *path) {
    return path != NULL && path[0] != '\0' && access(path, F_OK) == 0;
}

static int path_readable(const char *path) {
    return path != NULL && path[0] != '\0' && access(path, R_OK) == 0;
}

/* Portable substring search (bionic has no memmem). */
static const char *buffer_contains(const unsigned char *buffer, size_t length, const char *needle) {
    size_t needle_length = strlen(needle);

    if (needle_length == 0 || length < needle_length) {
        return NULL;
    }
    for (size_t index = 0; index + needle_length <= length; index++) {
        if (memcmp(buffer + index, needle, needle_length) == 0) {
            return (const char *)(buffer + index);
        }
    }

    return NULL;
}

/* A glibc dynamic loader exports GLIBC_x.y version symbols; Android's bionic
   linker never does, which makes this a reliable discriminator. */
static int is_glibc_loader(const char *path) {
    if (!path_readable(path)) {
        return 0;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return 0;
    }
    errno = 0;
    long size = ftell(file);
    if (size <= 0 || errno != 0) {
        (void)fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return 0;
    }

    unsigned char *buffer = malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return 0;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    (void)fclose(file);

    int found = buffer_contains(buffer, read, "GLIBC_2.") != NULL;
    free(buffer);

    return found;
}

static const char *find_glibc_loader(void) {
    for (int index = 0; AGY_PROOT_LOADER_CANDIDATES[index] != NULL; index++) {
        if (is_glibc_loader(AGY_PROOT_LOADER_CANDIDATES[index])) {
            return AGY_PROOT_LOADER_CANDIDATES[index];
        }
    }

    return NULL;
}

static const char *find_first_existing(const char *const *candidates) {
    for (int index = 0; candidates[index] != NULL; index++) {
        if (path_exists(candidates[index])) {
            return candidates[index];
        }
    }

    return NULL;
}

/* Join the existing library directories into a colon separated search path. */
static void build_library_path(char *out, size_t out_size) {
    out[0] = '\0';
    size_t used = 0;

    for (int index = 0; AGY_PROOT_LIB_CANDIDATES[index] != NULL; index++) {
        const char *candidate = AGY_PROOT_LIB_CANDIDATES[index];
        if (!path_exists(candidate)) {
            continue;
        }
        size_t length = strlen(candidate);
        size_t needed = length + (used > 0 ? 1 : 0);
        if (used + needed + 1 > out_size) {
            break;
        }
        if (used > 0) {
            out[used++] = ':';
        }
        memcpy(out + used, candidate, length);
        used += length;
        out[used] = '\0';
    }
}

/* Fill in the runtime specific loader, library, certificate and resolver
   locations.  Returns 0 when the host cannot be used. */
static int resolve_environment(struct agy_environment *environment) {
    memset(environment, 0, sizeof(*environment));

    const char *proot_loader = find_glibc_loader();
    if (proot_loader == NULL) {
        environment->runtime = AGY_RUNTIME_UNKNOWN;
        environment->label = "unknown";
        return 0;
    }

    environment->runtime = AGY_RUNTIME_PROOT;
    environment->label = "proot";

    (void)snprintf(environment->loader, sizeof(environment->loader), "%s", proot_loader);
    build_library_path(environment->lib_path, sizeof(environment->lib_path));

    const char *cert = find_first_existing(AGY_PROOT_CERT_CANDIDATES);
    if (cert == NULL) {
        return 0;
    }
    (void)snprintf(environment->cert_path, sizeof(environment->cert_path), "%s", cert);
    (void)snprintf(environment->resolv_path, sizeof(environment->resolv_path), "/etc/resolv.conf");

    const char *qemu = find_first_existing(AGY_PROOT_QEMU_CANDIDATES);
    if (qemu != NULL) {
        (void)snprintf(environment->qemu_path, sizeof(environment->qemu_path), "%s", qemu);
    } else {
        environment->qemu_path[0] = '\0';
    }

    return 1;
}

struct semantic_version {
    unsigned long core[3];
    const char *prerelease;
    size_t prerelease_length;
};

struct version_identifier {
    const char *start;
    size_t length;
    int numeric;
};

static int validate_version_identifier(int enforce_numeric_leading_zero, const char *start,
                                       size_t length) {
    int numeric = 1;

    if (length == 0) {
        return 0;
    }

    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)start[index];
        if (!isalnum(character) && character != '-') {
            return 0;
        }
        if (!isdigit(character)) {
            numeric = 0;
        }
    }

    return !(enforce_numeric_leading_zero && numeric && length > 1 && start[0] == '0');
}

static int validate_identifier_list(int enforce_numeric_leading_zero, const char *start,
                                    size_t length) {
    size_t identifier_start = 0;

    if (length == 0) {
        return 0;
    }

    for (;;) {
        size_t identifier_end = identifier_start;
        while (identifier_end < length && start[identifier_end] != '.') {
            identifier_end++;
        }

        if (!validate_version_identifier(enforce_numeric_leading_zero, start + identifier_start,
                                         identifier_end - identifier_start)) {
            return 0;
        }
        if (identifier_end == length) {
            return 1;
        }
        identifier_start = identifier_end + 1;
    }
}

static int parse_core_component(const char **cursor, unsigned long *value) {
    if (!isdigit((unsigned char)**cursor)) {
        return 0;
    }
    if (**cursor == '0' && isdigit((unsigned char)(*cursor)[1])) {
        return 0;
    }

    errno = 0;
    char *end = NULL;
    *value = strtoul(*cursor, &end, 10);
    if (errno == ERANGE || end == *cursor) {
        return 0;
    }

    *cursor = end;
    return 1;
}

static int parse_semantic_version(const char *text, struct semantic_version *version) {
    const char *cursor = text;

    memset(version, 0, sizeof(*version));
    if (*cursor == 'v') {
        cursor++;
    }

    for (size_t component = 0; component < 3; component++) {
        if (!parse_core_component(&cursor, &version->core[component])) {
            return 0;
        }
        if (component == 2) {
            break;
        }
        if (*cursor != '.') {
            return 0;
        }
        cursor++;
    }

    if (*cursor == '-') {
        const char *prerelease_start = ++cursor;
        while (*cursor != '\0' && *cursor != '+') {
            cursor++;
        }
        version->prerelease = prerelease_start;
        version->prerelease_length = (size_t)(cursor - prerelease_start);
        if (!validate_identifier_list(1, version->prerelease, version->prerelease_length)) {
            return 0;
        }
    }

    if (*cursor == '+') {
        const char *build_start = ++cursor;
        while (*cursor != '\0') {
            cursor++;
        }
        if (!validate_identifier_list(0, build_start, (size_t)(cursor - build_start))) {
            return 0;
        }
    }

    return *cursor == '\0';
}

static struct version_identifier next_version_identifier(const char **cursor, size_t *remaining) {
    struct version_identifier identifier = {
        .start = *cursor,
        .length = 0,
        .numeric = 1,
    };

    while (identifier.length < *remaining && (*cursor)[identifier.length] != '.') {
        if (!isdigit((unsigned char)(*cursor)[identifier.length])) {
            identifier.numeric = 0;
        }
        identifier.length++;
    }

    size_t consumed = identifier.length;
    if (consumed < *remaining) {
        consumed++;
    }
    *cursor += consumed;
    *remaining -= consumed;
    return identifier;
}

static int compare_version_identifiers(const struct version_identifier *candidate,
                                       const struct version_identifier *installed) {
    if (candidate->numeric != installed->numeric) {
        return candidate->numeric ? -1 : 1;
    }

    if (candidate->numeric && candidate->length != installed->length) {
        return candidate->length < installed->length ? -1 : 1;
    }

    size_t common_length =
        candidate->length < installed->length ? candidate->length : installed->length;
    int lexical = memcmp(candidate->start, installed->start, common_length);
    if (lexical != 0) {
        return lexical < 0 ? -1 : 1;
    }
    if (candidate->length == installed->length) {
        return 0;
    }

    return candidate->length < installed->length ? -1 : 1;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int compare_prerelease_versions(const struct semantic_version *candidate,
                                       const struct semantic_version *installed) {
    if (candidate->prerelease_length == 0 || installed->prerelease_length == 0) {
        if (candidate->prerelease_length == installed->prerelease_length) {
            return 0;
        }
        return candidate->prerelease_length == 0 ? 1 : -1;
    }

    const char *candidate_cursor = candidate->prerelease;
    const char *installed_cursor = installed->prerelease;
    size_t candidate_remaining = candidate->prerelease_length;
    size_t installed_remaining = installed->prerelease_length;

    while (candidate_remaining > 0 && installed_remaining > 0) {
        struct version_identifier candidate_identifier =
            next_version_identifier(&candidate_cursor, &candidate_remaining);
        struct version_identifier installed_identifier =
            next_version_identifier(&installed_cursor, &installed_remaining);
        int comparison = compare_version_identifiers(&candidate_identifier, &installed_identifier);
        if (comparison != 0) {
            return comparison;
        }
    }

    if (candidate_remaining == installed_remaining) {
        return 0;
    }
    return candidate_remaining == 0 ? -1 : 1;
}

// Returns -1 when candidate is older, 0 when equal, 1 when newer, and -2 when invalid.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int compare_release_versions(const char *candidate_text, const char *installed_text) {
    struct semantic_version candidate;
    struct semantic_version installed;

    if (!parse_semantic_version(candidate_text, &candidate) ||
        !parse_semantic_version(installed_text, &installed)) {
        return -2;
    }

    for (size_t component = 0; component < 3; component++) {
        if (candidate.core[component] != installed.core[component]) {
            return candidate.core[component] < installed.core[component] ? -1 : 1;
        }
    }

    return compare_prerelease_versions(&candidate, &installed);
}

static void print_update_usage(void) {
    printf("Usage: agy update [options]\n\n"
           "Options:\n"
           "  -y, --yes, --auto  Apply updates without prompting\n"
           "  -h, --help         Show this help message\n\n"
           "Environment:\n"
           "  AGY_AUTO_UPDATE=1   Apply updates without prompting\n"
           "  AGY_UPDATE_DEBUG=1  Show startup update-check errors on stderr\n");
}

static int env_var_enabled(const char *name) {
    const char *value = getenv(name);

    return value != NULL && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
}

static void report_update_check_error(enum update_check_mode mode, const char *message) {
    if (mode == UPDATE_CHECK_EXPLICIT) {
        printf("[agy-proot] Error: %s\n", message);
    } else if (env_var_enabled("AGY_UPDATE_DEBUG")) {
        (void)fprintf(stderr, "[agy-proot] Automatic update check failed: %s\n", message);
    }
}

static int should_perform_update(int auto_update) {
    if (auto_update) {
        printf("[agy-proot] Proceeding with automatic update (non-interactive)...\n");
        return 1;
    }

    if (!isatty(STDIN_FILENO)) {
        printf("[agy-proot] Error: standard input is not a TTY and auto-update is not enabled.\n");
        printf("[agy-proot] Run `agy update -y` or set AGY_AUTO_UPDATE=1 for non-interactive "
               "updates.\n");
        return 0;
    }

    for (;;) {
        printf("[agy-proot] Would you like to update now? [Y/n]: ");
        (void)fflush(stdout);

        char response_line[64] = {0};
        if (fgets(response_line, sizeof(response_line), stdin) == NULL) {
            return 0;
        }
        if (strchr(response_line, '\n') == NULL) {
            int ch = 0;
            while ((ch = getchar()) != '\n' && ch != EOF) {
            }
        }

        if (response_line[0] == '\n' || response_line[0] == '\0') {
            return 1;
        }
        if (response_line[0] == 'y' || response_line[0] == 'Y') {
            return 1;
        }
        if (response_line[0] == 'n' || response_line[0] == 'N') {
            return 0;
        }

        printf("[agy-proot] Invalid selection. Enter y or n.\n");
    }
}

static void check_and_perform_proot_update(enum update_check_mode mode, const char *dir,
                                           int auto_update);

// Query this fork's latest release and update the installed twin binaries in place.
/* Update the installed engine from the official Antigravity CLI manifest. */
static void check_and_perform_update(enum update_check_mode mode, const char *dir, int auto_update,
                                     const struct agy_environment *environment) {
    (void)environment;
    check_and_perform_proot_update(mode, dir, auto_update);
}

/* ------------------------------------------------------------------------- *
 * VA39 memory-layout patches
 *
 * This mirrors proot/patch_va39.py and the inline patcher used by the upstream
 * Termux build.sh, but runs in-process so the PRoot self-updater can patch a
 * freshly downloaded engine without depending on Python being installed.
 * ------------------------------------------------------------------------- */

struct va39_word_rewrite {
    unsigned int from;
    unsigned int to;
};

static const struct va39_word_rewrite AGY_VA39_WORD_REWRITES[] = {
    {0xD2C20009, 0xD2C00409}, {0xD2C2000A, 0xD2C0040A}, {0xF2C20008, 0xF2DFF408},
    {0xF2C20009, 0xF2DFF409}, {0xD2C10009, 0xD2C00209}, {0xD2C1000A, 0xD2C0020A},
    {0xF2C38008, 0xF2DFF708}, {0xF2C38009, 0xF2DFF709}, {0x92560A6C, 0x925D0A6C},
    {0x92560A6A, 0x925D0A6A}, {0xD2C3000D, 0xD2C0060D}, {0xD2C3000C, 0xD2C0060C},
    {0xD2C08008, 0xD2C00108},
};

struct va39_counts {
    unsigned int ubfx;
    unsigned int lsl;
    unsigned int mask;
    unsigned int mmap;
    unsigned int word_rewrites;
    unsigned int faccessat2;
};

static unsigned int read_u32(const unsigned char *pointer) {
    return (unsigned int)pointer[0] | ((unsigned int)pointer[1] << 8) |
           ((unsigned int)pointer[2] << 16) | ((unsigned int)pointer[3] << 24);
}

static void write_u32(unsigned char *pointer, unsigned int value) {
    pointer[0] = (unsigned char)(value & 0xFF);
    pointer[1] = (unsigned char)((value >> 8) & 0xFF);
    pointer[2] = (unsigned char)((value >> 16) & 0xFF);
    pointer[3] = (unsigned char)((value >> 24) & 0xFF);
}

static void patch_va39_buffer(unsigned char *data, size_t size, struct va39_counts *counts) {
    memset(counts, 0, sizeof(*counts));

    /* Pass 1: retarget the UBFX immediate fields that drive TCMalloc's
       aligned mapping arithmetic to a 39-bit address space. */
    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
        unsigned int word = read_u32(data + offset);
        if ((word & 0x7F800000U) != 0x53000000U) {
            continue;
        }
        unsigned int immr = (word >> 16) & 0x3FU;
        unsigned int imms = (word >> 10) & 0x3FU;
        unsigned int mask = word & ~(0x3FU << 16) & ~(0x3FU << 10);
        if (immr == 42 && imms == 44) {
            write_u32(data + offset, mask | (35U << 16) | (37U << 10));
            counts->ubfx++;
        } else if (immr == 22 && imms == 21) {
            write_u32(data + offset, mask | (29U << 16) | (28U << 10));
            counts->lsl++;
        }
    }

    /* Pass 2: relax the page-alignment bitmask sequence. */
    for (size_t offset = 0; offset + 8 <= size; offset += 4) {
        if (read_u32(data + offset) == 0x92D3800AU && read_u32(data + offset + 4) == 0xF2E0000AU) {
            write_u32(data + offset, 0x9280000AU);
            write_u32(data + offset + 4, 0xD35DFD4AU);
            counts->mask++;
        }
    }

    /* Pass 3: retarget the mmap length constant. */
    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
        if (read_u32(data + offset) == 0xF2E00029U) {
            write_u32(data + offset, 0xD3596129U);
            counts->mmap++;
        }
    }

    /* Pass 4: unconditional instruction rewrites. */
    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
        unsigned int word = read_u32(data + offset);
        for (size_t index = 0;
             index < sizeof(AGY_VA39_WORD_REWRITES) / sizeof(AGY_VA39_WORD_REWRITES[0]); index++) {
            if (word == AGY_VA39_WORD_REWRITES[index].from) {
                write_u32(data + offset, AGY_VA39_WORD_REWRITES[index].to);
                counts->word_rewrites++;
                break;
            }
        }
    }

    /* Pass 5: neutralise the faccessat2 wrapper so it stays available on
       kernels that do not implement the syscall. */
    for (size_t offset = 0; offset + 16 <= size; offset += 4) {
        if (read_u32(data + offset) == 0xAA1F03E5U && read_u32(data + offset + 4) == 0xAA1F03E6U &&
            read_u32(data + offset + 8) == 0xD28036E0U &&
            (read_u32(data + offset + 12) & 0xFC000000U) == 0x94000000U) {
            write_u32(data + offset + 8, 0xD2800600U);
            counts->faccessat2++;
        }
    }
}

static int patch_va39_file(const char *source, const char *destination) {
    FILE *input = fopen(source, "rb");
    if (input == NULL) {
        (void)fprintf(stderr, "[agy-proot] Cannot read engine: %s\n", source);
        return 1;
    }
    if (fseek(input, 0, SEEK_END) != 0) {
        (void)fclose(input);
        return 1;
    }
    errno = 0;
    long size = ftell(input);
    if (size <= 0 || errno != 0) {
        (void)fclose(input);
        return 1;
    }
    if (fseek(input, 0, SEEK_SET) != 0) {
        (void)fclose(input);
        return 1;
    }

    unsigned char *buffer = malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(input);
        (void)fprintf(stderr, "[agy-proot] Out of memory while patching the engine.\n");
        return 1;
    }
    size_t read = fread(buffer, 1, (size_t)size, input);
    (void)fclose(input);
    if (read != (size_t)size) {
        free(buffer);
        return 1;
    }

    struct va39_counts counts;
    patch_va39_buffer(buffer, read, &counts);

    FILE *output = fopen(destination, "wb");
    if (output == NULL) {
        free(buffer);
        (void)fprintf(stderr, "[agy-proot] Cannot write engine: %s\n", destination);
        return 1;
    }
    size_t written = fwrite(buffer, 1, read, output);
    (void)fclose(output);
    free(buffer);

    if (written != read) {
        (void)fprintf(stderr, "[agy-proot] Short write while patching the engine.\n");
        return 1;
    }
    if (chmod(destination, 0755) != 0) {
        return 1;
    }

    (void)printf("[agy-proot] VA39 patches: ubfx=%u, lsl=%u, mask=%u, mmap=%u, "
                 "faccessat2=%u, word_rewrites=%u\n",
                 counts.ubfx, counts.lsl, counts.mask, counts.mmap, counts.faccessat2,
                 counts.word_rewrites);

    unsigned int total = counts.ubfx + counts.lsl + counts.mask + counts.mmap + counts.faccessat2 +
                         counts.word_rewrites;
    if (total == 0) {
        (void)fprintf(stderr,
                      "[agy-proot] No VA39 patch site was found in the engine. The upstream "
                      "binary layout changed, so the patch rules have to be updated.\n");
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- *
 * PRoot self-update
 *
 * The native Termux build replaces a twin binary pair published by this
 * repository.  A PRoot installation is built locally from source, so the
 * updater instead refreshes the engine straight from Google's official
 * manifest (version + URL + SHA-512), re-applies the VA39 patches and swaps
 * the engine in place, leaving the locally compiled bootstrapper untouched.
 * ------------------------------------------------------------------------- */

static int fetch_official_manifest(char *out, size_t out_size) {
    char command[1024];
    /* The official manifest is always fetched over TLS.  The protocol
       restriction is dropped for a mirrored manifest so the update path can be
       exercised in a test environment. */
    const char *protocol =
        strncmp(AGY_MANIFEST_URL, "https://", 8) == 0 ? "--proto '=https' --tlsv1.2 " : "";
    int written = snprintf(command, sizeof(command),
                           "curl %s--connect-timeout 5 --max-time 30 "
                           "-fsSL --retry 3 --retry-delay 2 --retry-all-errors '%s'",
                           protocol, AGY_MANIFEST_URL);
    if (written < 0 || written >= (int)sizeof(command)) {
        return 0;
    }

    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        return 0;
    }

    size_t total = 0;
    while (total + 1 < out_size) {
        size_t chunk = fread(out + total, 1, out_size - 1 - total, pipe);
        if (chunk == 0) {
            break;
        }
        total += chunk;
    }
    out[total] = '\0';

    int status = pclose(pipe);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 0;
    }

    return total > 0;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int extract_json_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[128];
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || written >= (int)sizeof(pattern)) {
        return 0;
    }

    const char *cursor = strstr(json, pattern);
    if (cursor == NULL) {
        return 0;
    }
    cursor += (size_t)written;

    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    if (*cursor != ':') {
        return 0;
    }
    cursor++;

    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    if (*cursor != '"') {
        return 0;
    }
    cursor++;

    size_t index = 0;
    while (*cursor != '\0' && *cursor != '"' && index + 1 < out_size) {
        out[index++] = *cursor++;
    }
    out[index] = '\0';

    return index > 0;
}

static int read_installed_version(const char *dir, char *out, size_t out_size) {
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/.agy.version", dir);
    if (written < 0 || written >= (int)sizeof(path)) {
        return 0;
    }

    FILE *file = fopen(path, "r");
    if (file != NULL) {
        char line[64];
        if (fgets(line, sizeof(line), file) != NULL) {
            for (size_t index = 0; index < sizeof(line); index++) {
                if (line[index] == '\r' || line[index] == '\n') {
                    line[index] = '\0';
                    break;
                }
            }
            if (line[0] != '\0') {
                (void)fclose(file);
                (void)snprintf(out, out_size, "%s", line);
                return 1;
            }
        }
        (void)fclose(file);
    }

    (void)snprintf(out, out_size, "%s", AGY_VERSION);

    return 1;
}

/* The download/verify/extract/install sequence lives in a small shell script
   so the quoting stays readable; it is written to a temporary file at runtime
   and removed afterwards. */
static const char AGY_PROOT_UPDATE_SCRIPT[] =
    "#!/bin/sh\n"
    "set -e\n"
    "install_dir=$1\n"
    "url=$2\n"
    "sha=$3\n"
    "version=$4\n"
    "tmp=$(mktemp -d \"${TMPDIR:-/tmp}/agy-update.XXXXXX\")\n"
    "committed=0\n"
    "cleanup() {\n"
    "  status=$?\n"
    "  if [ \"$committed\" -eq 0 ] && [ -e \"$install_dir/.agy.va39.old\" ]; then\n"
    "    mv -f \"$install_dir/.agy.va39.old\" \"$install_dir/agy.va39\" || :\n"
    "  fi\n"
    "  rm -rf \"$tmp\"\n"
    "  exit $status\n"
    "}\n"
    "trap cleanup EXIT\n"
    "trap 'exit 129' HUP\n"
    "trap 'exit 130' INT\n"
    "trap 'exit 143' TERM\n"
    "case \"$url\" in\n"
    "  https://*) proto=\"--proto '=https' --tlsv1.2\" ;;\n"
    "  *) proto=\"\" ;;\n"
    "esac\n"
    "curl $proto --connect-timeout 10 --max-time 900 -fsSL \\\n"
    "  --retry 5 --retry-delay 3 --retry-all-errors \\\n"
    "  --speed-limit 1024 --speed-time 60 \\\n"
    "  -o \"$tmp/engine.tar.gz\" \"$url\"\n"
    "printf '%s  %s\\n' \"$sha\" \"$tmp/engine.tar.gz\" | sha512sum -c - >/dev/null\n"
    "tar -xzf \"$tmp/engine.tar.gz\" -C \"$tmp\" antigravity\n"
    "test -s \"$tmp/antigravity\"\n"
    "\"$install_dir/agy\" __patch-va39 \"$tmp/antigravity\" \"$tmp/agy.va39.new\"\n"
    "test -s \"$tmp/agy.va39.new\"\n"
    "chmod 0755 \"$tmp/agy.va39.new\"\n"
    "\"$tmp/agy.va39.new\" --version >/dev/null 2>&1\n"
    "cp -p \"$install_dir/agy.va39\" \"$install_dir/.agy.va39.old\"\n"
    "install -m 0755 \"$tmp/agy.va39.new\" \"$install_dir/agy.va39\"\n"
    "printf '%s\\n' \"$version\" > \"$install_dir/.agy.version.tmp\"\n"
    "mv -f \"$install_dir/.agy.version.tmp\" \"$install_dir/.agy.version\"\n"
    "committed=1\n"
    "rm -f \"$install_dir/.agy.va39.old\"\n";

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static int perform_proot_update(const char *dir, const char *url, const char *sha,
                                const char *version) {
    char script_path[] = "/tmp/agy-update-XXXXXX";
    int descriptor = mkstemp(script_path);
    if (descriptor < 0) {
        (void)fprintf(stderr, "[agy-proot] Could not create the update script.\n");
        return -1;
    }

    size_t script_length = strlen(AGY_PROOT_UPDATE_SCRIPT);
    ssize_t written = write(descriptor, AGY_PROOT_UPDATE_SCRIPT, script_length);
    (void)close(descriptor);
    if (written != (ssize_t)script_length) {
        (void)unlink(script_path);
        return -1;
    }
    if (chmod(script_path, 0700) != 0) {
        (void)unlink(script_path);
        return -1;
    }

    char command[PATH_MAX + 1024];
    int length = snprintf(command, sizeof(command), "sh '%s' '%s' '%s' '%s' '%s'", script_path, dir,
                          url, sha, version);
    if (length < 0 || length >= (int)sizeof(command)) {
        (void)unlink(script_path);
        return -1;
    }

    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    int status = system(command);
    (void)unlink(script_path);

    return status;
}

static void check_and_perform_proot_update(enum update_check_mode mode, const char *dir,
                                           int auto_update) {
    char manifest[4096];
    char latest_version[64];
    char url[1024];
    char sha[256];
    char installed_version[64];

    if (mode == UPDATE_CHECK_EXPLICIT) {
        printf("[agy-proot] Querying the official Antigravity CLI manifest...\n");
    }

    if (!fetch_official_manifest(manifest, sizeof(manifest))) {
        report_update_check_error(mode, "the official manifest query did not succeed");
        return;
    }
    if (!extract_json_string(manifest, "version", latest_version, sizeof(latest_version))) {
        report_update_check_error(mode, "the official manifest did not include a version");
        return;
    }
    if (!extract_json_string(manifest, "url", url, sizeof(url)) ||
        !extract_json_string(manifest, "sha512", sha, sizeof(sha))) {
        report_update_check_error(mode, "the official manifest did not include a download URL");
        return;
    }
    if (!read_installed_version(dir, installed_version, sizeof(installed_version))) {
        report_update_check_error(mode, "could not determine the installed version");
        return;
    }

    const char *clean_latest = (latest_version[0] == 'v') ? latest_version + 1 : latest_version;
    const char *clean_installed =
        (installed_version[0] == 'v') ? installed_version + 1 : installed_version;

    int comparison = compare_release_versions(clean_latest, clean_installed);

    if (mode == UPDATE_CHECK_EXPLICIT) {
        printf("[agy-proot] Installed engine version: v%s\n", clean_installed);
        printf("[agy-proot] Latest available version: v%s\n", clean_latest);
    }

    if (comparison == -2) {
        report_update_check_error(mode, "could not compare installed and available versions");
        return;
    }
    if (comparison <= 0) {
        if (mode == UPDATE_CHECK_EXPLICIT) {
            if (comparison == 0) {
                printf("[agy-proot] You are already up to date with the latest engine release.\n");
            } else {
                printf("[agy-proot] Installed version v%s is newer than latest release v%s; "
                       "no update applied.\n",
                       clean_installed, clean_latest);
            }
        }
        return;
    }

    printf("\n[agy-proot] A new engine release (v%s) is available!\n", clean_latest);
    if (!should_perform_update(auto_update)) {
        printf("[agy-proot] Update cancelled.\n");
        return;
    }

    printf("\n[agy-proot] Downloading and applying the engine update...\n");
    int status = perform_proot_update(dir, url, sha, clean_latest);
    if (status == 0) {
        if (mode == UPDATE_CHECK_STARTUP) {
            printf("[agy-proot] Update completed successfully. Starting the CLI...\n");
        } else {
            printf("[agy-proot] Update completed successfully! Please restart the CLI.\n");
        }
        return;
    }

    if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 125) {
        printf("[agy-proot] Error: Update failed and rollback could not be completed.\n");
    } else {
        printf("[agy-proot] Error: Update failed; the installed engine was left unchanged or "
               "restored.\n");
    }
}

static int is_update_help_flag(const char *arg) {
    return strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0;
}

static int is_update_auto_flag(const char *arg) {
    return strcmp(arg, "-y") == 0 || strcmp(arg, "--yes") == 0 || strcmp(arg, "--auto") == 0;
}

static int update_command_requests_help(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        if (is_update_help_flag(argv[i])) {
            return 1;
        }
    }

    return 0;
}

static int is_update_command(int argc, char **argv) {
    return argc >= 2 && strcmp(argv[1], "update") == 0;
}

static int env_requests_auto_update(void) {
    return env_var_enabled("AGY_AUTO_UPDATE");
}

static int handle_update_command(const char *dir, int argc, char **argv,
                                 const struct agy_environment *environment) {
    int auto_update = env_requests_auto_update();

    for (int i = 2; i < argc; i++) {
        if (is_update_help_flag(argv[i])) {
            print_update_usage();
            return 0;
        }
        if (is_update_auto_flag(argv[i])) {
            auto_update = 1;
        }
    }

    check_and_perform_update(UPDATE_CHECK_EXPLICIT, dir, auto_update, environment);
    return 0;
}

static int should_check_for_update_on_startup(int argc, char *const *argv) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            return 0;
        }
    }

    return 1;
}

static void perform_startup_update_check(const char *dir, int argc, char **argv,
                                         const struct agy_environment *environment) {
    if (should_check_for_update_on_startup(argc, argv)) {
        check_and_perform_update(UPDATE_CHECK_STARTUP, dir, env_requests_auto_update(),
                                 environment);
    }
}

static void print_unsupported_host_message(void) {
    (void)fprintf(stderr,
                  "[agy-proot] Unsupported host: no glibc dynamic loader was found.\n"
                  "[agy-proot] This build requires a glibc userland.\n"
                  "[agy-proot] Use a PRoot Debian/Ubuntu root on Termux, or a native aarch64 "
                  "glibc Linux host.\n");
}

static int require_resolver_config(const struct agy_environment *environment) {
    if (!path_readable(environment->resolv_path)) {
        (void)fprintf(stderr, "[agy-proot] Missing resolver configuration: %s\n",
                      environment->resolv_path);
        (void)fprintf(stderr, "[agy-proot] Without this file, login and OAuth network requests "
                              "may fail.\n");
        return 0;
    }

    return 1;
}

static int resolve_qemu_for_cpu(const struct agy_environment *environment, const char **qemu) {
    unsigned long hwcap = getauxval(AT_HWCAP);

    *qemu = NULL;
    if ((hwcap & HWCAP_ATOMICS) != 0) {
        return 1;
    }

    if (path_exists(environment->qemu_path)) {
        *qemu = environment->qemu_path;
        return 1;
    }

    (void)fprintf(stderr, "[agy-proot] CPU lacks LSE atomics, and qemu-aarch64 was not found.\n");
    (void)fprintf(stderr, "[agy-proot] You may need to install the qemu-user-aarch64 package.\n");
    return 0;
}

int main(int argc, char **argv) {
    char exec_path[PATH_MAX];
    char patched_bin[PATH_MAX];
    const char *loader = NULL;
    const char *dir = NULL;
    const char *qemu = NULL;
    const char *exec_target = NULL;
    const char *exec_error = NULL;
    char **new_argv = NULL;
    int arg_idx = 0;
    int written = 0;
    ssize_t read_len = 0;
    struct agy_environment environment;

    /* Hidden maintenance command: apply the VA39 patches to an engine binary. */
    if (argc == 4 && strcmp(argv[1], "__patch-va39") == 0) {
        return patch_va39_file(argv[2], argv[3]);
    }

    if (!resolve_environment(&environment)) {
        print_unsupported_host_message();
        return 1;
    }

    if (!resolve_qemu_for_cpu(&environment, &qemu)) {
        return 1;
    }

    loader = environment.loader;
    exec_target = loader;
    exec_error = "[agy-proot] execv failed";

    if (!path_exists(loader)) {
        (void)fprintf(stderr, "[agy-proot] Missing dynamic loader: %s\n", loader);
        return 1;
    }

    // Clear conflicting Android Bionic preloads and search paths.
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");

    // Set dynamic Go resolver and SSL configuration.
    setenv("GODEBUG", "netdns=cgo", 1);
    setenv("SSL_CERT_FILE", environment.cert_path, 1);

    read_len = readlink("/proc/self/exe", exec_path, sizeof(exec_path) - 1);
    if (read_len < 0 || read_len >= (ssize_t)sizeof(exec_path)) {
        return 1;
    }
    exec_path[read_len] = '\0';
    dir = dirname(exec_path);

    if (is_update_command(argc, argv)) {
        if (update_command_requests_help(argc, argv)) {
            return handle_update_command(dir, argc, argv, &environment);
        }
        if (!require_resolver_config(&environment)) {
            return 1;
        }
        return handle_update_command(dir, argc, argv, &environment);
    }

    if (!require_resolver_config(&environment)) {
        return 1;
    }

    perform_startup_update_check(dir, argc, argv, &environment);

    // Use only the runtime libraries that belong to the detected host.
    char lib_path[PATH_MAX + 16];
    (void)snprintf(lib_path, sizeof(lib_path), "%s", environment.lib_path);
    if (lib_path[0] == '\0') {
        return 1;
    }

    // Construct path to the patched binary
    written = snprintf(patched_bin, sizeof(patched_bin), "%s/agy.va39", dir);
    if (written < 0 || written >= (int)sizeof(patched_bin)) {
        return 1;
    }

    // We allocate enough space for: qemu + loader + "--library-path" + lib_path
    // + patched_bin + user args + NULL
    int new_argc = argc + 6;
    new_argv = malloc((size_t)new_argc * sizeof(*new_argv));
    if (!new_argv) {
        return 1;
    }

    arg_idx = 0;
    if (qemu) {
        new_argv[arg_idx++] = (char *)qemu;
        exec_target = qemu;
        exec_error = "[agy-proot] execv (qemu) failed";
    }
    new_argv[arg_idx++] = (char *)loader;
    new_argv[arg_idx++] = "--library-path";
    new_argv[arg_idx++] = lib_path;
    new_argv[arg_idx++] = patched_bin;

    for (int i = 1; i < argc; i++) {
        new_argv[arg_idx++] = argv[i];
    }
    new_argv[arg_idx] = NULL;

    // NOLINTNEXTLINE(clang-analyzer-optin.taint.GenericTaint)
    if (execv(exec_target, new_argv) == -1) {
        perror(exec_error);
        free(new_argv);
        return 1;
    }
}
