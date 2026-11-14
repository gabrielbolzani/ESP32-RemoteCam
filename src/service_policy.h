#ifndef REMOTECAM_SERVICE_POLICY_H
#define REMOTECAM_SERVICE_POLICY_H
#include <stdint.h>
#include <string.h>

/* One NVS word: four optional services + one refresh interval. Critical
 * Wi-Fi/HTTP/stream/snapshot services intentionally have no persisted bits. */
#define RC_SERVICE_MAGIC 0x53560000UL
#define RC_SERVICE_FTP 1UL
#define RC_SERVICE_WEB_SD 2UL
#define RC_SERVICE_OTA 4UL
#define RC_SERVICE_PANEL_REFRESH 8UL
#define RC_SERVICE_INTERVAL_MASK 0x30UL
#define RC_SERVICE_DEFAULT (RC_SERVICE_MAGIC | 0x0fUL)

static inline int rc_service_valid(uint32_t value) {
    return (value & 0xffff0000UL) == RC_SERVICE_MAGIC &&
        (value & 0x0000ffc0UL) == 0 && ((value >> 4) & 3UL) < 3;
}
static inline uint32_t rc_service_load(uint32_t stored) {
    return rc_service_valid(stored) ? stored : RC_SERVICE_DEFAULT;
}
static inline int rc_service_enabled(uint32_t value, uint32_t flag) {
    return (value & flag) != 0;
}
static inline unsigned rc_service_interval(uint32_t value) {
    unsigned code = (value >> 4) & 3;
    return code == 1 ? 5 : (code == 2 ? 10 : 2);
}
static inline int rc_service_set_interval(uint32_t value, unsigned seconds, uint32_t *result) {
    uint32_t code;
    if (seconds == 2) code = 0;
    else if (seconds == 5) code = 1;
    else if (seconds == 10) code = 2;
    else return 0;
    *result = (value & ~RC_SERVICE_INTERVAL_MASK) | (code << 4);
    return 1;
}

static inline int rc_service_web_sd_route(const char *path) {
    return strcmp(path, "/gallery") == 0 || strncmp(path, "/api/sd/", 8) == 0;
}

enum rc_service_phase {
    RC_SERVICE_DISABLED,
    RC_SERVICE_STARTING,
    RC_SERVICE_ENABLED,
    RC_SERVICE_BUSY,
    RC_SERVICE_STOPPING,
    RC_SERVICE_PAUSED,
    RC_SERVICE_ERROR
};
static inline enum rc_service_phase rc_service_state(int requested, int actual, unsigned active, int paused, int failed) {
    if (!requested) return actual || active ? RC_SERVICE_STOPPING : RC_SERVICE_DISABLED;
    if (paused) return RC_SERVICE_PAUSED;
    if (failed) return RC_SERVICE_ERROR;
    if (!actual) return RC_SERVICE_STARTING;
    return active ? RC_SERVICE_BUSY : RC_SERVICE_ENABLED;
}
static inline const char *rc_service_state_name(enum rc_service_phase state) {
    switch (state) {
        case RC_SERVICE_DISABLED: return "disabled";
        case RC_SERVICE_STARTING: return "starting";
        case RC_SERVICE_ENABLED: return "enabled";
        case RC_SERVICE_BUSY: return "busy";
        case RC_SERVICE_STOPPING: return "stopping";
        case RC_SERVICE_PAUSED: return "paused";
        default: return "error";
    }
}
#endif
