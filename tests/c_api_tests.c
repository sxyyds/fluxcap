#include <fluxcap/fluxcap.h>

#include <stdio.h>
#include <string.h>

static int check(int condition, const char* message) {
    if (condition) {
        return 1;
    }
    fprintf(stderr, "[FAIL] %s; last_error: %s\n", message, fluxcap_last_error());
    return 0;
}

int main(void) {
    fluxcap_config config = fluxcap_config_default();
    fluxcap_session* session = NULL;
    uint32_t display_count = 0;

    if (!check(config.struct_size == sizeof(fluxcap_config),
               "default config has the wrong struct size")
        || !check(config.abi_version == FLUXCAP_ABI_VERSION,
                  "default config has the wrong ABI version")
        || !check(strcmp(fluxcap_version_string(), "0.1.0") == 0,
                  "version string is unexpected")
        || !check(fluxcap_enumerate_displays(NULL, 0, &display_count)
                      == FLUXCAP_STATUS_OK,
                  "display count query failed")
        || !check(display_count > 0, "display count is zero")) {
        return 1;
    }

    config.buffer_count = 1;
    if (!check(fluxcap_create(&config, &session) == FLUXCAP_STATUS_INVALID_ARGUMENT,
               "invalid buffer count was accepted")
        || !check(session == NULL, "failed create returned a session")
        || !check(fluxcap_last_error()[0] != '\0',
                  "failed create did not provide diagnostics")) {
        fluxcap_destroy(session);
        return 1;
    }

    puts("[PASS] public header and C ABI smoke test");
    return 0;
}
