#ifndef REMOTECAM_FTP_PROTOCOL_H
#define REMOTECAM_FTP_PROTOCOL_H

/* Read-only FTP: RFC 959, RFC 2428 (PASV/EPSV), RFC 3659 (MLSD/REST).
 * No storage writes and no active-mode connections. One session owns its
 * control/data sockets. Filesystem policy and synchronization belong to hooks. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
typedef SOCKET rcftp_socket;
typedef int rcftp_socklen;
#define RCFTP_BAD INVALID_SOCKET
static inline void rcftp_close(rcftp_socket fd) { if (fd != RCFTP_BAD) closesocket(fd); }
static inline uint32_t rcftp_now(void) { return GetTickCount(); }
static inline void rcftp_delay(unsigned ms) { Sleep(ms); }
static inline int rcftp_again(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINTR; }
static inline int rcftp_nonblock(rcftp_socket fd) { u_long enabled = 1; return ioctlsocket(fd, FIONBIO, &enabled) == 0; }
#else
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/tcp.h>
#include <unistd.h>
#include <errno.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
typedef int rcftp_socket;
typedef socklen_t rcftp_socklen;
#define RCFTP_BAD (-1)
static inline void rcftp_close(rcftp_socket fd) { if (fd != RCFTP_BAD) close(fd); }
static inline uint32_t rcftp_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static inline void rcftp_delay(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
static inline int rcftp_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static inline int rcftp_nonblock(rcftp_socket fd) { int enabled = 1; return ioctl(fd, FIONBIO, &enabled) == 0; }
#endif

#define RCFTP_PATH_MAX 180
#define RCFTP_LINE_MAX 512
#define RCFTP_IDLE_MS 180000UL
#define RCFTP_DATA_WAIT_MS 20000UL

typedef struct {
    uint64_t size;
    int directory;
    char modified[15];
} rcftp_info;
typedef struct {
    char name[RCFTP_PATH_MAX + 1];
    rcftp_info info;
} rcftp_entry;
typedef struct {
    int (*available)(void *);
    int (*stat)(void *, const char *, rcftp_info *);
    int (*acquire)(void *, const char *, int, char *, size_t);
    void (*release)(void *);
    void *(*open_file)(void *, const char *);
    int (*seek_file)(void *, void *, uint64_t);
    int (*read_file)(void *, void *, uint8_t *, size_t);
    void (*close_file)(void *, void *);
    void *(*open_dir)(void *, const char *);
    int (*next_entry)(void *, void *, rcftp_entry *);
    void (*close_dir)(void *, void *);
    void (*progress)(void *, size_t);
} rcftp_fs;
typedef struct {
    rcftp_socket control;
    rcftp_socket passive;
    rcftp_socket data;
    uint16_t passive_port;
    uint32_t peer_ip;
    uint32_t local_ip;
    uint32_t passive_since;
    uint64_t restart;
    int user_accepted;
    int logged_in;
    int ascii;
    int epsv_only;
    int failed;
    char cwd[RCFTP_PATH_MAX + 1];
    const rcftp_fs *fs;
    void *context;
} rcftp_session;

static inline int rcftp_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (toupper((unsigned char)*left++) != toupper((unsigned char)*right++)) return 0;
    }
    return !*left && !*right;
}
static inline int rcftp_path(const char *cwd, const char *argument, char *output, size_t capacity) {
    char joined[RCFTP_PATH_MAX * 2 + 3];
    const char *cursor;
    size_t length = 1;
    if (!argument || !*argument) argument = ".";
    if (strlen(argument) > RCFTP_PATH_MAX || strlen(cwd) > RCFTP_PATH_MAX || capacity < 2) return 0;
    if (*argument == '/') snprintf(joined, sizeof(joined), "%s", argument);
    else snprintf(joined, sizeof(joined), "%s/%s", cwd, argument);
    output[0] = '/';
    output[1] = 0;
    cursor = joined;
    while (*cursor) {
        const char *start;
        size_t size, i;
        while (*cursor == '/') ++cursor;
        if (!*cursor) break;
        start = cursor;
        while (*cursor && *cursor != '/') ++cursor;
        size = (size_t)(cursor - start);
        if (size == 1 && start[0] == '.') continue;
        if (size == 2 && start[0] == '.' && start[1] == '.') {
            if (length == 1) return 0;
            while (length > 1 && output[length - 1] != '/') --length;
            if (length > 1) --length;
            output[length] = 0;
            continue;
        }
        if (!size || start[size - 1] == '.' || start[size - 1] == ' ') return 0;
        for (i = 0; i < size; ++i) {
            unsigned char c = (unsigned char)start[i];
            if (c < 32 || c == 127 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
        }
        if (length + (length > 1 ? 1 : 0) + size > RCFTP_PATH_MAX || length + (length > 1 ? 1 : 0) + size >= capacity) return 0;
        if (length > 1) output[length++] = '/';
        memcpy(output + length, start, size);
        length += size;
        output[length] = 0;
    }
    return 1;
}
static inline int rcftp_number(const char *text, uint64_t *value) {
    uint64_t result = 0;
    if (!text || !*text) return 0;
    while (*text) {
        unsigned digit;
        if (*text < '0' || *text > '9') return 0;
        digit = (unsigned)(*text++ - '0');
        if (result > (UINT64_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    *value = result;
    return 1;
}
static inline int rcftp_send(rcftp_socket socket, const void *data, size_t size) {
    const char *bytes = (const char *)data;
    size_t sent = 0;
    uint32_t progress = rcftp_now();
    while (sent < size) {
        int count = send(socket, bytes + sent, (int)(size - sent > 4096 ? 4096 : size - sent), 0);
        if (count > 0) {
            sent += (size_t)count;
            progress = rcftp_now();
        } else if (count == 0 || !rcftp_again()) return 0;
        if ((uint32_t)(rcftp_now() - progress) > 5000) return 0;
        if (sent < size) rcftp_delay(1);
    }
    return 1;
}
static inline int rcftp_reply(rcftp_session *s, int code, const char *format, ...) {
    char text[600];
    char line[640];
    va_list arguments;
    int size, length;
    va_start(arguments, format);
    size = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (size < 0 || (size_t)size >= sizeof(text)) return 0;
    length = snprintf(line, sizeof(line), "%d %s\r\n", code, text);
    if (length < 0 || (size_t)length >= sizeof(line)) return 0;
    if (!rcftp_send(s->control, line, (size_t)length)) { s->failed = 1; return 0; }
    return 1;
}
static inline int rcftp_connected(rcftp_socket socket) {
    char byte;
    int count = recv(socket, &byte, 1, MSG_PEEK);
    return count > 0 || (count < 0 && rcftp_again());
}
static inline void rcftp_end_data(rcftp_session *s) {
    rcftp_close(s->data);
    rcftp_close(s->passive);
    s->data = RCFTP_BAD;
    s->passive = RCFTP_BAD;
    s->passive_since = 0;
}
static inline rcftp_socket rcftp_listen(uint32_t address, uint16_t port, int backlog) {
    struct sockaddr_in endpoint;
    rcftp_socket socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int enabled = 1;
    if (socket_fd == RCFTP_BAD) return RCFTP_BAD;
    memset(&endpoint, 0, sizeof(endpoint));
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = address;
    endpoint.sin_port = htons(port);
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&enabled, sizeof(enabled));
    if (!rcftp_nonblock(socket_fd) || bind(socket_fd, (struct sockaddr *)&endpoint, sizeof(endpoint)) || listen(socket_fd, backlog)) {
        rcftp_close(socket_fd);
        return RCFTP_BAD;
    }
    return socket_fd;
}
static inline int rcftp_pasv(rcftp_session *s, int extended) {
    struct sockaddr_in endpoint;
    rcftp_socklen size = sizeof(endpoint);
    uint16_t port;
    uint32_t ip;
    rcftp_end_data(s);
    s->passive = rcftp_listen(s->local_ip, s->passive_port, 1);
    if (s->passive == RCFTP_BAD || getsockname(s->passive, (struct sockaddr *)&endpoint, &size)) {
        rcftp_end_data(s);
        return rcftp_reply(s, 425, "Cannot open passive data port.");
    }
    s->passive_since = rcftp_now();
    port = ntohs(endpoint.sin_port);
    if (extended) return rcftp_reply(s, 229, "Entering Extended Passive Mode (|||%u|).", port);
    ip = ntohl(s->local_ip);
    return rcftp_reply(s, 227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u).", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), port >> 8, port & 255);
}
static inline int rcftp_accept_data(rcftp_session *s) {
    uint32_t started = rcftp_now();
    if (s->passive == RCFTP_BAD) return 0;
    while ((uint32_t)(rcftp_now() - started) < RCFTP_DATA_WAIT_MS) {
        struct sockaddr_in peer;
        rcftp_socklen size = sizeof(peer);
        rcftp_socket fd = accept(s->passive, (struct sockaddr *)&peer, &size);
        if (fd != RCFTP_BAD) {
            int enabled = 1;
            if (peer.sin_addr.s_addr != s->peer_ip || !rcftp_nonblock(fd)) {
                rcftp_close(fd);
                continue;
            }
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled, sizeof(enabled));
            s->data = fd;
            rcftp_close(s->passive);
            s->passive = RCFTP_BAD;
            return 1;
        }
        if (!rcftp_again() || !rcftp_connected(s->control)) return 0;
        rcftp_delay(5);
    }
    return 0;
}
static inline int rcftp_facts(char *buffer, size_t capacity, const rcftp_info *info, const char *name) {
    return snprintf(buffer, capacity, "type=%s;size=%llu;modify=%s;perm=%s; %s\r\n", info->directory ? "dir" : "file", (unsigned long long)info->size, info->modified, info->directory ? "el" : "r", name);
}
static inline int rcftp_listing(rcftp_session *s, const char *path, int mode) {
    rcftp_info info;
    rcftp_entry entry;
    void *directory = NULL;
    int result = 1;
    int count;
    char reason[160] = "SD busy or unavailable.";
    char line[640];
    uint8_t *batch = NULL;
    size_t batch_used = 0;
    if (s->passive == RCFTP_BAD) return rcftp_reply(s, 425, "Use PASV or EPSV first.");
    if (!s->fs->acquire(s->context, path, 1, reason, sizeof(reason))) {
        rcftp_end_data(s);
        return rcftp_reply(s, 450, "%s", reason);
    }
    if (!s->fs->stat(s->context, path, &info) || !info.directory || !(directory = s->fs->open_dir(s->context, path))) {
        s->fs->release(s->context);
        rcftp_end_data(s);
        return rcftp_reply(s, 550, "Directory unavailable.");
    }
    batch = (uint8_t *)malloc(2048);
    if (!batch) {
        s->fs->close_dir(s->context, directory);
        s->fs->release(s->context);
        rcftp_end_data(s);
        return rcftp_reply(s, 451, "Not enough memory for listing.");
    }
    s->restart = 0;
    if (!rcftp_reply(s, 150, "Opening directory listing.") || !rcftp_accept_data(s)) {
        free(batch);
        s->fs->close_dir(s->context, directory);
        s->fs->release(s->context);
        rcftp_end_data(s);
        return rcftp_reply(s, 425, "Data connection unavailable.");
    }
    while ((count = s->fs->next_entry(s->context, directory, &entry)) > 0) {
        int size;
        if (mode == 2) size = rcftp_facts(line, sizeof(line), &entry.info, entry.name);
        else if (mode == 1) size = snprintf(line, sizeof(line), "%s\r\n", entry.name);
        else {
            static const char *months[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
            unsigned month = 1, day = 1, hour = 0, minute = 0;
            if (strlen(entry.info.modified) == 14) {
                month = (entry.info.modified[4] - '0') * 10 + entry.info.modified[5] - '0';
                day = (entry.info.modified[6] - '0') * 10 + entry.info.modified[7] - '0';
                hour = (entry.info.modified[8] - '0') * 10 + entry.info.modified[9] - '0';
                minute = (entry.info.modified[10] - '0') * 10 + entry.info.modified[11] - '0';
            }
            if (month < 1 || month > 12) month = 1;
            size = snprintf(line, sizeof(line), "%s 1 ftp ftp %llu %s %2u %02u:%02u %s\r\n", entry.info.directory ? "dr-xr-xr-x" : "-r--r--r--", (unsigned long long)entry.info.size, months[month - 1], day, hour, minute, entry.name);
        }
        if (size < 0 || (size_t)size >= sizeof(line)) { result = 0; break; }
        if (batch_used + (size_t)size > 2048) {
            if (!rcftp_send(s->data, batch, batch_used)) { result = 0; break; }
            if (s->fs->progress) s->fs->progress(s->context, batch_used);
            batch_used = 0;
            rcftp_delay(1);
        }
        memcpy(batch + batch_used, line, (size_t)size);
        batch_used += (size_t)size;
    }
    if (count < 0) result = 0;
    if (result && batch_used && !rcftp_send(s->data, batch, batch_used)) result = 0;
    free(batch);
    s->fs->close_dir(s->context, directory);
    rcftp_end_data(s);
    s->fs->release(s->context);
    return rcftp_reply(s, result ? 226 : 426, result ? "Listing complete." : "Listing interrupted.");
}
static inline int rcftp_retrieve(rcftp_session *s, const char *path) {
    rcftp_info info;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    void *file = NULL;
    uint64_t remaining;
    uint64_t restart = s->restart;
    int success = 1;
    int pending_cr = 0;
    char reason[160] = "SD busy or unavailable.";
    s->restart = 0;
    if (s->passive == RCFTP_BAD) return rcftp_reply(s, 425, "Use PASV or EPSV first.");
    if (!s->fs->acquire(s->context, path, 0, reason, sizeof(reason))) {
        rcftp_end_data(s);
        return rcftp_reply(s, 450, "%s", reason);
    }
    if (!s->fs->stat(s->context, path, &info) || info.directory || restart > info.size || (s->ascii && restart) || !(file = s->fs->open_file(s->context, path))) {
        rcftp_end_data(s);
        s->fs->release(s->context);
        return rcftp_reply(s, 550, "File or restart position unavailable.");
    }
    input = (uint8_t *)malloc(2048);
    if (s->ascii) output = (uint8_t *)malloc(4100);
    if (!input || (s->ascii && !output) || !s->fs->seek_file(s->context, file, restart)) {
        free(input);
        free(output);
        s->fs->close_file(s->context, file);
        rcftp_end_data(s);
        s->fs->release(s->context);
        return rcftp_reply(s, 451, "Cannot prepare file transfer.");
    }
    if (!rcftp_reply(s, 150, "Opening %s file transfer.", s->ascii ? "ASCII" : "binary") || !rcftp_accept_data(s)) {
        free(input);
        free(output);
        s->fs->close_file(s->context, file);
        rcftp_end_data(s);
        s->fs->release(s->context);
        return rcftp_reply(s, 425, "Data connection unavailable.");
    }
    remaining = info.size - restart;
    while (remaining) {
        size_t wanted = remaining > 2048 ? 2048 : (size_t)remaining;
        int count = s->fs->read_file(s->context, file, input, wanted);
        if (count <= 0 || (size_t)count > wanted) { success = 0; break; }
        if (s->ascii) {
            size_t written = 0;
            int i;
            for (i = 0; i < count; ++i) {
                uint8_t c = input[i];
                if (pending_cr) {
                    output[written++] = '\r';
                    output[written++] = c == '\n' ? '\n' : 0;
                    pending_cr = 0;
                    if (c == '\n') continue;
                }
                if (c == '\r') pending_cr = 1;
                else if (c == '\n') { output[written++] = '\r'; output[written++] = '\n'; }
                else output[written++] = c;
            }
            if (written && !rcftp_send(s->data, output, written)) { success = 0; break; }
        } else if (!rcftp_send(s->data, input, (size_t)count)) { success = 0; break; }
        remaining -= (size_t)count;
        if (s->fs->progress) s->fs->progress(s->context, (size_t)count);
        rcftp_delay(1);
    }
    if (success && pending_cr && !rcftp_send(s->data, "\r\0", 2)) success = 0;
    free(input);
    free(output);
    s->fs->close_file(s->context, file);
    rcftp_end_data(s);
    s->fs->release(s->context);
    return rcftp_reply(s, success ? 226 : 426, success ? "Transfer complete." : "Transfer interrupted.");
}
static inline int rcftp_command(rcftp_session *s, char *line) {
    char *argument;
    char path[RCFTP_PATH_MAX + 1];
    rcftp_info info;
    size_t n;
    while (*line == ' ') ++line;
    argument = strchr(line, ' ');
    if (argument) { *argument++ = 0; while (*argument == ' ') ++argument; }
    else argument = line + strlen(line);
    if (rcftp_equal(line, "QUIT")) { rcftp_reply(s, 221, "Goodbye."); return 0; }
    if (rcftp_equal(line, "USER")) {
        s->logged_in = 0;
        s->restart = 0;
        strcpy(s->cwd, "/");
        rcftp_end_data(s);
        s->user_accepted = rcftp_equal(argument, "anonymous") || rcftp_equal(argument, "ftp");
        return rcftp_reply(s, s->user_accepted ? 331 : 530, s->user_accepted ? "Anonymous access; send any password." : "Use anonymous access.");
    }
    if (rcftp_equal(line, "PASS")) {
        if (!s->user_accepted) return rcftp_reply(s, 503, "Send USER anonymous first.");
        s->logged_in = 1;
        return rcftp_reply(s, 230, "Anonymous read-only access enabled.");
    }
    if (rcftp_equal(line, "SYST")) return rcftp_reply(s, 215, "UNIX Type: L8");
    if (rcftp_equal(line, "FEAT")) {
        const char *features = "211-Features\r\n UTF8\r\n EPSV\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n MLST type*;size*;modify*;perm*;\r\n211 End\r\n";
        return rcftp_send(s->control, features, strlen(features));
    }
    if (rcftp_equal(line, "AUTH")) return rcftp_reply(s, 502, "TLS/SFTP not supported; local plain FTP only.");
    if (!s->logged_in) return rcftp_reply(s, 530, "Login with USER anonymous and PASS first.");
    if (rcftp_equal(line, "NOOP") || rcftp_equal(line, "CLNT")) return rcftp_reply(s, 200, "OK");
    if (rcftp_equal(line, "HELP")) return rcftp_reply(s, 214, "Read-only FTP: PWD CWD CDUP PASV EPSV LIST NLST MLSD MLST RETR SIZE MDTM REST TYPE QUIT");
    if (rcftp_equal(line, "OPTS")) return rcftp_reply(s, (rcftp_equal(argument, "UTF8 ON") || strncmp(argument, "MLST ", 5) == 0) ? 200 : 501, "Options processed.");
    if (rcftp_equal(line, "TYPE")) {
        if (rcftp_equal(argument, "I") || rcftp_equal(argument, "L 8")) s->ascii = 0;
        else if (rcftp_equal(argument, "A")) s->ascii = 1;
        else return rcftp_reply(s, 504, "Use TYPE I or A.");
        return rcftp_reply(s, 200, "Transfer type set.");
    }
    if (rcftp_equal(line, "MODE")) return rcftp_reply(s, rcftp_equal(argument, "S") ? 200 : 504, "Stream mode only.");
    if (rcftp_equal(line, "STRU")) return rcftp_reply(s, rcftp_equal(argument, "F") ? 200 : 504, "File structure only.");
    if (rcftp_equal(line, "PWD") || rcftp_equal(line, "XPWD")) return rcftp_reply(s, 257, "\"%s\"", s->cwd);
    if (rcftp_equal(line, "PASV")) {
        if (s->epsv_only) return rcftp_reply(s, 501, "EPSV ALL is active.");
        return rcftp_pasv(s, 0);
    }
    if (rcftp_equal(line, "EPSV")) {
        if (rcftp_equal(argument, "ALL")) { s->epsv_only = 1; return rcftp_reply(s, 200, "EPSV ALL accepted."); }
        if (*argument && strcmp(argument, "1")) return rcftp_reply(s, 522, "IPv4 only; use (1).");
        return rcftp_pasv(s, 1);
    }
    if (rcftp_equal(line, "PORT") || rcftp_equal(line, "EPRT")) return rcftp_reply(s, 502, "Active mode disabled; use PASV or EPSV.");
    if (rcftp_equal(line, "ABOR")) { rcftp_end_data(s); s->restart = 0; return rcftp_reply(s, 226, "No transfer pending."); }
    if (rcftp_equal(line, "REST")) {
        uint64_t restart;
        if (s->ascii || !rcftp_number(argument, &restart)) return rcftp_reply(s, 501, "REST requires TYPE I and a nonnegative byte offset.");
        s->restart = restart;
        return rcftp_reply(s, 350, "Restart position accepted.");
    }
    if (rcftp_equal(line, "STOR") || rcftp_equal(line, "APPE") || rcftp_equal(line, "STOU") || rcftp_equal(line, "DELE") || rcftp_equal(line, "RNFR") || rcftp_equal(line, "RNTO") || rcftp_equal(line, "MKD") || rcftp_equal(line, "XMKD") || rcftp_equal(line, "RMD") || rcftp_equal(line, "XRMD") || rcftp_equal(line, "SITE")) {
        rcftp_end_data(s);
        return rcftp_reply(s, 550, "Read-only FTP. Use the web interface to modify the SD card.");
    }
    if (rcftp_equal(line, "STAT") && !*argument) return rcftp_reply(s, 211, "RemoteCam read-only FTP ready.");
    if (rcftp_equal(line, "CDUP") || rcftp_equal(line, "XCUP")) {
        if (!strcmp(s->cwd, "/")) return rcftp_reply(s, 250, "At SD root.");
        argument = (char *)"..";
        line = (char *)"CWD";
    }
    if (rcftp_equal(line, "LIST") || rcftp_equal(line, "NLST")) {
        if (!strcmp(argument, "-a") || !strcmp(argument, "-l") || !strcmp(argument, "-al") || !strcmp(argument, "-la")) argument = (char *)"";
    }
    if (!rcftp_path(s->cwd, argument, path, sizeof(path))) {
        if (rcftp_equal(line, "RETR") || rcftp_equal(line, "LIST") || rcftp_equal(line, "NLST") || rcftp_equal(line, "MLSD")) {
            s->restart = 0;
            rcftp_end_data(s);
        }
        return rcftp_reply(s, 550, "Invalid path or path outside SD root.");
    }
    if (rcftp_equal(line, "CWD") || rcftp_equal(line, "XCWD")) {
        if (!s->fs->stat(s->context, path, &info) || !info.directory) return rcftp_reply(s, 550, "Directory unavailable.");
        memcpy(s->cwd, path, strlen(path) + 1);
        return rcftp_reply(s, 250, "Directory changed.");
    }
    if (rcftp_equal(line, "LIST") || rcftp_equal(line, "NLST") || rcftp_equal(line, "MLSD")) return rcftp_listing(s, path, rcftp_equal(line, "MLSD") ? 2 : (rcftp_equal(line, "NLST") ? 1 : 0));
    if (rcftp_equal(line, "RETR")) return rcftp_retrieve(s, path);
    if (rcftp_equal(line, "SIZE") || rcftp_equal(line, "MDTM") || rcftp_equal(line, "MLST")) {
        if (!s->fs->stat(s->context, path, &info)) return rcftp_reply(s, 550, "Item unavailable.");
        if (rcftp_equal(line, "SIZE")) {
            if (info.directory || s->ascii) return rcftp_reply(s, 550, "SIZE requires a file and TYPE I.");
            return rcftp_reply(s, 213, "%llu", (unsigned long long)info.size);
        }
        if (rcftp_equal(line, "MDTM")) return rcftp_reply(s, 213, "%s", info.modified);
        {
            char facts[640];
            const char *heading = "250-Listing\r\n ";
            int size = rcftp_facts(facts, sizeof(facts), &info, path);
            if (size < 0 || (size_t)size >= sizeof(facts)) return 0;
            return rcftp_send(s->control, heading, strlen(heading)) && rcftp_send(s->control, facts, (size_t)size) && rcftp_send(s->control, "250 End\r\n", 9);
        }
    }
    n = strlen(line);
    return rcftp_reply(s, 502, n ? "Command not implemented." : "Empty command.");
}

/* Takes ownership of the control socket and closes all sockets on return. */
static inline void rcftp_run(rcftp_socket control, uint16_t passive_port, const char *version, const rcftp_fs *fs, void *context) {
    rcftp_session s;
    struct sockaddr_in address;
    rcftp_socklen size = sizeof(address);
    char line[RCFTP_LINE_MAX + 1];
    size_t used = 0;
    int overflow = 0;
    int telnet = 0;
    uint32_t last = rcftp_now();
    uint32_t line_started = 0;
    int enabled = 1;
    memset(&s, 0, sizeof(s));
    s.control = control;
    s.passive = RCFTP_BAD;
    s.data = RCFTP_BAD;
    s.passive_port = passive_port;
    s.fs = fs;
    s.context = context;
    s.ascii = 0;
    strcpy(s.cwd, "/");
    if (!rcftp_nonblock(control) || getsockname(control, (struct sockaddr *)&address, &size)) { rcftp_close(control); return; }
    s.local_ip = address.sin_addr.s_addr;
    size = sizeof(address);
    if (getpeername(control, (struct sockaddr *)&address, &size)) { rcftp_close(control); return; }
    s.peer_ip = address.sin_addr.s_addr;
    setsockopt(control, IPPROTO_TCP, TCP_NODELAY, (const char *)&enabled, sizeof(enabled));
    setsockopt(control, SOL_SOCKET, SO_OOBINLINE, (const char *)&enabled, sizeof(enabled));
    if (!rcftp_reply(&s, 220, "RemoteCam %s - anonymous read-only FTP", version)) goto finished;
    for (;;) {
        unsigned char c;
        int received;
        uint32_t now = rcftp_now();
        if (fs->available && !fs->available(context)) { rcftp_reply(&s, 421, "Service temporarily unavailable."); break; }
        if (s.passive != RCFTP_BAD && (uint32_t)(now - s.passive_since) > 30000) rcftp_end_data(&s);
        if ((uint32_t)(now - last) > RCFTP_IDLE_MS || (line_started && (uint32_t)(now - line_started) > 10000)) {
            rcftp_reply(&s, 421, "Control connection timed out.");
            break;
        }
        received = recv(control, (char *)&c, 1, 0);
        if (received == 0) break;
        if (received < 0) {
            if (!rcftp_again()) break;
            rcftp_delay(10);
            continue;
        }
        if (!line_started) line_started = now ? now : 1;
        if (telnet) { telnet = 0; continue; }
        if (c == 255) { telnet = 1; continue; }
        if (c == '\r') continue;
        if (c == '\n') {
            int keep = 1;
            line[used] = 0;
            if (overflow) keep = rcftp_reply(&s, 500, "Command too long or invalid.");
            else keep = rcftp_command(&s, line);
            memset(line, 0, sizeof(line));
            used = 0;
            overflow = 0;
            line_started = 0;
            last = rcftp_now();
            if (!keep || s.failed) break;
        } else if (c < 32 || c == 127 || used >= RCFTP_LINE_MAX) {
            overflow = 1;
        } else {
            line[used++] = (char)c;
        }
    }
finished:
    rcftp_end_data(&s);
    fs->release(context);
    rcftp_close(control);
}
#endif
