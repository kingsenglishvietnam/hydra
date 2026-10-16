/* Compile the real CLI against a test-only pipe, never the live control pipe. */
#include "../../common/hydra_ipc.h"
#undef HYDRA_CONTROL_PIPE
#define HYDRA_CONTROL_PIPE L"\\\\.\\pipe\\hydra_test_cli_fragments"
#include "../../hydractl/hydractl.cpp"
