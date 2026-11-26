/* icx2read — print EVGA iCX2 sensors.
 *
 *   icx2read            temperatures (reads only)
 *   icx2read --power    also the three power inputs (performs Precision X1's latch writes)
 *   icx2read --json     one JSON object, for the menu-bar app
 *   icx2read --dump     identity and raw registers 0x31-0x39, for checking a new card
 */
#include "icx2.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    bool power = false, json = false, dump = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--power")) power = true;
        else if (!strcmp(argv[i], "--json")) json = true;
        else if (!strcmp(argv[i], "--dump")) dump = true;
        else { fprintf(stderr, "usage: icx2read [--power] [--json] [--dump]\n"); return 2; }
    }
    char err[256] = "";
    icx2_t *h = icx2_open(err, sizeof err);
    if (!h) {
        if (json) printf("{\"ok\":false,\"error\":\"%s\"}\n", err); else fprintf(stderr, "icx2read: %s\n", err);
        return 1;
    }
    icx2_reading_t r; memset(&r, 0, sizeof r);
    bool tok = icx2_read_temps(h, &r);
    icx2_read_fans(h, &r);
    if (power) icx2_read_power(h, &r);
    if (dump) {
        uint8_t a = 0, b = 0;
        icx2_smbus_read(h, 0x02, &a); icx2_smbus_read(h, 0x03, &b);
        printf("port %d  id %02X %02X\n", icx2_port(h), a, b);
        for (int i = 0; i < 9; i++) { uint8_t v = 0; icx2_smbus_read(h, (uint8_t)(0x31 + i), &v); printf("reg 0x%02X = 0x%02X\n", 0x31 + i, v); }
    }
    if (json) {
        printf("{\"ok\":%s,\"port\":%d,\"temps\":{", tok ? "true" : "false", icx2_port(h));
        for (int i = 0; i < ICX2_NUM_TEMPS; i++) printf("%s\"%s\":%d", i ? "," : "", icx2_temp_names[i], r.temp_c[i]);
        printf("}");
        if (r.fan_ok) printf(",\"fans\":[{\"fan\":2,\"target\":%d,\"rpm\":%d},{\"fan\":3,\"target\":%d,\"rpm\":%d}]", r.fan_target[0], r.fan_rpm[0], r.fan_target[1], r.fan_rpm[1]);
        if (power && r.power_ok) {
            printf(",\"rails\":[");
            for (int i = 0; i < ICX2_NUM_RAILS; i++)
                printf("%s{\"name\":\"%s\",\"mA\":%u,\"mV\":%u}", i ? "," : "", icx2_rail_names[i], r.rail_ma[i], r.rail_mv[i]);
            printf("]");
        }
        printf("}\n");
    } else if (!dump) {
        printf("EVGA iCX2 on I2C port %d\n", icx2_port(h));
        if (tok) for (int i = 0; i < ICX2_NUM_TEMPS; i++) printf("  %-5s %3d °C\n", icx2_temp_names[i], r.temp_c[i]);
        else printf("  temperatures: read failed\n");
        if (r.fan_ok) for (int i = 0; i < ICX2_NUM_FANS; i++) printf("  Fan %d  %4d RPM  (target %d %%)\n", i + 2, r.fan_rpm[i], r.fan_target[i]);
        if (power) {
            if (r.power_ok) {
                double total = 0;
                for (int i = 0; i < ICX2_NUM_RAILS; i++) {
                    double w = r.rail_ma[i] * (double)r.rail_mv[i] / 1e6; total += w;
                    printf("  %-8s %6.3f A  %6.3f V  %6.1f W\n", icx2_rail_names[i], r.rail_ma[i] / 1000.0, r.rail_mv[i] / 1000.0, w);
                }
                printf("  total    %6.1f W\n", total);
            } else printf("  power: read failed\n");
        }
    }
    icx2_close(h);
    return tok ? 0 : 1;
}
