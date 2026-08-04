/* test_edid.c -- native unit test for hydra_edid.h (runs on Linux, no WDK). */
#include "../common/hydra_edid.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); ++fails; } } while (0)

static void dump(const uint8_t* e) {
    for (int i = 0; i < HYDRA_EDID_SIZE; ++i) {
        printf("%02X%s", e[i], (i % 16 == 15) ? "\n" : " ");
    }
}

static int test_mode(uint32_t w, uint32_t h, uint32_t hz) {
    uint8_t e[HYDRA_EDID_SIZE];
    char tag[64]; snprintf(tag, sizeof tag, "%ux%u@%u", w, h, hz);
    printf("[%s]\n", tag);

    int rc = hydra_edid_build(e, w, h, hz, "HYD", "Hydra Seat B");
    CHECK(rc == 0, "build returns 0");

    /* Fixed header pattern */
    static const uint8_t hdr[8] = {0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0};
    CHECK(memcmp(e, hdr, 8) == 0, "header magic 00 FF*6 00");

    /* Version 1.4 */
    CHECK(e[18] == 1 && e[19] == 4, "EDID version 1.4");

    /* Checksum: all 128 bytes sum to 0 mod 256 */
    uint32_t sum = 0; for (int i = 0; i < 128; ++i) sum += e[i];
    CHECK((sum & 0xFF) == 0, "checksum: bytes sum to 0 mod 256");

    /* Manufacturer id round-trips to HYD */
    uint8_t mfr[2]; hydra_edid_mfr(mfr, "HYD");
    CHECK(e[8] == mfr[0] && e[9] == mfr[1], "manufacturer id = HYD");

    /* Preferred DTD encodes the requested resolution */
    uint32_t rw = 0, rh = 0; hydra_edid_read_active(e, &rw, &rh);
    char m[96]; snprintf(m, sizeof m, "preferred DTD active = %ux%u (want %ux%u)", rw, rh, w, h);
    CHECK(rw == w && rh == h, m);

    /* Descriptor 3 is a monitor-name descriptor (tag 0xFC) */
    CHECK(e[90]==0 && e[91]==0 && e[92]==0 && e[93]==0xFC, "descriptor 3 is name (0xFC)");

    /* Non-zero pixel clock */
    CHECK(!(e[54]==0 && e[55]==0), "preferred DTD pixel clock is non-zero");

    if (w == 1920 && h == 1080 && hz == 60) { puts("  --- 1080p60 EDID bytes ---"); dump(e); }
    puts("");
    return 0;
}

int main(void) {
    puts("== hydra_edid unit test ==\n");
    test_mode(1920, 1080, 60);
    test_mode(2560, 1440, 60);
    test_mode(1366, 768, 60);
    test_mode(3840, 2160, 30);

    /* bad args rejected */
    uint8_t e[HYDRA_EDID_SIZE];
    printf("[bad args]\n");
    CHECK(hydra_edid_build(e, 0, 1080, 60, "HYD", "x") == -1, "reject width 0");
    CHECK(hydra_edid_build(e, 1920, 0, 60, "HYD", "x") == -1, "reject height 0");
    CHECK(hydra_edid_build(e, 1920, 1080, 0, "HYD", "x") == -1, "reject refresh 0");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails==1?"":"s");
    return fails ? 1 : 0;
}
