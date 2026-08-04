/* hydra_edid.h  --  EDID 1.4 block generator for the Hydra virtual monitor.
 *
 * Fills HYDRA-TODO(a). A virtual (indirect) monitor still has to hand Windows a
 * well-formed EDID: the OS parses it, validates the checksum, and derives the
 * advertised mode from the *preferred detailed timing descriptor* (DTD). A
 * malformed EDID = the monitor never appears, or appears with the wrong mode.
 *
 * This is pure C (no Windows types) so it can be:
 *   - #included by the UMDF driver (iddseat) to emit a per-seat EDID from the
 *     `edid = "WxH@Hz"` line in seats.toml, and
 *   - unit-tested natively (tests/test_edid.c) without the WDK.
 *
 * The timings are synthesised with fixed reduced-blanking-style porches. For a
 * virtual head the exact porch values never clock real hardware; what matters is
 * that (1) the block is structurally valid, (2) the checksum is correct, and
 * (3) hactive/vactive in the preferred DTD equal the requested resolution. All
 * three are asserted by the native test.
 */
#ifndef HYDRA_EDID_H
#define HYDRA_EDID_H

#include <stdint.h>
#include <stddef.h>

#define HYDRA_EDID_SIZE 128

/* Pack a 3-letter PNP manufacturer id (e.g. "HYD") into the 2 EDID bytes.
 * Each letter is 5 bits, A=1..Z=26, MSB of the pair is 0. Big-endian on the wire. */
static inline void hydra_edid_mfr(uint8_t out[2], const char id[3])
{
    uint16_t v = (uint16_t)(((id[0] - 'A' + 1) & 0x1F) << 10)
               | (uint16_t)(((id[1] - 'A' + 1) & 0x1F) << 5)
               | (uint16_t)( (id[2] - 'A' + 1) & 0x1F);
    out[0] = (uint8_t)(v >> 8);
    out[1] = (uint8_t)(v & 0xFF);
}

/* Write an 18-byte Detailed Timing Descriptor for w x h @ hz. */
static inline void hydra_edid_dtd(uint8_t d[18], uint32_t w, uint32_t h, uint32_t hz)
{
    /* Reduced-blanking-ish fixed geometry. Consistent, and Windows accepts it. */
    const uint32_t hblank = 160, vblank = 40;
    const uint32_t hfront = 48,  hsync  = 32;   /* h back porch = 160-48-32 = 80 */
    const uint32_t vfront = 3,   vsync  = 6;
    const uint32_t htotal = w + hblank;
    const uint32_t vtotal = h + vblank;

    /* Pixel clock in 10 kHz units. */
    uint64_t clk = (uint64_t)htotal * vtotal * hz;
    uint32_t clk10k = (uint32_t)((clk + 5000) / 10000);   /* round to nearest 10kHz */
    if (clk10k < 1) clk10k = 1;
    if (clk10k > 0xFFFF) clk10k = 0xFFFF;                  /* EDID DTD clock is 16-bit */

    d[0] = (uint8_t)(clk10k & 0xFF);
    d[1] = (uint8_t)((clk10k >> 8) & 0xFF);

    d[2] = (uint8_t)(w & 0xFF);
    d[3] = (uint8_t)(hblank & 0xFF);
    d[4] = (uint8_t)((((w >> 8) & 0x0F) << 4) | ((hblank >> 8) & 0x0F));

    d[5] = (uint8_t)(h & 0xFF);
    d[6] = (uint8_t)(vblank & 0xFF);
    d[7] = (uint8_t)((((h >> 8) & 0x0F) << 4) | ((vblank >> 8) & 0x0F));

    d[8]  = (uint8_t)(hfront & 0xFF);
    d[9]  = (uint8_t)(hsync & 0xFF);
    d[10] = (uint8_t)((((vfront & 0x0F) << 4)) | (vsync & 0x0F));
    d[11] = (uint8_t)((((hfront >> 8) & 0x03) << 6)
                    | (((hsync  >> 8) & 0x03) << 4)
                    | (((vfront >> 4) & 0x03) << 2)
                    |  ((vsync  >> 4) & 0x03));

    /* Physical image size (mm), arbitrary 16:9-ish; only affects reported DPI. */
    const uint32_t wmm = 528, hmm = 297;
    d[12] = (uint8_t)(wmm & 0xFF);
    d[13] = (uint8_t)(hmm & 0xFF);
    d[14] = (uint8_t)((((wmm >> 8) & 0x0F) << 4) | ((hmm >> 8) & 0x0F));

    d[15] = 0;   /* h border */
    d[16] = 0;   /* v border */
    d[17] = 0x1E;/* interlace off, digital separate sync, +Hsync +Vsync */
}

/* Write a "display descriptor" tag block: type 0xFB..0xFF with ASCII payload. */
static inline void hydra_edid_string_desc(uint8_t d[18], uint8_t tag, const char* s)
{
    d[0] = 0x00; d[1] = 0x00; d[2] = 0x00;   /* signals a display descriptor, not a DTD */
    d[3] = tag;                               /* 0xFC=name, 0xFF=serial, 0xFE=ascii text */
    d[4] = 0x00;
    size_t i = 0;
    for (; i < 13 && s && s[i]; ++i) d[5 + i] = (uint8_t)s[i];
    if (i < 13) { d[5 + i] = 0x0A; ++i; }     /* terminator */
    for (; i < 13; ++i) d[5 + i] = 0x20;      /* pad with spaces */
}

/* Monitor range-limits descriptor (tag 0xFD): keeps the OS happy about ranges. */
static inline void hydra_edid_range_desc(uint8_t d[18], uint32_t hz)
{
    uint8_t vmin = (uint8_t)(hz > 5 ? hz - 5 : 1);
    uint8_t vmax = (uint8_t)(hz + 5);
    d[0]=0x00; d[1]=0x00; d[2]=0x00; d[3]=0xFD; d[4]=0x00;
    d[5]=vmin; d[6]=vmax;         /* vertical field rate min/max (Hz) */
    d[7]=30;   d[8]=160;          /* horizontal line rate min/max (kHz) */
    d[9]=(uint8_t)(400/10);       /* max pixel clock / 10 MHz */
    d[10]=0x00;                   /* no extended timing info */
    d[11]=0x0A; d[12]=0x20; d[13]=0x20; d[14]=0x20; d[15]=0x20; d[16]=0x20; d[17]=0x20;
}

/* Build a complete, checksum-valid 128-byte EDID for w x h @ hz.
 * `name` is truncated to 13 chars. Returns 0 on success, -1 on bad args. */
static inline int hydra_edid_build(uint8_t out[HYDRA_EDID_SIZE],
                                   uint32_t w, uint32_t h, uint32_t hz,
                                   const char mfr[3], const char* name)
{
    if (!out || w == 0 || h == 0 || hz == 0 || w > 0x0FFF + 0xFF) return -1;
    for (int i = 0; i < HYDRA_EDID_SIZE; ++i) out[i] = 0;

    /* 0..7 : fixed header pattern */
    out[0]=0x00; out[1]=0xFF; out[2]=0xFF; out[3]=0xFF;
    out[4]=0xFF; out[5]=0xFF; out[6]=0xFF; out[7]=0x00;

    /* 8..9 : manufacturer id */
    hydra_edid_mfr(&out[8], mfr);
    /* 10..11 : product code (LE) */
    out[10]=0x01; out[11]=0x00;
    /* 12..15 : serial (LE) */
    out[12]=0x01; out[13]=0x00; out[14]=0x00; out[15]=0x00;
    /* 16..17 : week / year (year = 1990 + value) */
    out[16]=1; out[17]=(uint8_t)(2025 - 1990);
    /* 18..19 : EDID version 1.4 */
    out[18]=1; out[19]=4;

    /* 20 : basic display params — digital input, 8 bpc, DisplayPort-ish */
    out[20]=0x80 | (0x2 << 4) | 0x05;  /* bit7 digital; bits6-4=010 => 8bpc; bits3-0 iface=DP */
    /* 21..22 : image size cm (16:9 ~ 53x30) */
    out[21]=53; out[22]=30;
    /* 23 : display gamma (2.2 -> (2.2*100)-100 = 120) */
    out[23]=120;
    /* 24 : feature support — RGB, preferred timing is native, continuous freq */
    out[24]=0x0A;

    /* 25..34 : chromaticity — generic sRGB-ish coordinates. */
    static const uint8_t chroma[10] = {0xEE,0x91,0xA3,0x54,0x4C,0x99,0x26,0x0F,0x50,0x54};
    for (int i=0;i<10;++i) out[25+i]=chroma[i];

    /* 35..37 : established timings — none (we advertise via DTD). */
    out[35]=0; out[36]=0; out[37]=0;
    /* 38..53 : standard timings — all unused (0x0101). */
    for (int i=38;i<=53;i+=2){ out[i]=0x01; out[i+1]=0x01; }

    /* 54..71 : descriptor 1 = preferred detailed timing (our mode). */
    hydra_edid_dtd(&out[54], w, h, hz);
    /* 72..89 : descriptor 2 = monitor range limits. */
    hydra_edid_range_desc(&out[72], hz);
    /* 90..107 : descriptor 3 = monitor name. */
    hydra_edid_string_desc(&out[90], 0xFC, name ? name : "Hydra Seat");
    /* 108..125 : descriptor 4 = ascii text (seat label reused). */
    hydra_edid_string_desc(&out[108], 0xFE, name ? name : "Hydra Seat");

    /* 126 : extension block count. */
    out[126]=0;

    /* 127 : checksum so that the 128 bytes sum to 0 mod 256. */
    uint32_t sum = 0;
    for (int i=0;i<127;++i) sum += out[i];
    out[127] = (uint8_t)((256 - (sum & 0xFF)) & 0xFF);
    return 0;
}

/* Read hactive/vactive back out of the preferred DTD (for validation). */
static inline void hydra_edid_read_active(const uint8_t edid[HYDRA_EDID_SIZE],
                                          uint32_t* w, uint32_t* h)
{
    const uint8_t* d = &edid[54];
    if (w) *w = (uint32_t)d[2] | ((uint32_t)(d[4] >> 4) << 8);
    if (h) *h = (uint32_t)d[5] | ((uint32_t)(d[7] >> 4) << 8);
}

/* Recover the full mode (w,h,hz) from the preferred DTD. Hz is derived from the
 * pixel clock and the blanking this generator uses, then rounded. */
static inline void hydra_edid_read_mode(const uint8_t edid[HYDRA_EDID_SIZE],
                                        uint32_t* w, uint32_t* h, uint32_t* hz)
{
    const uint8_t* d = &edid[54];
    uint32_t rw = (uint32_t)d[2] | ((uint32_t)(d[4] >> 4) << 8);
    uint32_t rh = (uint32_t)d[5] | ((uint32_t)(d[7] >> 4) << 8);
    uint32_t hblank = (uint32_t)d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
    uint32_t vblank = (uint32_t)d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
    uint32_t clk10k = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    uint64_t total = (uint64_t)(rw + hblank) * (rh + vblank);
    uint32_t rhz = 60;
    if (total) rhz = (uint32_t)(((uint64_t)clk10k * 10000 + total / 2) / total);
    if (w) *w = rw;
    if (h) *h = rh;
    if (hz) *hz = rhz ? rhz : 60;
}

#endif /* HYDRA_EDID_H */
