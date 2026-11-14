#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/service_policy.h"

#define CHECK(condition, code) do { if (!(condition)) return (code); } while (0)
int main(int argc, char **argv) {
    unsigned flags, index;
    const unsigned intervals[3] = {2,5,10};
    FILE *file;
    uint32_t value;
    const char *critical[] = {"/", "/stream", "/snapshot", "/raw", "/api/status", "/api/services", "/assets/services.js", "/api/timelapse/start"};
    CHECK(argc == 2, 2);
    file = fopen(argv[1], "wb");
    CHECK(file != NULL, 3);
    for (flags = 0; flags < 16; ++flags) {
        for (index = 0; index < 3; ++index) {
            CHECK(rc_service_set_interval(RC_SERVICE_MAGIC | flags, intervals[index], &value), 4);
            CHECK(rc_service_valid(value), 5);
            CHECK((value & 15) == flags, 6);
            CHECK(rc_service_interval(value) == intervals[index], 7);
            CHECK(fwrite(&value, sizeof(value), 1, file) == 1, 8);
        }
    }
    fclose(file);
    file = fopen(argv[1], "rb");
    CHECK(file != NULL, 9);
    for (flags = 0; flags < 16; ++flags) {
        for (index = 0; index < 3; ++index) {
            CHECK(fread(&value, sizeof(value), 1, file) == 1, 10);
            value = rc_service_load(value);
            CHECK((value & 15) == flags && rc_service_interval(value) == intervals[index], 11);
        }
    }
    fclose(file);
    CHECK(rc_service_load(0) == RC_SERVICE_DEFAULT, 12);
    CHECK(rc_service_load(0xffffffffUL) == RC_SERVICE_DEFAULT, 13);
    CHECK(rc_service_load(RC_SERVICE_MAGIC | 0x30) == RC_SERVICE_DEFAULT, 14);
    CHECK(!rc_service_set_interval(RC_SERVICE_DEFAULT, 0, &value), 15);
    CHECK(!rc_service_set_interval(RC_SERVICE_DEFAULT, 3, &value), 16);
    CHECK(rc_service_state(0,0,0,0,0) == RC_SERVICE_DISABLED, 17);
    CHECK(rc_service_state(0,1,0,0,0) == RC_SERVICE_STOPPING, 18);
    CHECK(rc_service_state(0,0,1,0,0) == RC_SERVICE_STOPPING, 19);
    CHECK(rc_service_state(1,0,0,0,0) == RC_SERVICE_STARTING, 20);
    CHECK(rc_service_state(1,1,0,0,0) == RC_SERVICE_ENABLED, 21);
    CHECK(rc_service_state(1,1,3,0,0) == RC_SERVICE_BUSY, 22);
    CHECK(rc_service_state(1,0,0,1,0) == RC_SERVICE_PAUSED, 23);
    CHECK(rc_service_state(1,0,0,0,1) == RC_SERVICE_ERROR, 24);
    for (index = 0; index < sizeof(critical)/sizeof(critical[0]); ++index) CHECK(!rc_service_web_sd_route(critical[index]), 25);
    CHECK(rc_service_web_sd_route("/gallery"), 26);
    CHECK(rc_service_web_sd_route("/api/sd/list"), 27);
    CHECK(rc_service_web_sd_route("/api/sd/archive"), 28);
    CHECK(!rc_service_web_sd_route("/gallery-other"), 29);
    puts("48 saved/reloaded configurations; drain states and protected routes OK");
    return 0;
}
