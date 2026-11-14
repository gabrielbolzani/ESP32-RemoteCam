#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../src/ftp_protocol.h"

typedef struct {
    char root[1024];
    CRITICAL_SECTION mutex;
    LONG sessions;
    int leased;
} test_shared;
typedef struct {
    test_shared *shared;
    SOCKET control;
    int leased;
} test_client;
typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW item;
    int first;
} test_directory;

static int wide_path(test_client *c, const char *path, wchar_t *out, size_t capacity) {
    char joined[1400];
    int length;
    if (snprintf(joined, sizeof(joined), "%s%s", c->shared->root, path) >= (int)sizeof(joined)) return 0;
    length = MultiByteToWideChar(CP_UTF8, 0, joined, -1, out, (int)capacity);
    return length > 0;
}
static void make_info(DWORD attributes, DWORD high, DWORD low, FILETIME *changed, rcftp_info *out) {
    SYSTEMTIME utc;
    memset(out, 0, sizeof(*out));
    out->directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = out->directory ? 0 : ((uint64_t)high << 32) | low;
    FileTimeToSystemTime(changed, &utc);
    snprintf(out->modified, sizeof(out->modified), "%04u%02u%02u%02u%02u%02u", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
}
static int disabled(test_client *client) {
    wchar_t marker[1400];
    return wide_path(client, "/ftp-disabled.flag", marker, 1400) && GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}
static int available(void *context) {
    test_client *client = (test_client *)context;
    return client->leased || !disabled(client);
}
static int file_stat(void *context, const char *path, rcftp_info *out) {
    test_client *client = (test_client *)context;
    WIN32_FILE_ATTRIBUTE_DATA data;
    wchar_t full[1400];
    if (!wide_path(client, path, full, 1400) || !GetFileAttributesExW(full, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    make_info(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, &data.ftLastWriteTime, out);
    return 1;
}
static int acquire(void *context, const char *path, int listing, char *reason, size_t capacity) {
    test_client *client = (test_client *)context;
    wchar_t marker[1400];
    int blocked = 0;
    EnterCriticalSection(&client->shared->mutex);
    wide_path(client, "/busy.flag", marker, 1400);
    if (client->shared->leased || disabled(client) || GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) blocked = 1;
    wide_path(client, "/recording.flag", marker, 1400);
    if (!listing && GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES && !strncmp(path, "/active/", 8)) {
        size_t length = strlen(path);
        if (length < 4 || strcmp(path + length - 4, ".jpg")) blocked = 1;
    }
    if (blocked) {
        snprintf(reason, capacity, "Busy or recording.");
        LeaveCriticalSection(&client->shared->mutex);
        return 0;
    }
    client->shared->leased = 1;
    client->leased = 1;
    LeaveCriticalSection(&client->shared->mutex);
    return 1;
}
static void release(void *context) {
    test_client *client = (test_client *)context;
    EnterCriticalSection(&client->shared->mutex);
    if (client->leased) {
        client->leased = 0;
        client->shared->leased = 0;
    }
    LeaveCriticalSection(&client->shared->mutex);
}
static void *open_file(void *context, const char *path) {
    wchar_t full[1400];
    HANDLE file;
    if (!wide_path((test_client *)context, path, full, 1400)) return NULL;
    file = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    return file == INVALID_HANDLE_VALUE ? NULL : file;
}
static int seek_file(void *context, void *file, uint64_t offset) {
    LARGE_INTEGER position;
    (void)context;
    position.QuadPart = offset;
    return SetFilePointerEx((HANDLE)file, position, NULL, FILE_BEGIN) != 0;
}
static int read_file(void *context, void *file, uint8_t *data, size_t size) {
    DWORD count;
    (void)context;
    return ReadFile((HANDLE)file, data, (DWORD)size, &count, NULL) ? (int)count : -1;
}
static void close_file(void *context, void *file) { (void)context; CloseHandle((HANDLE)file); }
static void *open_dir(void *context, const char *path) {
    test_directory *directory;
    wchar_t full[1400];
    size_t length;
    if (!wide_path((test_client *)context, path, full, 1390)) return NULL;
    length = wcslen(full);
    full[length++] = L'/';
    full[length++] = L'*';
    full[length] = 0;
    directory = (test_directory *)calloc(1, sizeof(*directory));
    if (!directory) return NULL;
    directory->handle = FindFirstFileW(full, &directory->item);
    if (directory->handle == INVALID_HANDLE_VALUE) { free(directory); return NULL; }
    directory->first = 1;
    return directory;
}
static int next_entry(void *context, void *opaque, rcftp_entry *out) {
    test_directory *directory = (test_directory *)opaque;
    (void)context;
    for (;;) {
        WIN32_FIND_DATAW *item = &directory->item;
        if (!directory->first && !FindNextFileW(directory->handle, item)) return GetLastError() == ERROR_NO_MORE_FILES ? 0 : -1;
        directory->first = 0;
        if (!wcscmp(item->cFileName, L".") || !wcscmp(item->cFileName, L"..") || (item->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, item->cFileName, -1, out->name, sizeof(out->name), NULL, NULL)) continue;
        make_info(item->dwFileAttributes, item->nFileSizeHigh, item->nFileSizeLow, &item->ftLastWriteTime, &out->info);
        return 1;
    }
}
static void close_dir(void *context, void *opaque) {
    test_directory *directory = (test_directory *)opaque;
    (void)context;
    FindClose(directory->handle);
    free(directory);
}
static void progress(void *context, size_t count) { (void)context; (void)count; Sleep(1); }
static rcftp_fs filesystem = {available,file_stat,acquire,release,open_file,seek_file,read_file,close_file,open_dir,next_entry,close_dir,progress};
static DWORD WINAPI worker(LPVOID opaque) {
    test_client *client = (test_client *)opaque;
    rcftp_run(client->control, 0, "test", &filesystem, client);
    release(client);
    EnterCriticalSection(&client->shared->mutex);
    --client->shared->sessions;
    LeaveCriticalSection(&client->shared->mutex);
    free(client);
    return 0;
}
int main(int argc, char **argv) {
    WSADATA data;
    test_shared shared;
    SOCKET listener;
    struct sockaddr_in endpoint;
    int size = sizeof(endpoint);
    uint32_t started;
    uint16_t control_port;
    test_client probe;
    char resolved[181];
    if (argc != 2) return 2;
    if (!rcftp_path("/sub", "../arquivo.txt", resolved, sizeof(resolved)) || strcmp(resolved, "/arquivo.txt")) return 3;
    if (rcftp_path("/", "../../fora", resolved, sizeof(resolved))) return 4;
    if (WSAStartup(MAKEWORD(2,2), &data)) return 5;
    memset(&shared, 0, sizeof(shared));
    snprintf(shared.root, sizeof(shared.root), "%s", argv[1]);
    InitializeCriticalSection(&shared.mutex);
    listener = rcftp_listen(inet_addr("127.0.0.1"), 0, 3);
    if (listener == RCFTP_BAD || getsockname(listener, (struct sockaddr *)&endpoint, &size)) return 6;
    control_port = ntohs(endpoint.sin_port);
    memset(&probe, 0, sizeof(probe));
    probe.shared = &shared;
    printf("%u\n", control_port);
    fflush(stdout);
    started = rcftp_now();
    while ((uint32_t)(rcftp_now() - started) < 180000) {
        SOCKET connection;
        if (disabled(&probe)) {
            if (listener != RCFTP_BAD) rcftp_close(listener);
            listener = RCFTP_BAD;
            Sleep(2);
            continue;
        }
        if (listener == RCFTP_BAD) listener = rcftp_listen(inet_addr("127.0.0.1"), control_port, 3);
        if (listener == RCFTP_BAD) { Sleep(2); continue; }
        connection = accept(listener, NULL, NULL);
        if (connection != RCFTP_BAD) {
            test_client *client;
            HANDLE thread;
            rcftp_nonblock(connection);
            int full;
            EnterCriticalSection(&shared.mutex);
            full = shared.sessions >= 2;
            if (!full) ++shared.sessions;
            LeaveCriticalSection(&shared.mutex);
            if (full) {
                const char *message = "421 Two sessions already open.\r\n";
                send(connection, message, (int)strlen(message), 0);
                rcftp_close(connection);
                continue;
            }
            client = (test_client *)calloc(1, sizeof(*client));
            if (!client) return 7;
            client->shared = &shared;
            client->control = connection;
            thread = CreateThread(NULL, 0, worker, client, 0, NULL);
            if (!thread) return 8;
            CloseHandle(thread);
        }
        Sleep(1);
    }
    rcftp_close(listener);
    return 0;
}
