/* A harmless process for testing seat-scoped client termination. No RDP,
 * input hooks, network connections or device access. */
#include <windows.h>
int main(void)
{
    Sleep(60000);
    return 0;
}
