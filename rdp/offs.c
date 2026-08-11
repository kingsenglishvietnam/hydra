/* offs.c -- which struct member lives at 0x68?
 *
 * The crash is `call *0x68(%r14)` with one argument, gdi->context.
 * Two candidate structs. Let the compiler answer instead of counting by hand.
 *
 * build:
 *   gcc -D__STDC_NO_THREADS__=1 -D_WIN32_WINNT=0x0A00 `
 *       -IC:/msys64/mingw64/include/freerdp3 -IC:/msys64/mingw64/include/winpr3 `
 *       offs.c -o offs.exe -LC:/msys64/mingw64/lib -lfreerdp3 -lwinpr3
 */
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/client/rdpgfx.h>
#include <stddef.h>
#include <stdio.h>

#define P(t, m) printf("  %-28s 0x%03zx\n", #m, offsetof(t, m))

int main(void)
{
    printf("sizeof(RdpgfxClientContext) = %zu\n", sizeof(RdpgfxClientContext));
    printf("RdpgfxClientContext:\n");
    P(RdpgfxClientContext, CacheToSurface);
    P(RdpgfxClientContext, CacheImportOffer);
    P(RdpgfxClientContext, CacheImportReply);
    P(RdpgfxClientContext, MapSurfaceToOutput);
    P(RdpgfxClientContext, MapSurfaceToWindow);
    P(RdpgfxClientContext, UpdateSurfaces);

    printf("\nsizeof(rdpUpdate) = %zu\n", sizeof(rdpUpdate));
    printf("rdpUpdate:\n");
    P(rdpUpdate, BeginPaint);
    P(rdpUpdate, EndPaint);
    P(rdpUpdate, SetBounds);
    P(rdpUpdate, Synchronize);
    P(rdpUpdate, DesktopResize);

    printf("\nrdpGdi:\n");
    P(rdpGdi, context);
    return 0;
}
