/* icx2.h — read EVGA iCX2 sensors (RTX 20 series) through the NullMoth NVRM user client.
 *
 * The iCX2 microcontroller sits on one of the GPU's I2C ports at 7-bit address 0x2B. The register map below was
 * taken from an NvAPISpy capture of EVGA Precision X1 on an EVGA RTX 2080 FTW3 Ultra:
 *
 *   0x02 / 0x03   identity, reads 0x74 / 0xF9
 *   0x31 .. 0x39  nine temperatures, one byte each, whole degrees C
 *   0xE0 .. 0xEB  three power inputs, each 4 bytes: current (mA, little-endian u16) then voltage (mV, LE u16).
 *                 Precision X1 latches them first with: 0x0E=0xE5, 0x0E=0xE9, 0x1E=0x01, 0x0E=0xE0.
 *   0x51 / 0x54   Fan 2: target duty (%) and measured speed in units of 30 RPM (RPM = value x 30)
 *   0x46 / 0x49   Fan 3: the same. (Precision X1 sets the targets through 0x53 / 0x48; this code never writes them.)
 *                 Fan 1 is driven by the GPU itself and its RPM comes from the NVIDIA driver, not from the iCX.
 *                 Mapping confirmed against X1's display: 0x54=23 -> 690 RPM, 0x49=18 -> 540 RPM.
 *
 * Address 0x49 on the same card is the RGB controller; nothing here touches it.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define ICX2_ADDR        0x2B
#define ICX2_NUM_TEMPS   9
#define ICX2_NUM_RAILS   3
#define ICX2_NUM_FANS    2    /* iCX fans: index 0 = card Fan 2, index 1 = card Fan 3 */

typedef struct icx2 icx2_t;

typedef struct {
    bool     ok;
    int      temp_c[ICX2_NUM_TEMPS];     /* registers 0x31..0x39 */
    bool     power_ok;
    uint16_t rail_ma[ICX2_NUM_RAILS];    /* current, mA */
    uint16_t rail_mv[ICX2_NUM_RAILS];    /* voltage, mV */
    bool     fan_ok;
    int      fan_target[ICX2_NUM_FANS];  /* duty the iCX is driving, % (index 0 = Fan 2, 1 = Fan 3) */
    int      fan_rpm[ICX2_NUM_FANS];     /* measured speed, RPM */
} icx2_reading_t;

/* Opens the NVRM user client, allocates an NV40_I2C object and finds the port that answers at 0x2B with the
 * iCX2 identity. Returns NULL (and fills err) when the card or driver is not there. */
icx2_t *icx2_open(char *err, int errlen);
void    icx2_close(icx2_t *h);
int     icx2_port(const icx2_t *h);

/* Temperatures only: reads, no writes. */
bool icx2_read_temps(icx2_t *h, icx2_reading_t *out);
/* Fan 2 and Fan 3 target (%) and measured speed (RPM): reads only. */
bool icx2_read_fans(icx2_t *h, icx2_reading_t *out);
/* Power inputs: performs Precision X1's latch sequence (writes to 0x0E and 0x1E on 0x2B only), then reads. */
bool icx2_read_power(icx2_t *h, icx2_reading_t *out);

/* Labels for the nine temperatures, in register order. Assumed to follow EVGA's iCX order (GPU2, MEM1-3, PWR1-5);
 * confirm against Precision X1 before relying on individual names. */
extern const char *const icx2_temp_names[ICX2_NUM_TEMPS];
extern const char *const icx2_rail_names[ICX2_NUM_RAILS];

/* Low-level, for diagnostics. */
bool icx2_smbus_read(icx2_t *h, uint8_t reg, uint8_t *val);
