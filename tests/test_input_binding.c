#include "../input/hydra_input_binding.h"
#include <stdio.h>

static int fails;
#define CHECK(c, m) do { if (c) printf("  ok   %s\n", m); else { printf("  FAIL %s\n", m); ++fails; } } while (0)

int main(void)
{
    /* Two identical Dell keyboards/mice are a real four-seat workstation case. */
    const HydraInputDevice inventory[] = {
        {1, L"HID\\VID_413C&PID_2113&MI_00"},
        {2, L"HID\\VID_413C&PID_2113&MI_00"},
        {3, L"HID\\VID_0D62&PID_913E&MI_00"},
        {4, L"HID\\VID_C0F4&PID_04C0&MI_00"},
        {11, L"HID\\VID_413C&PID_301A"},
        {12, L"HID\\VID_413C&PID_301A"},
        {13, L"HID\\VID_0D62&PID_12EC"},
        {14, L"HID\\VID_056A&PID_0357"}
    };
    int out[4] = {0}, bad = -1;
    HydraInputSelector s[4] = {{2, L""}, {0, L"vid_0d62&pid_913e"}, {0, L"PID_04C0"}};
    CHECK(hydra_resolve_inputs(s, 3, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_OK &&
          out[0] == 2 && out[1] == 3 && out[2] == 4 && bad == -1,
          "console plus three seats get distinct keyboard devices");
    s[0].number = 0; s[0].id = L"VID_413C&PID_2113";
    CHECK(hydra_resolve_inputs(s, 3, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_AMBIGUOUS && bad == 0,
          "identical keyboard models are not silently merged");
    CHECK(!out[0] && !out[1] && !out[2], "failed resolution publishes no partial ownership map");
    s[0].id = L"VID_413C&PID_301A";
    CHECK(hydra_resolve_inputs(s, 1, inventory, 8, 11, 20, out, &bad) == HYDRA_BIND_AMBIGUOUS,
          "identical mouse models are not silently merged");
    s[0].id = L"VID_0D62"; s[1].id = L"PID_913E";
    CHECK(hydra_resolve_inputs(s, 2, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_SHARED && bad == 1,
          "different substring selectors cannot claim the same physical device");
    s[0].number = 3; s[0].id = L"";
    CHECK(hydra_resolve_inputs(s, 2, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_SHARED,
          "numeric versus hardware-ID ownership overlap is detected");
    s[0].number = 0; s[0].id = L"NOT_ATTACHED";
    CHECK(hydra_resolve_inputs(s, 1, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_MISSING,
          "missing devices fail before capture");
    s[0].number = 9; s[0].id = L"";
    CHECK(hydra_resolve_inputs(s, 1, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_MISSING,
          "unpopulated numeric device rejected");
    s[0].number = 11;
    CHECK(hydra_resolve_inputs(s, 1, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_INVALID,
          "keyboard cannot select mouse device number");
    s[0].number = 3; s[0].id = L"VID_0D62";
    CHECK(hydra_resolve_inputs(s, 1, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_INVALID,
          "mixed selector is not silently reinterpreted");
    s[0].number = 0; s[0].id = L"";
    CHECK(hydra_resolve_inputs(s, 1, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_INVALID,
          "empty selector is not a wildcard");
    for (int i = 0; i < 4; ++i) { s[i].number = i + 1; s[i].id = L""; }
    CHECK(hydra_resolve_inputs(s, 4, inventory, 8, 1, 10, out, &bad) == HYDRA_BIND_OK && out[3] == 4,
          "router's four-extra-seat limit is supported");
    {
        int n = 77;
        CHECK(!hydra_parse_number("9999999999999999", 1, 65535, &n) && n == 77,
              "overflow does not modify destination");
        CHECK(!hydra_valid_seat_name("B\\C") && hydra_valid_seat_name("seat-C_2"),
              "IPC name rejects path separators");
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
