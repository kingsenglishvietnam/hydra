/* hydra_devprops.h  --  custom device-property keys shared by the producer of
 * the properties (hydrad, at SwDeviceCreate) and the consumer (iddseat, which
 * reads them in DeviceAdd). Keeping them in one header means the two sides can
 * never drift on the GUID/PID.
 *
 * WHY WE REDEFINE DEFINE_DEVPROPKEY
 *   The SDK's DEFINE_DEVPROPKEY only *defines* the key object when INITGUID is
 *   defined before <devpropdef.h>; otherwise it merely declares `extern const`,
 *   and you discover this at link time as an unresolved external. Rather than
 *   force INITGUID on every includer (which would also start emitting every
 *   DEFINE_GUID in every later header), we emit the definitions ourselves.
 *   DECLSPEC_SELECTANY puts each in its own COMDAT, so multiple translation
 *   units including this header fold to one definition -- no ODR clash.
 */
#pragma once
#include <devpropdef.h>

#undef  HYDRA_DEFINE_DEVPROPKEY
#define HYDRA_DEFINE_DEVPROPKEY(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8, pid) \
    EXTERN_C const DEVPROPKEY DECLSPEC_SELECTANY name =                               \
        { { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }, pid }

/* {d7a90000-1111-4bea-9a77-5e0000000002}, PID 2 = seat name, PID 3 = mode. */
HYDRA_DEFINE_DEVPROPKEY(DEVPKEY_Hydra_SeatName,
    0xd7a90000, 0x1111, 0x4bea, 0x9a, 0x77, 0x5e, 0x00, 0x00, 0x00, 0x00, 0x02, 2);
HYDRA_DEFINE_DEVPROPKEY(DEVPKEY_Hydra_SeatMode,
    0xd7a90000, 0x1111, 0x4bea, 0x9a, 0x77, 0x5e, 0x00, 0x00, 0x00, 0x00, 0x02, 3);
