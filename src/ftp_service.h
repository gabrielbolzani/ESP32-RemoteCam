#pragma once
#include "ftp_protocol.h"
#include <mdns.h>

static constexpr uint16_t FTP_CONTROL_PORT = 21;
static constexpr uint16_t FTP_PASSIVE_FIRST = 50000;
static constexpr unsigned FTP_MAX_SESSIONS = 2;
rcftp_socket ftpListener = RCFTP_BAD;
std::atomic<uint32_t> ftpSessionMask{0};
std::atomic<bool> ftpTransferring{false};
std::atomic<bool> ftpListening{false};
std::atomic<bool> ftpStartFailed{false};
uint32_t ftpLastStartAttempt = 0;
bool ftpAdvertised = false;

struct FtpContext {
    rcftp_socket socket;
    unsigned slot;
    bool leased = false;
};
struct FtpDirectory {
    DIR *directory;
    String path;
    FtpDirectory(DIR *value, const String &folder) : directory(value), path(folder) {}
};

int ftpAvailable(void *opaque) {
    auto *context = static_cast<FtpContext *>(opaque);
    // A transfer already holding the SD lease finishes before this session exits.
    return (context->leased || serviceEnabled(RC_SERVICE_FTP)) &&
        !otaActive.load() && WiFi.status() == WL_CONNECTED;
}
bool ftpStatUnlocked(const String &path, rcftp_info *out) {
    struct stat info;
    if (!validPath(path) || stat(absolutePath(path).c_str(), &info) || info.st_size < 0) return false;
    if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) return false;
    memset(out, 0, sizeof(*out));
    out->directory = S_ISDIR(info.st_mode);
    out->size = out->directory ? 0 : uint64_t(info.st_size);
    struct tm utc = {};
    time_t modified = info.st_mtime;
    if (!gmtime_r(&modified, &utc) || !strftime(out->modified, sizeof(out->modified), "%Y%m%d%H%M%S", &utc)) {
        strlcpy(out->modified, "19700101000000", sizeof(out->modified));
    }
    return true;
}
int ftpStat(void *opaque, const char *path, rcftp_info *out) {
    auto *context = static_cast<FtpContext *>(opaque);
    if (!context->leased && !serviceEnabled(RC_SERVICE_FTP)) return 0;
    State value = snapshotState();
    if (!value.sdMounted || value.maintenance || otaActive) return 0;
    Guard disk(sdMutex);
    value = snapshotState();
    if (!value.sdMounted || value.maintenance || otaActive) return 0;
    return ftpStatUnlocked(path, out);
}
int ftpAcquire(void *opaque, const char *path, int listing, char *reason, size_t capacity) {
    auto *context = static_cast<FtpContext *>(opaque);
    Guard lock(stateMutex);
    if (!serviceEnabled(RC_SERVICE_FTP)) {
        strlcpy(reason, "FTP is being disabled. No new transfers are accepted.", capacity);
        return 0;
    }
    if (!validPath(path) || !state.sdMounted || state.maintenance || state.download || otaActive) {
        strlcpy(reason, "SD busy or unavailable. Try again after the current operation.", capacity);
        return 0;
    }
    if (!listing && recordingBusy(state) && pathContains(state.session, path)) {
        String lower = path;
        lower.toLowerCase();
        if (!lower.endsWith(".jpg") && !lower.endsWith(".jpeg")) {
            strlcpy(reason, "Recording is active. Only completed JPEGs may be downloaded from this session.", capacity);
            return 0;
        }
    }
    state.download = true;
    context->leased = true;
    ftpTransferring = true;
    return 1;
}
void ftpRelease(void *opaque) {
    auto *context = static_cast<FtpContext *>(opaque);
    Guard lock(stateMutex);
    if (context->leased) {
        context->leased = false;
        state.download = false;
        ftpTransferring = false;
    }
}
void *ftpOpenFile(void *, const char *path) {
    Guard disk(sdMutex);
    return fopen(absolutePath(path).c_str(), "rb");
}
int ftpSeekFile(void *, void *file, uint64_t offset) {
    if (offset > LONG_MAX) return 0;
    Guard disk(sdMutex);
    return fseek(static_cast<FILE *>(file), static_cast<long>(offset), SEEK_SET) == 0;
}
int ftpReadFile(void *, void *file, uint8_t *data, size_t size) {
    Guard disk(sdMutex);
    FILE *stream = static_cast<FILE *>(file);
    size_t count = fread(data, 1, size, stream);
    return count == 0 && ferror(stream) ? -1 : int(count);
}
void ftpCloseFile(void *, void *file) {
    Guard disk(sdMutex);
    fclose(static_cast<FILE *>(file));
}
void *ftpOpenDirectory(void *, const char *path) {
    Guard disk(sdMutex);
    DIR *directory = opendir(absolutePath(path).c_str());
    if (!directory) return nullptr;
    auto *cursor = new (std::nothrow) FtpDirectory(directory, path);
    if (!cursor) closedir(directory);
    return cursor;
}
int ftpNextEntry(void *, void *opaque, rcftp_entry *entry) {
    auto *cursor = static_cast<FtpDirectory *>(opaque);
    Guard disk(sdMutex);
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(cursor->directory);
        if (!item) return errno ? -1 : 0;
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        String path = cursor->path + (cursor->path == "/" ? "" : "/") + item->d_name;
        if (!validPath(path) || strlen(item->d_name) > RCFTP_PATH_MAX) continue;
        if (!ftpStatUnlocked(path, &entry->info)) {
            if (errno == ENOENT) continue;
            return -1;
        }
        strlcpy(entry->name, item->d_name, sizeof(entry->name));
        return 1;
    }
}
void ftpCloseDirectory(void *, void *opaque) {
    auto *cursor = static_cast<FtpDirectory *>(opaque);
    {
        Guard disk(sdMutex);
        closedir(cursor->directory);
    }
    delete cursor;
}
void ftpProgress(void *, size_t count) {
    // Cede mais tempo ao vídeo durante downloads, sem segurar o mutex do SD.
    if (count >= 1024 && streamClients.load()) vTaskDelay(pdMS_TO_TICKS(6));
    else vTaskDelay(1);
}
const rcftp_fs ftpFilesystem = {
    ftpAvailable, ftpStat, ftpAcquire, ftpRelease,
    ftpOpenFile, ftpSeekFile, ftpReadFile, ftpCloseFile,
    ftpOpenDirectory, ftpNextEntry, ftpCloseDirectory, ftpProgress
};

void ftpWorker(void *opaque) {
    auto *context = static_cast<FtpContext *>(opaque);
    rcftp_run(context->socket, FTP_PASSIVE_FIRST + context->slot, VERSION, &ftpFilesystem, context);
    ftpRelease(context);
    ftpSessionMask.fetch_and(~(1UL << context->slot));
    delete context;
    vTaskDelete(nullptr);
}
void startFtpServer() {
    if (!serviceEnabled(RC_SERVICE_FTP) || otaActive || ftpListener != RCFTP_BAD) return;
    ftpLastStartAttempt = millis() ? millis() : 1;
    ftpListener = rcftp_listen(htonl(INADDR_ANY), FTP_CONTROL_PORT, FTP_MAX_SESSIONS);
    ftpListening = ftpListener != RCFTP_BAD;
    ftpStartFailed = ftpListener == RCFTP_BAD;
    if (ftpListening.load()) ftpAdvertised = MDNS.addService("ftp", "tcp", FTP_CONTROL_PORT);
    Serial.printf("FTP: %s | porta 21 | passivo 50000-50001 | anonymous/read-only\n", ftpListening.load() ? "ativo" : "indisponivel");
}
void reconcileFtpService() {
    if (!serviceEnabled(RC_SERVICE_FTP) || otaActive.load()) {
        if (ftpListener != RCFTP_BAD) {
            rcftp_close(ftpListener);
            ftpListener = RCFTP_BAD;
        }
        ftpListening = false;
        ftpStartFailed = false;
        ftpLastStartAttempt = 0;
        if (ftpAdvertised) {
            mdns_service_remove("_ftp", "_tcp");
            ftpAdvertised = false;
        }
        return;
    }
    if (ftpListener == RCFTP_BAD && (!ftpLastStartAttempt || millis() - ftpLastStartAttempt >= 2000)) startFtpServer();
}
void serviceFtpClients() {
    reconcileFtpService();
    if (ftpListener == RCFTP_BAD || !serviceEnabled(RC_SERVICE_FTP)) return;
    rcftp_socket client = accept(ftpListener, nullptr, nullptr);
    if (client == RCFTP_BAD) return;
    rcftp_nonblock(client);
    if (otaActive || !serviceEnabled(RC_SERVICE_FTP)) {
        const char unavailable[] = "421 FTP paused for firmware update.\r\n";
        send(client, unavailable, sizeof(unavailable) - 1, 0);
        rcftp_close(client);
        return;
    }
    unsigned slot;
    for (slot = 0; slot < FTP_MAX_SESSIONS; ++slot) {
        uint32_t bit = 1UL << slot;
        uint32_t previous = ftpSessionMask.fetch_or(bit);
        if (!(previous & bit)) break;
    }
    if (slot == FTP_MAX_SESSIONS) {
        const char busy[] = "421 Two FTP sessions are already open. Limit concurrent connections in your client.\r\n";
        send(client, busy, sizeof(busy) - 1, 0);
        rcftp_close(client);
        return;
    }
    auto *context = new (std::nothrow) FtpContext;
    if (context) {
        context->socket = client;
        context->slot = slot;
        if (xTaskCreatePinnedToCore(ftpWorker, "ftp-client", 10240, context, 1, nullptr, 1) == pdPASS) return;
        delete context;
    }
    ftpSessionMask.fetch_and(~(1UL << slot));
    rcftp_close(client);
}
