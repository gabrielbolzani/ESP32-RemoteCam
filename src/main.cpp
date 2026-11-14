#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_camera.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <driver/gpio.h>
#include <sdmmc_cmd.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <ff.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include "camera_pins.h"
#include "web_ui.h"
#include "firmware_version.h"
#include "http_transport.h"
#include "zip_store.h"
#include "service_policy.h"
#include <new>
#include <cmath>
#include <memory>

static const char *VERSION = FIRMWARE_VERSION;
static constexpr int LED_CHANNEL = 2;
static constexpr size_t SLOT_BYTES = 384 * 1024;
static constexpr int SLOT_COUNT = 3;
static constexpr uint64_t SPACE_RESERVE = 2 * 1024 * 1024;
static constexpr char SD_BASE[] = "/sdcard";
static constexpr char ROOT_TL[] = "/sdcard/timelapse";

struct Profile {
    int resolution = FRAMESIZE_VGA;
    int quality = 16;
    int brightness = 0;
    int contrast = 0;
    int saturation = 0;
    bool vflip = false;
    bool mirror = false;
};
struct State {
    Profile profile;
    Profile recordingProfile;
    bool ready = false;
    bool recordingFlash = false;
    bool photoPriority = false;
    bool sdPresent = false;
    bool sdMounted = false;
    bool maintenance = false;
    bool writing = false;
    bool download = false;
    int intervalSeconds = 10;
    int ledPct = 25;
    bool ledEnabled = false;
    bool ledOverride = false;
    bool autoFlash = false;
    int actualWidth = 0;
    int actualHeight = 0;
    uint32_t captured = 0;
    uint32_t sent = 0;
    uint32_t captureErrors = 0;
    uint32_t dropped = 0;
    uint32_t saved = 0;
    uint32_t missed = 0;
    uint32_t maxCaptureMs = 0;
    uint32_t captureMs = 0;
    uint32_t sendMs = 0;
    uint32_t frameBytes = 0;
    uint32_t jobId = 0;
    uint32_t cardSerial = 0;
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t sessionBytes = 0;
    int64_t startedUs = 0;
    int64_t endedUs = 0;
    int64_t nextUs = 0;
    char phase[20] = "idle";
    char job[32] = "idle";
    char session[181] = "";
    char error[180] = "";
};
State state;
enum class StorageOperation { DeleteTree, Rename };
struct StorageCommand {
    StorageOperation operation;
    uint32_t id;
    char path[181];
    char destination[181];
};
struct StorageJobStatus {
    uint32_t id = 0;
    uint32_t processed = 0;
    uint32_t total = 0;
    char operation[16] = "";
    char phase[24] = "idle";
    char path[181] = "";
    char destination[181] = "";
    char error[160] = "";
};
StorageJobStatus storageJob;
QueueHandle_t storageCommandQueue;

SemaphoreHandle_t stateMutex;
SemaphoreHandle_t commandMutex;
SemaphoreHandle_t servicesMutex;
SemaphoreHandle_t sdMutex;
SemaphoreHandle_t preferencesMutex;
SemaphoreHandle_t ledMutex;
Preferences preferences;
std::atomic<uint32_t> serviceConfiguration{RC_SERVICE_DEFAULT};
std::atomic<uint32_t> serviceRevision{0};
std::atomic<bool> servicePreferencesValid{false};
uint32_t serviceBootId = 0;
std::atomic<uint32_t> webSdRequests{0};
std::atomic<uint32_t> webSdJobs{0};
std::atomic<uint32_t> otaRequests{0};

bool serviceEnabled(uint32_t flag) {
    return rc_service_enabled(serviceConfiguration.load(), flag);
}
char hostname[33] = "webcam";
String formatChallenge;
int64_t formatExpires = 0;
uint32_t formatSerial = 0;
std::atomic<uint32_t> pendingSaves{0};
std::atomic<uint32_t> streamClients{0};
std::atomic<uint32_t> streamGeneration{0};
std::atomic<uint32_t> httpWorkers{0};
static constexpr uint32_t MAX_STREAM_CLIENTS = 2;
static constexpr uint32_t MAX_HTTP_WORKERS = 6;
WiFiServer httpServer(80);
std::atomic<bool> stopRecording{false};
std::atomic<bool> formatRequested{false};
std::atomic<bool> remountRequested{false};
std::atomic<bool> otaActive{false};
std::atomic<bool> cameraPaused{false};
std::atomic<int> snapshotsWaiting{0};
std::atomic<uint32_t> rebootAt{0};
std::atomic<uint32_t> ledChangedAt{0};
sdmmc_card_t *sdCard = nullptr;
FATFS *sdFilesystem = nullptr;
BYTE sdDrive = 0xff;
char driveName[4] = "";
bool hostInitialized = false;

class Guard {
public:
    explicit Guard(SemaphoreHandle_t mutex, bool enabled = true) : value(enabled ? mutex : nullptr) {
        if (value) xSemaphoreTake(value, portMAX_DELAY);
    }
    void release() {
        if (value) {
            xSemaphoreGive(value);
            value = nullptr;
        }
    }
    ~Guard() { release(); }
private:
    SemaphoreHandle_t value;
};
// The second check closes the race between an OFF request and admission.
// Already admitted operations keep their lease and are allowed to finish.
class ServiceLease {
public:
    ServiceLease(uint32_t flag, std::atomic<uint32_t> &counter, bool required = true)
        : activity(counter), counted(false), accepted(!required) {
        if (!required || !serviceEnabled(flag) || otaActive.load()) return;
        activity.fetch_add(1);
        if (serviceEnabled(flag) && !otaActive.load()) {
            counted = true;
            accepted = true;
        } else activity.fetch_sub(1);
    }
    ~ServiceLease() { if (counted) activity.fetch_sub(1); }
    bool allowed() const { return accepted; }
private:
    std::atomic<uint32_t> &activity;
    bool counted;
    bool accepted;
};

bool isWebSdRoute(const String &path) {
    return rc_service_web_sd_route(path.c_str());
}

State snapshotState() {
    Guard lock(stateMutex);
    return state;
}
void setError(const char *message) {
    Guard lock(stateMutex);
    strlcpy(state.error, message, sizeof(state.error));
}
void setJob(const char *value) {
    Guard lock(stateMutex);
    strlcpy(state.job, value, sizeof(state.job));
}
bool recordingBusy(const State &value) {
    return strcmp(value.phase, "starting") == 0 || strcmp(value.phase, "recording") == 0 || strcmp(value.phase, "stopping") == 0;
}
String randomHex(size_t bytes) {
    String result;
    result.reserve(bytes * 2);
    const char *digits = "0123456789abcdef";
    for (size_t index = 0; index < bytes; ++index) {
        uint8_t value = esp_random() & 255;
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}
bool secureEqual(const String &left, const String &right) {
    if (left.length() != right.length()) return false;
    uint8_t difference = 0;
    for (size_t index = 0; index < left.length(); ++index) difference |= left[index] ^ right[index];
    return difference == 0;
}

struct Frame {
    uint8_t *data = nullptr;
    size_t length = 0;
    int references = 0;
    uint32_t sequence = 0;
    int64_t timestamp = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};
Frame frames[SLOT_COUNT];
portMUX_TYPE framesMux = portMUX_INITIALIZER_UNLOCKED;
int latestFrame = -1;
uint32_t frameSequence = 0;
int reserveFrame() {
    int result = -1;
    portENTER_CRITICAL(&framesMux);
    for (int index = 0; index < SLOT_COUNT; ++index) {
        if (index != latestFrame && frames[index].references == 0) {
            frames[index].references = -1;
            result = index;
            break;
        }
    }
    portEXIT_CRITICAL(&framesMux);
    return result;
}
int acquireLatest(uint32_t after = 0, int64_t notBefore = 0) {
    int result = -1;
    portENTER_CRITICAL(&framesMux);
    if (latestFrame >= 0) {
        Frame &frame = frames[latestFrame];
        if (frame.references >= 0 && frame.sequence != after && frame.timestamp >= notBefore) {
            ++frame.references;
            result = latestFrame;
        }
    }
    portEXIT_CRITICAL(&framesMux);
    return result;
}
void releaseFrame(int index) {
    portENTER_CRITICAL(&framesMux);
    if (frames[index].references > 0) --frames[index].references;
    portEXIT_CRITICAL(&framesMux);
}
struct SaveItem { int frame; };
QueueHandle_t saveQueue;
enum CommandType { SET_PROFILE, START_TIMELAPSE };
struct CameraCommand {
    CommandType type;
    Profile profile;
    int intervalSeconds;
    bool photoPriority;
    bool flash;
};
QueueHandle_t cameraQueue;

bool validResolution(int value) {
    return value == FRAMESIZE_QQVGA || value == FRAMESIZE_QVGA || value == FRAMESIZE_VGA || value == FRAMESIZE_SVGA || value == FRAMESIZE_XGA || value == FRAMESIZE_SXGA || value == FRAMESIZE_UXGA || value == FRAMESIZE_CIF || value == FRAMESIZE_HVGA || value == FRAMESIZE_QCIF || value == FRAMESIZE_HQVGA || value == FRAMESIZE_240X240 || value == FRAMESIZE_HD || value == FRAMESIZE_96X96;
}
bool applyProfile(const Profile &profile) {
    sensor_t *sensor = esp_camera_sensor_get();
    if (!sensor) return false;
    return sensor->set_framesize(sensor, static_cast<framesize_t>(profile.resolution)) == 0 &&
        sensor->set_quality(sensor, profile.quality) == 0 &&
        sensor->set_brightness(sensor, profile.brightness) == 0 &&
        sensor->set_contrast(sensor, profile.contrast) == 0 &&
        sensor->set_saturation(sensor, profile.saturation) == 0 &&
        sensor->set_vflip(sensor, profile.vflip) == 0 &&
        sensor->set_hmirror(sensor, profile.mirror) == 0;
}
void warmup(int count) {
    for (int index = 0; index < count; ++index) {
        camera_fb_t *frame = esp_camera_fb_get();
        if (frame) esp_camera_fb_return(frame);
        vTaskDelay(1);
    }
}
void applyLed() {
    Guard lock(ledMutex);
    State value = snapshotState();
    int duty = (value.ledEnabled || value.ledOverride) ? (value.ledPct * 1023 + 50) / 100 : 0;
    ledcWrite(LED_CHANNEL, duty);
}
void overrideLed(bool enabled) {
    {
        Guard lock(stateMutex);
        state.ledOverride = enabled;
    }
    applyLed();
}
void persistProfile(const Profile &profile) {
    Guard lock(preferencesMutex);
    preferences.putInt("schema", 2);
    preferences.putInt("cam_res", profile.resolution);
    preferences.putInt("cam_qual", profile.quality);
    preferences.putInt("cam_brt", profile.brightness);
    preferences.putInt("cam_con", profile.contrast);
    preferences.putInt("cam_sat", profile.saturation);
    preferences.putBool("cam_vf", profile.vflip);
    preferences.putBool("cam_hm", profile.mirror);
}

bool validPath(const String &path) {
    if (path.length() == 0 || path.length() > 180 || path[0] != '/' || path.indexOf('\\') >= 0) return false;
    int start = 1;
    for (size_t index = 0; index < path.length(); ++index) {
        unsigned char value = path[index];
        if (value < 32 || value == 127 || value == ':' || value == '*' || value == '?' || value == '"' || value == '<' || value == '>' || value == '|') return false;
    }
    for (size_t index = 1; index <= path.length(); ++index) {
        if (index == path.length() || path[index] == '/') {
            String part = path.substring(start, index);
            if (part == "." || part == ".." || part.endsWith(".") || part.endsWith(" ")) return false;
            if (part.isEmpty() && index != path.length()) return false;
            start = index + 1;
        }
    }
    return true;
}
String canonicalPath(String path) {
    while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    return path;
}
String absolutePath(const String &path) { return String(SD_BASE) + path; }
bool pathContains(String parent, String child) {
    parent = canonicalPath(parent);
    child = canonicalPath(child);
    parent.toLowerCase();
    child.toLowerCase();
    return parent == "/" || parent == child || child.startsWith(parent + "/");
}
bool protectedPath(const String &rawPath, const State &value) {
    String path = canonicalPath(rawPath);
    if (path == "/" || path.equalsIgnoreCase("/timelapse")) return true;
    return recordingBusy(value) && value.session[0] &&
        (pathContains(path, value.session) || pathContains(value.session, path));
}
StorageJobStatus snapshotStorageJob() {
    Guard lock(stateMutex);
    return storageJob;
}
void storageProgress(const char *phase, uint32_t processed, uint32_t total) {
    Guard lock(stateMutex);
    strlcpy(storageJob.phase, phase, sizeof(storageJob.phase));
    storageJob.processed = processed;
    storageJob.total = total;
}

#include "ftp_service.h"

bool updateSpace() {
    DWORD freeClusters;
    FATFS *filesystem = nullptr;
    FRESULT result = f_getfree(driveName, &freeClusters, &filesystem);
    if (result != FR_OK || !filesystem) return false;
    Guard lock(stateMutex);
    state.totalBytes = uint64_t(filesystem->n_fatent - 2) * filesystem->csize * 512;
    state.freeBytes = uint64_t(freeClusters) * filesystem->csize * 512;
    return true;
}
void releaseStorage() {
    if (sdFilesystem) {
        f_mount(nullptr, driveName, 0);
        esp_vfs_fat_unregister_path(SD_BASE);
        sdFilesystem = nullptr;
    }
    if (sdDrive != 0xff) {
        ff_diskio_unregister(sdDrive);
        sdDrive = 0xff;
    }
    if (sdCard) {
        free(sdCard);
        sdCard = nullptr;
    }
    if (hostInitialized) {
        sdmmc_host_deinit();
        hostInitialized = false;
    }
}
bool mountStorage() {
    Guard disk(sdMutex);
    releaseStorage();
    {
        Guard lock(stateMutex);
        state.sdPresent = false;
        state.sdMounted = false;
        state.totalBytes = 0;
        state.freeBytes = 0;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t result = sdmmc_host_init();
    if (result != ESP_OK) { setError("Falha ao inicializar SDMMC."); return false; }
    hostInitialized = true;
    result = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    sdCard = static_cast<sdmmc_card_t *>(calloc(1, sizeof(sdmmc_card_t)));
    if (result != ESP_OK || !sdCard) { releaseStorage(); setError("Sem recursos para o SD."); return false; }
    result = sdmmc_card_init(&host, sdCard);
    if (result != ESP_OK) { releaseStorage(); setError("Cartão SD ausente ou sem comunicação."); return false; }
    {
        Guard lock(stateMutex);
        state.sdPresent = true;
        state.cardSerial = sdCard->cid.serial;
    }
    if (ff_diskio_get_drive(&sdDrive) != ESP_OK) { releaseStorage(); setError("Sem unidade FAT disponível."); return false; }
    snprintf(driveName, sizeof(driveName), "%u:", sdDrive);
    ff_diskio_register_sdmmc(sdDrive, sdCard);
    result = esp_vfs_fat_register(SD_BASE, driveName, 6, &sdFilesystem);
    if (result != ESP_OK) { releaseStorage(); setError("Falha ao registrar volume FAT."); return false; }
    FRESULT mounted = f_mount(sdFilesystem, driveName, 1);
    if (mounted != FR_OK || !updateSpace()) {
        setError("SD detectado, mas FAT não montado. Não houve formatação automática.");
        return false;
    }
    {
        Guard lock(stateMutex);
        state.sdMounted = true;
        state.error[0] = 0;
    }
    return true;
}
bool writeAll(const String &path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path.c_str(), "wb");
    if (!file) return false;
    bool success = true;
    size_t offset = 0;
    while (offset < size) {
        size_t amount = std::min(size - offset, size_t(8192));
        if (fwrite(data + offset, 1, amount, file) != amount) { success = false; break; }
        offset += amount;
        vTaskDelay(1);
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) success = false;
    if (fclose(file) != 0) success = false;
    return success;
}
bool saveManifest(const State &value, const char *phase) {
    StaticJsonDocument<1536> document;
    document["firmware"] = VERSION;
    document["session"] = value.session;
    document["state"] = phase;
    document["interval_seconds"] = value.intervalSeconds;
    document["resolution"] = value.recordingProfile.resolution;
    document["jpeg_quality"] = value.recordingProfile.quality;
    document["photo_priority"] = value.photoPriority;
    document["saved"] = value.saved;
    document["missed"] = value.missed;
    document["bytes"] = value.sessionBytes;
    document["elapsed_seconds"] = (esp_timer_get_time() - value.startedUs) / 1000000;
    document["last_error"] = value.error;
    time_t wall = time(nullptr);
    if (wall > 1700000000) document["updated_utc"] = static_cast<int64_t>(wall);
    document["recovery_note"] = "Se o dispositivo reiniciou com state=recording, esta sessão foi interrompida; imagens completas permanecem válidas.";
    String json;
    serializeJson(document, json);
    String temporary = absolutePath(String(value.session) + "/manifest.tmp");
    String finalName = absolutePath(String(value.session) + "/manifest.json");
    if (!writeAll(temporary, reinterpret_cast<const uint8_t *>(json.c_str()), json.length())) return false;
    // FATFS não substitui destino existente em rename; mantém a versão anterior.
    String previousName = absolutePath(String(value.session) + "/manifest.previous.json");
    struct stat info;
    bool hadPrevious = stat(finalName.c_str(), &info) == 0;
    if (hadPrevious) {
        if (unlink(previousName.c_str()) != 0 && errno != ENOENT) return false;
        if (rename(finalName.c_str(), previousName.c_str()) != 0) return false;
    }
    if (rename(temporary.c_str(), finalName.c_str()) == 0) return true;
    if (hadPrevious) rename(previousName.c_str(), finalName.c_str());
    return false;
}

// Preflight completo antes de excluir; memória limitada pela profundidade.
bool inspectTree(const String &path, unsigned depth, uint32_t &count, String &error, bool report = false, WebRequest *request = nullptr) {
    if (depth > 16 || !validPath(path)) { error = "Estrutura profunda demais ou caminho não suportado."; return false; }
    if (request && !request->client.connected()) { error = "Cliente desconectado."; return false; }
    struct stat info;
    if (stat(absolutePath(path).c_str(), &info)) { error = "Não foi possível ler um item da pasta."; return false; }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) { error = "Tipo de arquivo não suportado."; return false; }
    if (info.st_size < 0) { error = "Arquivo maior que o suportado pelo driver do cartão."; return false; }
    ++count;
    if (report && count % 32 == 0) storageProgress("scanning", 0, count);
    if (!S_ISDIR(info.st_mode)) return true;
    DIR *directory = opendir(absolutePath(path).c_str());
    if (!directory) { error = "Não foi possível abrir uma subpasta."; return false; }
    bool success = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) {
            if (errno) { error = "Falha ao percorrer a pasta."; success = false; }
            break;
        }
        String name = entry->d_name;
        if (name == "." || name == "..") continue;
        String child = path + (path == "/" ? "" : "/") + name;
        if (!inspectTree(child, depth + 1, count, error, report, request)) { success = false; break; }
        vTaskDelay(1);
    }
    closedir(directory);
    return success;
}

bool deleteTree(const String &path, unsigned depth, uint32_t &processed, uint32_t total, String &error) {
    if (depth > 16 || !validPath(path)) { error = "Estrutura não suportada."; return false; }
    struct stat info;
    if (stat(absolutePath(path).c_str(), &info)) { error = "Item desapareceu durante a exclusão."; return false; }
    if (S_ISDIR(info.st_mode)) {
        DIR *directory = opendir(absolutePath(path).c_str());
        if (!directory) { error = "Falha ao abrir pasta para exclusão."; return false; }
        bool success = true;
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(directory);
            if (!entry) {
                if (errno) { error = "Falha ao percorrer a pasta."; success = false; }
                break;
            }
            String name = entry->d_name;
            if (name == "." || name == "..") continue;
            String child = path + "/" + name;
            if (!deleteTree(child, depth + 1, processed, total, error)) { success = false; break; }
            vTaskDelay(1);
        }
        closedir(directory);
        if (!success) return false;
        if (rmdir(absolutePath(path).c_str())) { error = "Não foi possível remover uma pasta."; return false; }
    } else if (S_ISREG(info.st_mode)) {
        if (unlink(absolutePath(path).c_str())) { error = "Não foi possível remover um arquivo."; return false; }
    } else {
        error = "Tipo de item não suportado.";
        return false;
    }
    ++processed;
    if (processed % 8 == 0 || processed == total) storageProgress("deleting", processed, total);
    return true;
}

void runStorageCommand(const StorageCommand &command) {
    String error;
    uint32_t total = 0;
    uint32_t processed = 0;
    bool success = false;
    {
        Guard disk(sdMutex);
        State before = snapshotState();
        if (!before.sdMounted || protectedPath(command.path, before)) {
            error = "Cartão indisponível ou caminho protegido.";
        } else if (command.operation == StorageOperation::DeleteTree) {
            storageProgress("scanning", 0, 0);
            if (inspectTree(command.path, 0, total, error, true)) {
                storageProgress("deleting", 0, total);
                success = deleteTree(command.path, 0, processed, total, error);
            }
        } else {
            storageProgress("renaming", 0, 1);
            total = 1;
            struct stat fromInfo;
            struct stat toInfo;
            if (stat(absolutePath(command.path).c_str(), &fromInfo) || !S_ISDIR(fromInfo.st_mode)) {
                error = "Pasta de origem não encontrada.";
            } else if (!stat(absolutePath(command.destination).c_str(), &toInfo)) {
                error = "Já existe um item com o novo nome. Nada foi sobrescrito.";
            } else if (errno != ENOENT) {
                error = "Não foi possível verificar o nome de destino.";
            } else if (rename(absolutePath(command.path).c_str(), absolutePath(command.destination).c_str())) {
                error = "Não foi possível renomear a pasta.";
            } else {
                success = true;
                processed = 1;
            }
        }
        if (success || processed) updateSpace();
    }
    if (success) {
        Guard prefsLock(preferencesMutex);
        String active = preferences.isKey("active_session") ? preferences.getString("active_session", "") : "";
        if (!active.isEmpty() && preferences.getUInt("active_sd", 0) == snapshotState().cardSerial && pathContains(command.path, active)) {
            if (command.operation == StorageOperation::DeleteTree) {
                preferences.remove("active_session");
                preferences.remove("active_sd");
            } else {
                String replacement = String(command.destination) + active.substring(strlen(command.path));
                preferences.putString("active_session", replacement);
            }
        }
    }
    {
        Guard lock(stateMutex);
        if (success && state.session[0] && pathContains(command.path, state.session)) {
            if (command.operation == StorageOperation::DeleteTree) state.session[0] = 0;
            else {
                String replacement = String(command.destination) + String(state.session).substring(strlen(command.path));
                strlcpy(state.session, replacement.c_str(), sizeof(state.session));
            }
        }
        storageJob.processed = processed;
        storageJob.total = total;
        strlcpy(storageJob.phase, success ? "completed" : "failed", sizeof(storageJob.phase));
        strlcpy(storageJob.error, error.c_str(), sizeof(storageJob.error));
        strlcpy(state.job, success ? "storage_completed" : "storage_failed", sizeof(state.job));
        state.maintenance = false;
    }
}

void formatStorage() {
    Guard disk(sdMutex);
    setJob("formatting");
    bool success = false;
    if (sdCard && sdFilesystem && sdDrive != 0xff) {
        void *work = malloc(4096);
        if (work) {
            f_mount(nullptr, driveName, 0);
            DWORD partitions[4] = {100, 0, 0, 0};
            FRESULT result = f_fdisk(sdDrive, partitions, work);
            if (result == FR_OK) result = f_mkfs(driveName, FM_FAT32, 0, work, 4096);
            free(work);
            setJob("remounting");
            if (result == FR_OK) result = f_mount(sdFilesystem, driveName, 1);
            if (result == FR_OK) {
                const char check[] = "RemoteCam SD test";
                const String testPath = String(SD_BASE) + "/remotecam-check.tmp";
                success = writeAll(testPath, reinterpret_cast<const uint8_t *>(check), sizeof(check));
                FILE *file = success ? fopen(testPath.c_str(), "rb") : nullptr;
                char actual[sizeof(check)] = {};
                if (!file || fread(actual, 1, sizeof(actual), file) != sizeof(actual) || memcmp(actual, check, sizeof(check))) success = false;
                if (file) fclose(file);
                unlink(testPath.c_str());
                if (success) success = updateSpace();
            }
        }
    }
    Guard lock(stateMutex);
    state.maintenance = false;
    state.sdMounted = success;
    strlcpy(state.job, success ? "format_completed" : "format_failed", sizeof(state.job));
    strlcpy(state.error, success ? "" : "Falha na formatação/verificação. Nenhuma repetição automática.", sizeof(state.error));
}

void recoverInterruptedSession() {
    String session;
    uint32_t cardSerial;
    {
        Guard lock(preferencesMutex);
        session = preferences.isKey("active_session") ? preferences.getString("active_session", "") : "";
        cardSerial = preferences.isKey("active_sd") ? preferences.getUInt("active_sd", 0) : 0;
    }
    State value = snapshotState();
    if (session.isEmpty() || !validPath(session) || !session.startsWith("/timelapse/") || cardSerial != value.cardSerial) return;
    bool restored = false;
    {
        Guard disk(sdMutex);
        String finalName = absolutePath(session + "/manifest.json");
        String backupName = absolutePath(session + "/manifest.previous.json");
        FILE *file = fopen(finalName.c_str(), "rb");
        if (!file) file = fopen(backupName.c_str(), "rb");
        if (file) {
            char buffer[2048];
            size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
            fclose(file);
            buffer[count] = 0;
            StaticJsonDocument<2048> document;
            if (!deserializeJson(document, buffer, count)) {
                String previousState = document["state"] | "";
                if (previousState == "recording" || previousState == "starting") {
                    document["state"] = "interrupted";
                    document["end_reason"] = "Desligamento ou reinício; não há retomada automática.";
                    String contents;
                    serializeJson(document, contents);
                    String temporary = absolutePath(session + "/manifest.recovery.tmp");
                    if (writeAll(temporary, reinterpret_cast<const uint8_t *>(contents.c_str()), contents.length())) {
                        String recoveryBackup = absolutePath(session + "/manifest.before-recovery.json");
                        unlink(recoveryBackup.c_str());
                        struct stat info;
                        bool hadFinal = stat(finalName.c_str(), &info) == 0;
                        if (!hadFinal || rename(finalName.c_str(), recoveryBackup.c_str()) == 0) {
                            restored = rename(temporary.c_str(), finalName.c_str()) == 0;
                            if (!restored && hadFinal) rename(recoveryBackup.c_str(), finalName.c_str());
                        }
                    }
                } else restored = true;
            }
        }
    }
    if (restored) {
        Guard lock(preferencesMutex);
        preferences.remove("active_session");
        preferences.remove("active_sd");
    }
}

void storageTask(void *) {
    if (mountStorage()) recoverInterruptedSession();
    {
        Guard lock(stateMutex);
        state.maintenance = false;
    }
    for (;;) {
        StorageCommand storageCommand;
        if (xQueueReceive(storageCommandQueue, &storageCommand, 0) == pdTRUE) {
            runStorageCommand(storageCommand);
            webSdJobs.fetch_sub(1);
        }
        if (formatRequested.exchange(false)) {
            formatStorage();
            webSdJobs.fetch_sub(1);
        }
        if (remountRequested.exchange(false)) {
            bool result = mountStorage();
            if (result) recoverInterruptedSession();
            Guard lock(stateMutex);
            state.maintenance = false;
            strlcpy(state.job, result ? "mount_completed" : "mount_failed", sizeof(state.job));
            webSdJobs.fetch_sub(1);
        }
        SaveItem item;
        if (xQueueReceive(saveQueue, &item, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        {
            Guard lock(stateMutex);
            state.writing = true;
        }
        State before = snapshotState();
        bool success = false;
        if (!before.error[0] && before.sdMounted) {
            Guard disk(sdMutex);
            if (updateSpace() && snapshotState().freeBytes > frames[item.frame].length + SPACE_RESERVE) {
                char filename[48];
                snprintf(filename, sizeof(filename), "/frame_%08lu", static_cast<unsigned long>(before.saved + 1));
                String prefix = absolutePath(String(before.session) + filename);
                Frame &frame = frames[item.frame];
                success = writeAll(prefix + ".part", frame.data, frame.length);
                if (success) success = rename((prefix + ".part").c_str(), (prefix + ".jpg").c_str()) == 0;
                if (success) {
                    FILE *index = fopen(absolutePath(String(before.session) + "/frames.csv").c_str(), "a");
                    if (index) {
                        int count = fprintf(index, "%lu,%lld,%lld,%u,%u,%u\n", static_cast<unsigned long>(before.saved + 1), frame.timestamp, static_cast<int64_t>(time(nullptr)), unsigned(frame.length), frame.width, frame.height);
                        if (count < 0 || fflush(index) || fsync(fileno(index))) success = false;
                        if (fclose(index)) success = false;
                    } else success = false;
                }
                if (success) {
                    {
                        Guard lock(stateMutex);
                        ++state.saved;
                        state.sessionBytes += frame.length;
                    }
                    success = saveManifest(snapshotState(), "recording");
                }
            }
        }
        releaseFrame(item.frame);
        {
            Guard lock(stateMutex);
            state.writing = false;
            if (!success) {
                strlcpy(state.error, "Erro de gravação, falta de espaço ou cartão removido. Sessão interrompida.", sizeof(state.error));
                stopRecording = true;
            }
        }
        --pendingSaves;
    }
}

int captureToSlot(bool publish, bool record) {
    int slot = reserveFrame();
    if (slot < 0) {
        Guard lock(stateMutex);
        ++state.dropped;
        return -1;
    }
    int64_t start = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb || fb->len > SLOT_BYTES || fb->len < 4 || fb->format != PIXFORMAT_JPEG || fb->buf[0] != 0xff || fb->buf[1] != 0xd8) {
        if (fb) esp_camera_fb_return(fb);
        portENTER_CRITICAL(&framesMux);
        frames[slot].references = 0;
        portEXIT_CRITICAL(&framesMux);
        Guard lock(stateMutex);
        ++state.captureErrors;
        return -1;
    }
    Frame &target = frames[slot];
    memcpy(target.data, fb->buf, fb->len);
    target.length = fb->len;
    target.width = fb->width;
    target.height = fb->height;
    target.timestamp = esp_timer_get_time();
    target.sequence = ++frameSequence;
    esp_camera_fb_return(fb);
    portENTER_CRITICAL(&framesMux);
    target.references = record ? 1 : 0;
    if (publish) latestFrame = slot;
    portEXIT_CRITICAL(&framesMux);
    {
        Guard lock(stateMutex);
        ++state.captured;
        state.frameBytes = target.length;
        state.actualWidth = target.width;
        state.actualHeight = target.height;
        state.captureMs = (esp_timer_get_time() - start) / 1000;
        state.maxCaptureMs = std::max(state.maxCaptureMs, state.captureMs);
    }
    if (record) {
        ++pendingSaves;
        SaveItem item{slot};
        if (xQueueSend(saveQueue, &item, 0) != pdTRUE) {
            releaseFrame(slot);
            --pendingSaves;
            Guard lock(stateMutex);
            ++state.missed;
        }
    }
    return slot;
}
void cameraTask(void *) {
    Profile original = snapshotState().profile;
    int64_t lastCapture = 0;
    for (;;) {
        if (otaActive) {
            cameraPaused = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        cameraPaused = false;
        CameraCommand command;
        if (xQueueReceive(cameraQueue, &command, 0) == pdTRUE) {
            if (command.type == SET_PROFILE) {
                bool success = applyProfile(command.profile);
                if (success) {
                    warmup(2);
                    original = command.profile;
                    persistProfile(original);
                    Guard lock(stateMutex);
                    state.profile = original;
                    state.error[0] = 0;
                } else {
                    applyProfile(original);
                    setError("Sensor recusou configuração; perfil anterior restaurado.");
                }
                setJob(success ? "settings_completed" : "settings_failed");
            } else {
                State current = snapshotState();
                bool success = false;
                {
                    Guard disk(sdMutex);
                    mkdir(ROOT_TL, 0775);
                    String folder = absolutePath(current.session);
                    success = mkdir(folder.c_str(), 0775) == 0;
                    if (success) success = saveManifest(current, "starting");
                }
                if (success) {
                    Guard lock(preferencesMutex);
                    success = preferences.putString("active_session", current.session) > 0;
                    preferences.putUInt("active_sd", current.cardSerial);
                }
                if (success && !command.photoPriority) success = applyProfile(command.profile);
                if (success) warmup(2);
                {
                    Guard lock(stateMutex);
                    strlcpy(state.phase, success ? "recording" : "error", sizeof(state.phase));
                    strlcpy(state.job, success ? "timelapse_started" : "timelapse_failed", sizeof(state.job));
                    if (!success) strlcpy(state.error, "Falha ao criar sessão/aplicar perfil.", sizeof(state.error));
                    state.nextUs = esp_timer_get_time();
                }
                if (!success) applyProfile(original);
            }
        }
        State value = snapshotState();
        if (stopRecording && recordingBusy(value)) {
            {
                Guard lock(stateMutex);
                strlcpy(state.phase, "stopping", sizeof(state.phase));
            }
            if (pendingSaves.load() == 0) {
                {
                    applyProfile(original);
                    warmup(2);
                    overrideLed(false);
                    value = snapshotState();
                    bool success;
                    {
                        Guard disk(sdMutex);
                        success = saveManifest(value, value.error[0] ? "error" : "completed");
                    }
                    if (success) {
                        Guard lock(preferencesMutex);
                        preferences.remove("active_session");
                        preferences.remove("active_sd");
                    }
                    {
                        Guard lock(stateMutex);
                        if (!success && !state.error[0]) strlcpy(state.error, "Não foi possível finalizar o manifesto.", sizeof(state.error));
                        strlcpy(state.phase, state.error[0] ? "error" : "completed", sizeof(state.phase));
                        strlcpy(state.job, "timelapse_stopped", sizeof(state.job));
                        state.nextUs = 0;
                        state.endedUs = esp_timer_get_time();
                    }
                    stopRecording = false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t now = esp_timer_get_time();
        bool due = strcmp(value.phase, "recording") == 0 && now >= value.nextUs;
        bool video = (streamClients.load() > 0) || snapshotsWaiting > 0;
        if (!video && !due && value.ledOverride) overrideLed(false);
        if (!value.ready || (!due && (!video || now - lastCapture < 33000))) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        bool changed = due && value.photoPriority;
        bool illuminate = due ? value.recordingFlash : value.autoFlash;
        if (illuminate) {
            overrideLed(true);
            if (!video || due) vTaskDelay(pdMS_TO_TICKS(120));
        }
        bool configured = !changed || applyProfile(value.recordingProfile);
        if (changed || (illuminate && due)) warmup(2);
        int captured = configured ? captureToSlot(!changed, due) : -1;
        if (changed) {
            if (!applyProfile(original)) setError("Falha ao restaurar perfil do vídeo.");
            warmup(2);
        }
        if (illuminate && (due || (streamClients.load() == 0))) overrideLed(false);
        if (due) {
            Guard lock(stateMutex);
            if (captured < 0) ++state.missed;
            int64_t interval = int64_t(state.intervalSeconds) * 1000000;
            state.nextUs += interval;
            int64_t finished = esp_timer_get_time();
            if (state.nextUs <= finished) {
                uint32_t skipped = (finished - state.nextUs) / interval + 1;
                state.missed += skipped;
                state.nextUs += int64_t(skipped) * interval;
            }
        }
        if ((streamClients.load() == 0) && snapshotsWaiting == 0 && !due && value.ledOverride) overrideLed(false);
        lastCapture = esp_timer_get_time();
        vTaskDelay(1);
    }
}

String header(WebRequest *request, const char *name) {
    return request->header(name);
}
esp_err_t jsonReply(WebRequest *request, JsonDocument &document, const char *status = "200 OK") {
    String body;
    serializeJson(document, body);
    setResponseStatus(request, status);
    setResponseType(request, "application/json; charset=utf-8");
    setResponseHeader(request, "Cache-Control", "no-store");
    setResponseHeader(request, "X-Content-Type-Options", "nosniff");
    return sendResponse(request, body.c_str(), body.length());
}
esp_err_t message(WebRequest *request, const char *value, const char *status = "200 OK") {
    StaticJsonDocument<512> document;
    document["message"] = value;
    return jsonReply(request, document, status);
}
bool localMutation(WebRequest *request) {
    // Sem login. Rejeita comandos enviados por páginas de outra origem.
    String host = header(request, "Host");
    String origin = header(request, "Origin");
    String plainHost = host;
    if (plainHost.endsWith(":80")) plainHost.remove(plainHost.length() - 3);
    plainHost.toLowerCase();
    String configuredHost = hostname;
    configuredHost.toLowerCase();
    bool localHost = plainHost == WiFi.localIP().toString() ||
        plainHost == configuredHost + ".local" || plainHost == configuredHost;
    return localHost && (origin.isEmpty() || origin == "http://" + host || origin == "https://" + host);
}
bool readJson(WebRequest *request, JsonDocument &document) {
    if (request->content_len <= 0 || request->content_len > 2048) return false;
    char body[2049];
    size_t received = 0;
    int failures = 0;
    uint32_t bodyStarted = millis();
    while (received < request->content_len) {
        if (millis() - bodyStarted > 5000) return false;
        int count = receiveBody(request, body + received, request->content_len - received);
        if (count == RECEIVE_TIMEOUT && ++failures <= 2) continue;
        if (count <= 0) return false;
        received += count;
    }
    body[received] = 0;
    return !deserializeJson(document, body, received) && document.is<JsonObject>();
}
String queryValue(WebRequest *request, const char *name) {
    int question = request->uri.indexOf('?');
    if (question < 0) return "";
    String query = request->uri.substring(question + 1);
    String encoded;
    int start = 0;
    while (start < int(query.length())) {
        int end = query.indexOf('&', start);
        if (end < 0) end = query.length();
        String pair = query.substring(start, end);
        int equals = pair.indexOf('=');
        if (equals >= 0 && pair.substring(0, equals) == name) {
            encoded = pair.substring(equals + 1);
            break;
        }
        start = end + 1;
    }
    if (encoded.length() > 511) return "";
    String result;
    for (size_t index = 0; index < encoded.length(); ++index) {
        char value = encoded[index];
        if (value == '%') {
            if (index + 2 >= encoded.length() || !isxdigit(encoded[index + 1]) || !isxdigit(encoded[index + 2])) return "";
            char digits[3] = {encoded[index + 1], encoded[index + 2], 0};
            value = strtol(digits, nullptr, 16);
            index += 2;
            if (!value) return "";
        } else if (value == '+') value = ' ';
        result += value;
    }
    return result;
}
bool parseProfile(JsonVariantConst input, Profile &profile) {
    if (!input.is<JsonObjectConst>()) return false;
    if (!input["resolution"].is<int>() || !input["quality"].is<int>()) return false;
    profile.resolution = input["resolution"].as<int>();
    profile.quality = input["quality"].as<int>();
    if (!validResolution(profile.resolution) || profile.quality < 10 || profile.quality > 63) return false;
    const char *names[] = {"brightness", "contrast", "saturation"};
    int *values[] = {&profile.brightness, &profile.contrast, &profile.saturation};
    for (int index = 0; index < 3; ++index) {
        if (!input[names[index]].isNull()) {
            if (!input[names[index]].is<int>()) return false;
            *values[index] = input[names[index]].as<int>();
        }
        if (*values[index] < -2 || *values[index] > 2) return false;
    }
    if (!input["vflip"].isNull()) profile.vflip = input["vflip"].as<bool>();
    if (!input["mirror"].isNull()) profile.mirror = input["mirror"].as<bool>();
    return true;
}
void profileJson(JsonObject target, const Profile &profile) {
    target["resolution"] = profile.resolution;
    target["quality"] = profile.quality;
    target["brightness"] = profile.brightness;
    target["contrast"] = profile.contrast;
    target["saturation"] = profile.saturation;
    target["vflip"] = profile.vflip;
    target["mirror"] = profile.mirror;
}

void appendServices(JsonObject target) {
    uint32_t configuration = serviceConfiguration.load();
    bool wantedFtp = rc_service_enabled(configuration, RC_SERVICE_FTP);
    bool wantedSd = rc_service_enabled(configuration, RC_SERVICE_WEB_SD);
    bool wantedOta = rc_service_enabled(configuration, RC_SERVICE_OTA);
    uint32_t mask = ftpSessionMask.load();
    uint32_t clients = unsigned(bool(mask & 1)) + unsigned(bool(mask & 2));
    uint32_t requests = webSdRequests.load();
    uint32_t jobs = webSdJobs.load();
    bool updating = otaActive.load();
    target["revision"] = serviceRevision.load();
    target["boot_id"] = serviceBootId;
    target["sample_ms"] = esp_timer_get_time() / 1000;
    JsonObject fixed = target.createNestedObject("always_on");
    fixed["wifi"] = true;
    fixed["http"] = true;
    fixed["stream"] = true;
    fixed["snapshot"] = true;
    JsonObject ftp = target.createNestedObject("ftp");
    ftp["enabled"] = wantedFtp;
    ftp["state"] = rc_service_state_name(rc_service_state(wantedFtp, ftpListening.load(), clients, updating, ftpStartFailed.load()));
    ftp["clients"] = clients;
    ftp["transferring"] = ftpTransferring.load();
    JsonObject web = target.createNestedObject("web_sd");
    web["enabled"] = wantedSd;
    web["state"] = rc_service_state_name(rc_service_state(wantedSd, wantedSd && !updating, requests + jobs, updating, false));
    web["requests"] = requests;
    web["jobs"] = jobs;
    JsonObject ota = target.createNestedObject("ota");
    ota["enabled"] = wantedOta;
    ota["state"] = rc_service_state_name(rc_service_state(wantedOta, wantedOta, otaRequests.load() + unsigned(updating), false, false));
    ota["updating"] = updating;
    JsonObject panel = target.createNestedObject("panel_refresh");
    panel["enabled"] = rc_service_enabled(configuration, RC_SERVICE_PANEL_REFRESH);
    panel["state"] = panel["enabled"].as<bool>() ? "enabled" : "disabled";
    panel["interval_seconds"] = rc_service_interval(configuration);
}

esp_err_t servicesStatusHandler(WebRequest *request, bool saved = false) {
    DynamicJsonDocument document(2304);
    appendServices(document.createNestedObject("services"));
    if (saved) {
        document["saved"] = true;
        document["message"] = "Preferências salvas. Operações já iniciadas podem terminar antes de desligar.";
    }
    return jsonReply(request, document);
}

esp_err_t serviceUnavailable(WebRequest *request, const char *service) {
    if (String(request->uri).startsWith("/gallery")) {
        static const char page[] = "<!doctype html><html lang='pt-BR'><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Serviço desativado</title><body style='font:16px system-ui;background:#0b1119;color:#edf4fb;padding:32px'><h1>Acesso web ao SD desativado</h1><p>O cartão e o timelapse continuam independentes desta chave.</p><a style='color:#52d5bb' href='/#services'>Voltar ao painel para ativar o serviço</a></body></html>";
        setResponseStatus(request, "503 Service Unavailable");
        setResponseType(request, "text/html; charset=utf-8");
        setResponseHeader(request, "Cache-Control", "no-store");
        return sendResponse(request, page, sizeof(page) - 1);
    }
    StaticJsonDocument<512> document;
    document["code"] = "service_disabled";
    document["service"] = service;
    document["message"] = !strcmp(service, "ota") ? "Atualização OTA desativada. Ative em Serviços antes de enviar firmware." : "Acesso web ao SD desativado ou pausado. Ative em Serviços; operações já iniciadas continuam até terminar.";
    return jsonReply(request, document, "503 Service Unavailable");
}

esp_err_t configureServices(WebRequest *request) {
    if (!localMutation(request)) return message(request, "Use o painel da própria câmera para alterar serviços.", "403 Forbidden");
    StaticJsonDocument<512> body;
    if (!readJson(request, body)) return message(request, "JSON de serviços inválido.", "400 Bad Request");
    JsonObjectConst values = body.as<JsonObjectConst>();
    if (!values.size()) return message(request, "Informe ao menos uma preferência.", "400 Bad Request");
    for (JsonPairConst item : values) {
        const char *name = item.key().c_str();
        if (strcmp(name, "ftp") && strcmp(name, "web_sd") && strcmp(name, "ota") && strcmp(name, "panel_refresh") && strcmp(name, "refresh_seconds")) {
            return message(request, "Preferência inválida. Wi-Fi, painel, stream e snapshot não podem ser desativados.", "400 Bad Request");
        }
    }
    struct Setting { const char *name; uint32_t flag; };
    static const Setting settings[] = {
        {"ftp", RC_SERVICE_FTP}, {"web_sd", RC_SERVICE_WEB_SD},
        {"ota", RC_SERVICE_OTA}, {"panel_refresh", RC_SERVICE_PANEL_REFRESH}
    };
    for (const auto &setting : settings) {
        if (values.containsKey(setting.name) && !values[setting.name].is<bool>()) return message(request, "As chaves devem ser booleanas.", "400 Bad Request");
    }
    if (values.containsKey("refresh_seconds") && (!values["refresh_seconds"].is<int>() ||
        (values["refresh_seconds"].as<int>() != 2 && values["refresh_seconds"].as<int>() != 5 && values["refresh_seconds"].as<int>() != 10))) {
        return message(request, "Escolha atualização a cada 2, 5 ou 10 segundos.", "400 Bad Request");
    }
    bool stored = true;
    {
        Guard change(servicesMutex);
        uint32_t original = serviceConfiguration.load();
        uint32_t requested = original;
        for (const auto &setting : settings) {
            if (!values.containsKey(setting.name)) continue;
            if (values[setting.name].as<bool>()) requested |= setting.flag;
            else requested &= ~setting.flag;
        }
        if (values.containsKey("refresh_seconds")) rc_service_set_interval(requested, values["refresh_seconds"].as<unsigned>(), &requested);
        {
            Guard storage(preferencesMutex);
            if (requested != original || !servicePreferencesValid.load()) stored = preferences.putUInt("svc_cfg", requested) == sizeof(uint32_t);
        }
        if (stored) {
            servicePreferencesValid = true;
            serviceConfiguration.store(requested);
            if (requested != original) serviceRevision.fetch_add(1);
        }
    }
    if (!stored) return message(request, "Não foi possível salvar; as preferências anteriores foram mantidas.", "500 Internal Server Error");
    return servicesStatusHandler(request, true);
}

esp_err_t statusHandler(WebRequest *request) {
    State value = snapshotState();
    DynamicJsonDocument document(8192);
    document["version"] = VERSION;
    document["hostname"] = hostname;
    appendServices(document.createNestedObject("services"));
    document["uptime_seconds"] = esp_timer_get_time() / 1000000;
    document["reset_reason"] = int(esp_reset_reason());
    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapUsed = heapTotal > heapFree ? heapTotal - heapFree : 0;
    document["heap_free"] = heapFree;
    document["heap_total"] = heapTotal;
    document["heap_used"] = heapUsed;
    if (heapTotal) document["heap_used_pct"] = roundf(1000.0f * heapUsed / heapTotal) / 10.0f;
    else document["heap_used_pct"] = nullptr;
    document["heap_min"] = ESP.getMinFreeHeap();
    document["heap_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    document["psram_free"] = ESP.getFreePsram();
    document["psram_total"] = ESP.getPsramSize();
    float chipTemperature = temperatureRead();
    if (std::isfinite(chipTemperature) && chipTemperature >= -40 && chipTemperature <= 125) {
        document["chip_temperature_c"] = roundf(chipTemperature * 10) / 10;
    } else {
        document["chip_temperature_c"] = nullptr;
    }
    document["ota"] = otaActive.load();
    JsonObject ftp = document.createNestedObject("ftp");
    uint32_t ftpMask = ftpSessionMask.load();
    ftp["enabled"] = ftpListening.load();
    ftp["requested"] = serviceEnabled(RC_SERVICE_FTP);
    ftp["port"] = FTP_CONTROL_PORT;
    ftp["passive_first"] = FTP_PASSIVE_FIRST;
    ftp["passive_last"] = FTP_PASSIVE_FIRST + FTP_MAX_SESSIONS - 1;
    ftp["anonymous"] = true;
    ftp["read_only"] = true;
    ftp["sessions"] = int(bool(ftpMask & 1)) + int(bool(ftpMask & 2));
    ftp["transferring"] = ftpTransferring.load();
    document["error"] = value.error;
    document["job_id"] = value.jobId;
    document["job"] = value.job;
    bool connected = WiFi.status() == WL_CONNECTED;
    JsonObject wifi = document.createNestedObject("wifi");
    wifi["connected"] = connected;
    wifi["ip"] = WiFi.localIP().toString();
    wifi["channel"] = WiFi.channel();
    wifi_ps_type_t powerSave;
    if (esp_wifi_get_ps(&powerSave) == ESP_OK) wifi["power_save_mode"] = int(powerSave);
    if (connected) {
        int rssi = WiFi.RSSI();
        wifi["rssi_dbm"] = rssi;
        wifi["signal_pct"] = constrain((rssi + 90) * 2, 0, 100);
    } else {
        wifi["rssi_dbm"] = nullptr;
        wifi["signal_pct"] = nullptr;
    }
    JsonObject camera = document.createNestedObject("camera");
    camera["ready"] = value.ready;
    camera["streaming"] = (streamClients.load() > 0);
    camera["stream_clients"] = streamClients.load();
    camera["snapshot_clients"] = snapshotsWaiting.load();
    camera["width"] = value.actualWidth;
    camera["height"] = value.actualHeight;
    camera["captured"] = value.captured;
    camera["sent"] = value.sent;
    camera["errors"] = value.captureErrors;
    camera["dropped"] = value.dropped;
    camera["capture_ms"] = value.captureMs;
    camera["capture_max_ms"] = value.maxCaptureMs;
    camera["send_ms"] = value.sendMs;
    camera["jpeg_bytes"] = value.frameBytes;
    camera["xclk_mhz"] = 20;
    profileJson(camera.createNestedObject("profile"), value.profile);
    JsonObject led = document.createNestedObject("led");
    led["enabled"] = value.ledEnabled;
    led["intensity_pct"] = value.ledPct;
    led["effective_on"] = value.ledEnabled || value.ledOverride;
    led["auto_flash"] = value.autoFlash;
    JsonObject sd = document.createNestedObject("sd");
    sd["present"] = value.sdPresent;
    sd["mounted"] = value.sdMounted;
    sd["maintenance"] = value.maintenance;
    sd["total_bytes"] = value.totalBytes;
    sd["free_bytes"] = value.freeBytes;
    sd["serial"] = value.cardSerial;
    sd["download"] = value.download;
    sd["bus_bits"] = 1;
    StorageJobStatus job = snapshotStorageJob();
    JsonObject sdJob = sd.createNestedObject("job");
    sdJob["id"] = job.id;
    sdJob["operation"] = job.operation;
    sdJob["phase"] = job.phase;
    sdJob["processed"] = job.processed;
    sdJob["total"] = job.total;
    sdJob["path"] = job.path;
    sdJob["destination"] = job.destination;
    sdJob["error"] = job.error;
    JsonObject tl = document.createNestedObject("timelapse");
    tl["state"] = value.phase;
    tl["session"] = value.session;
    tl["saved"] = value.saved;
    tl["missed"] = value.missed;
    tl["bytes"] = value.sessionBytes;
    tl["interval_seconds"] = value.intervalSeconds;
    tl["elapsed_seconds"] = value.startedUs ? ((value.endedUs ? value.endedUs : esp_timer_get_time()) - value.startedUs) / 1000000 : 0;
    tl["next_seconds"] = value.nextUs ? std::max(int64_t(0), value.nextUs - esp_timer_get_time()) / 1000000 : 0;
    tl["photo_priority"] = value.photoPriority;
    tl["flash"] = value.recordingFlash;
    profileJson(tl.createNestedObject("profile"), value.recordingProfile);
    return jsonReply(request, document);
}


esp_err_t listFiles(WebRequest *request) {
    State value = snapshotState();
    if (!value.sdMounted || value.maintenance) return message(request, "SD indisponível.", "503 Service Unavailable");
    String path = queryValue(request, "path");
    if (path.isEmpty()) path = "/";
    if (!validPath(path)) return message(request, "Caminho inválido.", "400 Bad Request");
    String offsetValue = queryValue(request, "offset");
    int offset = offsetValue.isEmpty() ? 0 : offsetValue.toInt();
    if (offset < 0 || offset > 1000000) return message(request, "Página inválida.", "400 Bad Request");
    Guard disk(sdMutex);
    DIR *directory = opendir(absolutePath(path).c_str());
    if (!directory) return message(request, "Pasta não encontrada.", "404 Not Found");
    DynamicJsonDocument document(12288);
    document["path"] = path;
    JsonArray items = document.createNestedArray("items");
    struct dirent *entry;
    int skipped = 0;
    int count = 0;
    bool more = false;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (skipped++ < offset) continue;
        if (count == 24) { more = true; break; }
        String child = path + (path.endsWith("/") ? "" : "/") + entry->d_name;
        struct stat info;
        if (stat(absolutePath(child).c_str(), &info)) continue;
        JsonObject item = items.createNestedObject();
        item["name"] = entry->d_name;
        item["path"] = child;
        item["directory"] = S_ISDIR(info.st_mode);
        item["protected"] = protectedPath(child, value);
        item["bytes"] = uint64_t(info.st_size);
        item["modified"] = int64_t(info.st_mtime);
        item["incomplete"] = child.endsWith(".part") || child.endsWith(".tmp");
        ++count;
    }
    closedir(directory);
    disk.release(); // Não prende o cartão durante a transmissão da listagem.
    if (more) document["next_offset"] = offset + count;
    else document["next_offset"] = nullptr;
    return jsonReply(request, document);
}

esp_err_t streamHandler(WebRequest *request) {
    if (otaActive || !snapshotState().ready) return message(request, "Câmera indisponível.", "503 Service Unavailable");
    uint32_t count = streamClients.fetch_add(1);
    if (count >= MAX_STREAM_CLIENTS) {
        --streamClients;
        return message(request, "Limite de dois clientes de vídeo simultâneos.", "503 Service Unavailable");
    }
    uint32_t generation = streamGeneration.load();
    setResponseType(request, "multipart/x-mixed-replace;boundary=remotecamframe");
    setResponseHeader(request, "Cache-Control", "no-store");
    uint32_t sequence = 0;
    int64_t streamStarted = esp_timer_get_time();
    int64_t lastFrameAt = esp_timer_get_time();
    esp_err_t result = ESP_OK;
    while (request->client.connected() && generation == streamGeneration.load() && !otaActive) {
        int index = acquireLatest(sequence, streamStarted);
        if (index < 0) {
            if (esp_timer_get_time() - lastFrameAt > 15000000) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        Frame &frame = frames[index];
        sequence = frame.sequence;
        char partHeader[160];
        int length = snprintf(partHeader, sizeof(partHeader), "\r\n--remotecamframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Frame-Id: %lu\r\n\r\n", unsigned(frame.length), static_cast<unsigned long>(sequence));
        int64_t start = esp_timer_get_time();
        result = sendChunk(request, partHeader, length);
        if (result == ESP_OK) result = sendChunk(request, reinterpret_cast<const char *>(frame.data), frame.length);
        releaseFrame(index);
        if (result != ESP_OK) break;
        {
            Guard lock(stateMutex);
            ++state.sent;
            state.sendMs = (esp_timer_get_time() - start) / 1000;
        }
        lastFrameAt = esp_timer_get_time();
    }
    --streamClients;
    // A tarefa da câmera restaura a luz sem interromper uma foto em andamento.
    if (result == ESP_OK) sendChunk(request, nullptr, 0);
    return result;
}

esp_err_t snapshotHandler(WebRequest *request) {
    if (!snapshotState().ready || otaActive) return message(request, "Câmera indisponível.", "503 Service Unavailable");
    int64_t start = esp_timer_get_time();
    ++snapshotsWaiting;
    int index = -1;
    while (request->client.connected() && esp_timer_get_time() - start < 8000000 && !otaActive) {
        index = acquireLatest(0, start);
        if (index >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    --snapshotsWaiting;
    if (index < 0) return message(request, "Timeout de captura.", "503 Service Unavailable");
    setResponseType(request, "image/jpeg");
    setResponseHeader(request, "Cache-Control", "no-store");
    esp_err_t result = sendResponse(request, reinterpret_cast<const char *>(frames[index].data), frames[index].length);
    releaseFrame(index);
    return result;
}

esp_err_t sendFileChunks(WebRequest *request, const String &path, uint64_t size) {
    FILE *file;
    {
        Guard disk(sdMutex);
        file = fopen(path.c_str(), "rb");
    }
    if (!file) return ESP_FAIL;
    char buffer[4096];
    esp_err_t result = ESP_OK;
    uint64_t remaining = size;
    while (remaining && result == ESP_OK) {
        size_t count;
        {
            Guard disk(sdMutex);
            count = fread(buffer, 1, std::min(remaining, uint64_t(sizeof(buffer))), file);
        }
        if (!count) { result = ESP_FAIL; break; }
        result = sendChunk(request, buffer, count);
        remaining -= count;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    {
        Guard disk(sdMutex);
        fclose(file);
    }
    return result;
}
struct ZipPass {
    rc_zip_writer *writer;
    WebRequest *request;
    uint8_t *buffer;
    bool central;
    uint64_t localOffset = 0;
    uint64_t entries = 0;
    uint32_t fingerprint = 0xffffffffUL;
    ZipPass(rc_zip_writer *w, WebRequest *r, uint8_t *b, bool c) : writer(w), request(r), buffer(b), central(c) {}
};

bool zipFile(ZipPass &pass, const String &path, uint64_t size, uint32_t &crc) {
    FILE *file;
    {
        Guard disk(sdMutex);
        file = fopen(absolutePath(path).c_str(), "rb");
    }
    if (!file) return false;
    bool success = true;
    uint64_t remaining = size;
    uint32_t checksum = 0xffffffffUL;
    while (remaining) {
        if (!pass.request->client.connected()) { success = false; break; }
        size_t count;
        {
            Guard disk(sdMutex);
            count = fread(pass.buffer, 1, std::min(remaining, uint64_t(4096)), file);
        }
        if (!count) { success = false; break; }
        checksum = rc_zip_crc_update(checksum, pass.buffer, count);
        if (!pass.central && !rc_zip_write(pass.writer, pass.buffer, count)) { success = false; break; }
        remaining -= count;
        vTaskDelay(1);
    }
    {
        Guard disk(sdMutex);
        if (fclose(file)) success = false;
    }
    crc = checksum ^ 0xffffffffUL;
    return success;
}

bool zipTree(ZipPass &pass, const String &path, const String &relative, unsigned depth) {
    if (depth > 16 || !validPath(path) || !pass.request->client.connected()) return false;
    struct stat info;
    {
        Guard disk(sdMutex);
        if (stat(absolutePath(path).c_str(), &info) || info.st_size < 0) return false;
    }
    bool directory = S_ISDIR(info.st_mode);
    if (!directory && !S_ISREG(info.st_mode)) return false;
    String name = relative + (directory ? "/" : "");
    rc_zip_entry entry = {};
    entry.name = name.c_str();
    entry.size = directory ? 0 : uint64_t(info.st_size);
    entry.directory = directory;
    entry.local_offset = pass.central ? pass.localOffset : pass.writer->offset;
    struct tm modified = {};
    time_t timestamp = info.st_mtime;
    if (!gmtime_r(&timestamp, &modified) || modified.tm_year < 80 || modified.tm_year > 207) {
        modified = {};
        modified.tm_year = 80;
        modified.tm_mday = 1;
    }
    entry.dos_date = uint16_t(((modified.tm_year - 80) << 9) | ((modified.tm_mon + 1) << 5) | modified.tm_mday);
    entry.dos_time = uint16_t((modified.tm_hour << 11) | (modified.tm_min << 5) | (modified.tm_sec / 2));
    if (!pass.central && !rc_zip_local(pass.writer, &entry)) return false;
    if (!directory && !zipFile(pass, path, entry.size, entry.crc)) return false;
    if (pass.central) {
        if (!rc_zip_central(pass.writer, &entry)) return false;
    } else if (!rc_zip_descriptor(pass.writer, &entry)) return false;
    uint8_t fingerprint[24] = {};
    rc_zip_u32(fingerprint, name.length());
    rc_zip_u64(fingerprint + 4, entry.size);
    rc_zip_u32(fingerprint + 12, entry.crc);
    rc_zip_u16(fingerprint + 16, entry.dos_date);
    rc_zip_u16(fingerprint + 18, entry.dos_time);
    pass.fingerprint = rc_zip_crc_update(pass.fingerprint, fingerprint, sizeof(fingerprint));
    pass.fingerprint = rc_zip_crc_update(pass.fingerprint, reinterpret_cast<const uint8_t *>(name.c_str()), name.length());
    pass.localOffset += rc_zip_local_span(&entry);
    ++pass.entries;
    if (!directory) return true;
    DIR *handle;
    {
        Guard disk(sdMutex);
        handle = opendir(absolutePath(path).c_str());
    }
    if (!handle) return false;
    bool success = true;
    for (;;) {
        String childName;
        {
            Guard disk(sdMutex);
            errno = 0;
            struct dirent *child = readdir(handle);
            if (!child) {
                if (errno) success = false;
                break;
            }
            childName = child->d_name;
        }
        if (childName == "." || childName == "..") continue;
        String childPath = path + (path == "/" ? "" : "/") + childName;
        if (!zipTree(pass, childPath, relative + "/" + childName, depth + 1)) { success = false; break; }
        vTaskDelay(1);
    }
    {
        Guard disk(sdMutex);
        closedir(handle);
    }
    return success;
}

esp_err_t zipDirectory(WebRequest *request, const String &path, const String &archiveRoot) {
    uint32_t count = 0;
    String error;
    bool valid;
    {
        Guard disk(sdMutex);
        valid = inspectTree(path, 0, count, error, false, request);
    }
    if (!valid) { request->responseHeaders.clear(); return message(request, error.c_str(), "409 Conflict"); }
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[4096]);
    if (!buffer) { request->responseHeaders.clear(); return message(request, "Sem memória para iniciar o ZIP.", "503 Service Unavailable"); }
    rc_zip_writer writer = {};
    writer.context = request;
    writer.sink = [](void *context, const uint8_t *data, size_t size) -> int {
        return sendChunk(static_cast<WebRequest *>(context), reinterpret_cast<const char *>(data), size) == ESP_OK;
    };
    // Duas passagens: não armazena o ZIP nem o índice inteiro na RAM/SD.
    ZipPass files{&writer, request, buffer.get(), false};
    if (!zipTree(files, path, archiveRoot, 0)) return ESP_FAIL;
    uint64_t centralStart = writer.offset;
    ZipPass central{&writer, request, buffer.get(), true};
    if (!zipTree(central, path, archiveRoot, 0)) return ESP_FAIL;
    if (files.entries != central.entries || files.entries != count ||
        files.localOffset != central.localOffset || files.fingerprint != central.fingerprint) return ESP_FAIL;
    if (!rc_zip_finish(&writer, centralStart, central.entries)) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t mediaHandler(WebRequest *request) {
    String uri = request->uri;
    if (uri.startsWith("/snapshot") || uri.startsWith("/raw")) return snapshotHandler(request);
    if (!uri.startsWith("/api/sd/download?") && !uri.startsWith("/api/sd/archive?") && !uri.startsWith("/api/sd/image?")) return message(request, "Rota não encontrada.", "404 Not Found");
    String path = canonicalPath(queryValue(request, "path"));
    if (!validPath(path) || path.endsWith(".part") || path.endsWith(".tmp")) return message(request, "Arquivo inválido ou incompleto.", "400 Bad Request");
    State value = snapshotState();
    bool archive = uri.startsWith("/api/sd/archive?");
    bool inlineImage = uri.startsWith("/api/sd/image?");
    String lowerPath = path;
    lowerPath.toLowerCase();
    if (inlineImage && !lowerPath.endsWith(".jpg") && !lowerPath.endsWith(".jpeg")) {
        return message(request, "A visualização aceita somente JPEG.", "400 Bad Request");
    }
    {
        Guard lock(stateMutex);
        if (!state.sdMounted || state.maintenance || state.download || otaActive) return message(request, "SD ocupado ou indisponível.", "409 Conflict");
        // Manifesto/índice ativos não são snapshots consistentes; JPEGs completos são imutáveis.
        if (recordingBusy(state) && (archive || !path.endsWith(".jpg"))) return message(request, "Finalize a sessão para baixar o conjunto/metadados.", "409 Conflict");
        state.download = true;
    }
    struct stat info;
    bool exists;
    {
        Guard disk(sdMutex);
        exists = stat(absolutePath(path).c_str(), &info) == 0;
    }
    esp_err_t result;
    if (!exists || (archive ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode))) {
        result = message(request, "Item não encontrado.", "404 Not Found");
    } else {
        String basename = path.substring(path.lastIndexOf('/') + 1);
        if (basename.isEmpty()) basename = "cartao";
        String safeName;
        for (size_t index = 0; index < basename.length(); ++index) {
            char c = basename[index];
            safeName += isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' ? c : '_';
        }
        String disposition = String(inlineImage ? "inline; filename=\"" : "attachment; filename=\"") + safeName + (archive ? ".zip" : "") + "\"";
        setResponseHeader(request, "Content-Disposition", disposition.c_str());
        setResponseHeader(request, "Cache-Control", "no-store");
        setResponseType(request, archive ? "application/zip" : (inlineImage ? "image/jpeg" : "application/octet-stream"));
        if (archive) {
            result = zipDirectory(request, path, basename);
        } else result = sendFileChunks(request, absolutePath(path), info.st_size);
        if (result == ESP_OK) result = sendChunk(request, nullptr, 0);
    }
    {
        Guard lock(stateMutex);
        state.download = false;
    }
    return result;
}

esp_err_t otaHandler(WebRequest *request) {
    ServiceLease permission(RC_SERVICE_OTA, otaRequests);
    if (!permission.allowed()) return serviceUnavailable(request, "ota");
    State value = snapshotState();
    if (otaActive || webSdRequests.load() || webSdJobs.load() || recordingBusy(value) || value.maintenance || value.download || strcmp(value.job, "settings_applying") == 0) return message(request, "Finalize gravações e downloads antes de atualizar.", "409 Conflict");
    if (request->content_len < 1024 || request->content_len > ESP.getFreeSketchSpace()) return message(request, "Tamanho de firmware inválido.", "400 Bad Request");
    otaActive = true;
    ++streamGeneration;
    int attempts = 0;
    while (!cameraPaused && ++attempts < 100) vTaskDelay(pdMS_TO_TICKS(50));
    if (!cameraPaused) {
        otaActive = false;
        return message(request, "A câmera não parou a tempo.", "503 Service Unavailable");
    }
    overrideLed(false);
    bool success = Update.begin(request->content_len, U_FLASH);
    char buffer[4096];
    size_t remaining = request->content_len;
    int timeouts = 0;
    while (success && remaining) {
        int count = receiveBody(request, buffer, std::min(remaining, sizeof(buffer)));
        if (count == RECEIVE_TIMEOUT && ++timeouts < 4) continue;
        if (count <= 0) { success = false; break; }
        timeouts = 0;
        success = Update.write(reinterpret_cast<uint8_t *>(buffer), count) == size_t(count);
        remaining -= count;
    }
    if (success) success = Update.end(false);
    if (!success) Update.abort();
    if (success) rebootAt = millis() + 1800;
    else otaActive = false;
    return message(request, success ? "Firmware recebido e validado. Reiniciando." : "Atualização falhou; firmware anterior preservado.", success ? "200 OK" : "500 Internal Server Error");
}

esp_err_t queueStorageOperation(WebRequest *request, StorageOperation operation, const String &path, const String &destination = "") {
    StorageCommand command = {};
    command.operation = operation;
    strlcpy(command.path, path.c_str(), sizeof(command.path));
    strlcpy(command.destination, destination.c_str(), sizeof(command.destination));
    {
        Guard lock(stateMutex);
        if (!state.sdMounted || state.maintenance || state.download || recordingBusy(state) || otaActive) {
            return message(request, "Finalize gravações/downloads ou aguarde a manutenção do SD.", "409 Conflict");
        }
        if (protectedPath(path, state) || (!destination.isEmpty() && protectedPath(destination, state))) {
            return message(request, "A raiz, a pasta timelapse e sessões ativas são protegidas.", "400 Bad Request");
        }
        state.maintenance = true;
        command.id = ++state.jobId;
        storageJob = StorageJobStatus{};
        storageJob.id = command.id;
        strlcpy(storageJob.operation, operation == StorageOperation::DeleteTree ? "delete" : "rename", sizeof(storageJob.operation));
        strlcpy(storageJob.phase, "queued", sizeof(storageJob.phase));
        strlcpy(storageJob.path, path.c_str(), sizeof(storageJob.path));
        strlcpy(storageJob.destination, destination.c_str(), sizeof(storageJob.destination));
        strlcpy(state.job, "storage_queued", sizeof(state.job));
    }
    webSdJobs.fetch_add(1);
    if (xQueueSend(storageCommandQueue, &command, 0) != pdTRUE) {
        webSdJobs.fetch_sub(1);
        Guard lock(stateMutex);
        state.maintenance = false;
        strlcpy(storageJob.phase, "failed", sizeof(storageJob.phase));
        strlcpy(storageJob.error, "Fila de operações ocupada.", sizeof(storageJob.error));
        return message(request, "Fila ocupada; nenhuma alteração foi iniciada.", "409 Conflict");
    }
    StaticJsonDocument<512> reply;
    reply["message"] = operation == StorageOperation::DeleteTree ? "Exclusão aceita. Acompanhe o progresso." : "Renomeação aceita. Acompanhe o resultado.";
    reply["job_id"] = command.id;
    return jsonReply(request, reply, "202 Accepted");
}

esp_err_t controlHandler(WebRequest *request) {
    String path = request->uri;
    int question = path.indexOf('?');
    if (question >= 0) path = path.substring(0, question);
    // This endpoint remains available even during an OTA upload or SD drain.
    if (path == "/api/services") {
        if (request->method == HTTP_GET) return servicesStatusHandler(request);
        if (request->method == HTTP_POST) return configureServices(request);
        return message(request, "Método não permitido.", "405 Method Not Allowed");
    }
    Guard mutation(commandMutex, request->method != HTTP_GET && request->method != HTTP_HEAD);
    bool usesWebSd = isWebSdRoute(path);
    ServiceLease webAccess(RC_SERVICE_WEB_SD, webSdRequests, usesWebSd);
    if (usesWebSd && !webAccess.allowed()) return serviceUnavailable(request, "web_sd");
    if (request->method == HTTP_GET && (path == "/" || path == "/settings" || path == "/update")) {
        setResponseType(request, "text/html; charset=utf-8");
        setResponseHeader(request, "Cache-Control", "no-store");
        setResponseHeader(request, "X-Frame-Options", "DENY");
        setResponseHeader(request, "X-Content-Type-Options", "nosniff");
        setResponseHeader(request, "Content-Encoding", "gzip");
        return sendResponse(request, reinterpret_cast<const char *>(WEB_UI_GZIP), sizeof(WEB_UI_GZIP));
    }
    if (request->method == HTTP_GET && path == "/assets/services.js") {
        setResponseType(request, "application/javascript; charset=utf-8");
        setResponseHeader(request, "Cache-Control", "no-store");
        setResponseHeader(request, "Content-Encoding", "gzip");
        return sendResponse(request, reinterpret_cast<const char *>(SERVICES_UI_GZIP), sizeof(SERVICES_UI_GZIP));
    }
    if (request->method == HTTP_GET && path == "/assets/folder-actions.js") {
        setResponseType(request, "application/javascript; charset=utf-8");
        setResponseHeader(request, "Cache-Control", "no-store");
        setResponseHeader(request, "Content-Encoding", "gzip");
        return sendResponse(request, reinterpret_cast<const char *>(FOLDER_ACTIONS_GZIP), sizeof(FOLDER_ACTIONS_GZIP));
    }
    if (request->method == HTTP_GET && path == "/gallery") {
        setResponseType(request, "text/html; charset=utf-8");
        setResponseHeader(request, "Cache-Control", "no-store");
        setResponseHeader(request, "Content-Encoding", "gzip");
        return sendResponse(request, reinterpret_cast<const char *>(GALLERY_UI_GZIP), sizeof(GALLERY_UI_GZIP));
    }
    if (request->method == HTTP_HEAD) {
        if (path == "/stream" || path == "/snapshot" || path == "/raw") {
            if (!snapshotState().ready) return message(request, "Câmera indisponível.", "503 Service Unavailable");
            setResponseType(request, path == "/stream" ? "multipart/x-mixed-replace;boundary=remotecamframe" : "image/jpeg");
            return sendResponse(request, nullptr, 0);
        }
        return message(request, "Método não disponível nesta rota.", "405 Method Not Allowed");
    }
    if (request->method != HTTP_GET && !localMutation(request)) {
        return message(request, "Comandos devem vir da própria câmera/rede local.", "403 Forbidden");
    }
    if (request->method == HTTP_GET) {
        if (path == "/api/health") {
            StaticJsonDocument<256> health;
            State value = snapshotState();
            health["version"] = VERSION;
            health["rssi"] = WiFi.RSSI();
            health["camera"] = value.ready;
            health["sd"] = value.sdMounted;
            return jsonReply(request, health);
        }
        if (path == "/api/status" || path == "/status" || path == "/api/timelapse/status" || path == "/api/sd/status") return statusHandler(request);
        if (path == "/api/sd/list") return listFiles(request);
        if (path == "/stream") return streamHandler(request);
        if (path == "/snapshot" || path == "/raw") return snapshotHandler(request);
        if (path == "/api/sd/download" || path == "/api/sd/archive" || path == "/api/sd/image") return mediaHandler(request);
        return message(request, "Rota não encontrada.", "404 Not Found");
    }
    if (path == "/api/update" && request->method == HTTP_POST) return otaHandler(request);
    StaticJsonDocument<2048> body;
    if (!readJson(request, body)) return message(request, "JSON inválido ou excede 2048 bytes.", "400 Bad Request");
    if (path == "/api/led") {
        if (!body["intensity_pct"].is<int>() || !body["enabled"].is<bool>()) return message(request, "Informe enabled e intensity_pct.", "400 Bad Request");
        int pct = body["intensity_pct"];
        if (pct < 0 || pct > 100) return message(request, "Intensidade fora de 0–100%.", "400 Bad Request");
        {
            Guard lock(stateMutex);
            state.ledEnabled = body["enabled"].as<bool>() && pct > 0;
            if (pct > 0) state.ledPct = pct;
            if (body["auto_flash"].is<bool>()) state.autoFlash = body["auto_flash"].as<bool>();
        }
        applyLed();
        ledChangedAt = millis();
        return message(request, "Iluminação aplicada.");
    }
    if (path == "/api/camera") {
        State value = snapshotState();
        if (!value.ready || recordingBusy(value) || otaActive || strcmp(value.job, "settings_applying") == 0) return message(request, "Câmera ocupada.", "409 Conflict");
        CameraCommand command{};
        command.type = SET_PROFILE;
        command.profile = value.profile;
        if (!parseProfile(body.as<JsonVariantConst>(), command.profile)) return message(request, "Perfil inválido.", "400 Bad Request");
        {
            Guard lock(stateMutex);
            ++state.jobId;
            strlcpy(state.job, "settings_applying", sizeof(state.job));
        }
        if (xQueueSend(cameraQueue, &command, 0) != pdTRUE) {
            setJob("settings_failed");
            return message(request, "Fila ocupada.", "409 Conflict");
        }
        return message(request, "Aplicando perfil.", "202 Accepted");
    }
    if (path == "/api/timelapse/start") {
        State value = snapshotState();
        if (!value.ready || !value.sdMounted || value.maintenance || value.download || otaActive || recordingBusy(value) || strcmp(value.job, "settings_applying") == 0) return message(request, "Câmera/SD ocupados ou indisponíveis.", "409 Conflict");
        CameraCommand command{};
        command.type = START_TIMELAPSE;
        command.profile = value.profile;
        if (!body["interval_seconds"].is<int>() || !parseProfile(body["profile"], command.profile)) return message(request, "Intervalo e perfil obrigatórios.", "400 Bad Request");
        command.intervalSeconds = body["interval_seconds"];
        command.photoPriority = body["photo_priority"] | false;
        command.flash = body["flash"] | false;
        int minimum = command.photoPriority ? 3 : 1;
        if (command.intervalSeconds < minimum || command.intervalSeconds > 86400) return message(request, "Intervalo permitido: 1–86400 s; foto prioritária exige pelo menos 3 s.", "400 Bad Request");
        if (value.freeBytes < SPACE_RESERVE + SLOT_BYTES) return message(request, "Espaço insuficiente no cartão.", "409 Conflict");
        String session = "/timelapse/sessao-" + String(static_cast<uint32_t>(time(nullptr))) + "-" + randomHex(4);
        {
            Guard lock(stateMutex);
            strlcpy(state.phase, "starting", sizeof(state.phase));
            strlcpy(state.session, session.c_str(), sizeof(state.session));
            state.error[0] = 0;
            state.saved = 0;
            state.missed = 0;
            state.sessionBytes = 0;
            state.startedUs = esp_timer_get_time();
            state.endedUs = 0;
            state.nextUs = state.startedUs;
            state.intervalSeconds = command.intervalSeconds;
            state.recordingProfile = command.profile;
            state.photoPriority = command.photoPriority;
            state.recordingFlash = command.flash;
            ++state.jobId;
            strlcpy(state.job, "timelapse_starting", sizeof(state.job));
        }
        stopRecording = false;
        if (xQueueSend(cameraQueue, &command, 0) != pdTRUE) {
            Guard lock(stateMutex);
            strlcpy(state.phase, "error", sizeof(state.phase));
            return message(request, "Fila ocupada; tente novamente.", "409 Conflict");
        }
        return message(request, "Sessão aceita; acompanhe o estado no painel.", "202 Accepted");
    }
    if (path == "/api/timelapse/stop") {
        if (recordingBusy(snapshotState())) stopRecording = true;
        return message(request, "Finalizando gravações pendentes.", "202 Accepted");
    }
    if (path == "/api/sd/mount") {
        Guard lock(stateMutex);
        if (recordingBusy(state) || state.download || state.maintenance || otaActive) return message(request, "SD ocupado.", "409 Conflict");
        state.maintenance = true;
        ++state.jobId;
        strlcpy(state.job, "mounting", sizeof(state.job));
        webSdJobs.fetch_add(1);
        remountRequested = true;
        return message(request, "Remontagem solicitada.", "202 Accepted");
    }
    if (path == "/api/sd/format/prepare") {
        State value = snapshotState();
        if (!value.sdPresent || value.maintenance || value.download || recordingBusy(value) || otaActive) return message(request, "Cartão ausente ou ocupado.", "409 Conflict");
        formatChallenge = randomHex(16);
        formatSerial = value.cardSerial;
        formatExpires = esp_timer_get_time() + 60000000;
        StaticJsonDocument<512> reply;
        reply["challenge"] = formatChallenge;
        reply["card_serial"] = formatSerial;
        reply["total_bytes"] = sdCard ? uint64_t(sdCard->csd.capacity) * sdCard->csd.sector_size : 0;
        reply["message"] = "TODOS os dados do cartão serão apagados. Confirme em até 60 segundos.";
        return jsonReply(request, reply);
    }
    if (path == "/api/sd/format") {
        State value = snapshotState();
        if (formatChallenge.isEmpty() || esp_timer_get_time() > formatExpires || !secureEqual(body["challenge"].as<String>(), formatChallenge) || body["confirmation"].as<String>() != "FORMATAR" || value.cardSerial != formatSerial) return message(request, "Confirmação inválida ou expirada.", "400 Bad Request");
        Guard lock(stateMutex);
        if (!state.sdPresent || state.maintenance || state.download || recordingBusy(state) || otaActive) return message(request, "SD ocupado.", "409 Conflict");
        state.maintenance = true;
        state.sdMounted = false;
        ++state.jobId;
        strlcpy(state.job, "format_queued", sizeof(state.job));
        formatChallenge = "";
        webSdJobs.fetch_add(1);
        formatRequested = true;
        return message(request, "Formatação autorizada. Não remova nem desligue a placa.", "202 Accepted");
    }
    if (path == "/api/sd/rename" && request->method == HTTP_POST) {
        if (!body["path"].is<const char *>() || !body["new_name"].is<const char *>()) return message(request, "Pasta e novo nome são obrigatórios.", "400 Bad Request");
        String itemPath = canonicalPath(body["path"].as<String>());
        String newName = body["new_name"].as<String>();
        if (!validPath(itemPath) || newName.isEmpty() || newName.indexOf('/') >= 0 || newName.indexOf(char(92)) >= 0 || !validPath("/" + newName)) {
            return message(request, "Nome inválido. Informe apenas o nome, sem caminhos.", "400 Bad Request");
        }
        String destination = itemPath.substring(0, itemPath.lastIndexOf('/') + 1) + newName;
        if (!validPath(destination) || destination.equalsIgnoreCase(itemPath)) return message(request, "Informe um nome diferente e com caminho de até 180 bytes.", "400 Bad Request");
        return queueStorageOperation(request, StorageOperation::Rename, itemPath, destination);
    }
    if (path == "/api/sd/item" && request->method == HTTP_DELETE) {
        if (!body["path"].is<const char *>() || body["confirmation"].as<String>() != "EXCLUIR") return message(request, "Confirme a exclusão explicitamente.", "400 Bad Request");
        String itemPath = canonicalPath(body["path"].as<String>());
        if (!validPath(itemPath)) return message(request, "Caminho inválido.", "400 Bad Request");
        // Sem recursive=true, mantém a exclusão de arquivos e pastas vazias.
        if (!body["recursive"].isNull() && !body["recursive"].is<bool>()) return message(request, "recursive deve ser booleano.", "400 Bad Request");
        if (body["recursive"].as<bool>()) return queueStorageOperation(request, StorageOperation::DeleteTree, itemPath);
        State value = snapshotState();
        if (protectedPath(itemPath, value)) return message(request, "Caminho protegido.", "400 Bad Request");
        if (!value.sdMounted || value.maintenance || value.download || recordingBusy(value)) return message(request, "SD ocupado.", "409 Conflict");
        Guard disk(sdMutex);
        if (snapshotState().download) return message(request, "Download ativo.", "409 Conflict");
        struct stat info;
        if (stat(absolutePath(itemPath).c_str(), &info)) return message(request, "Item não encontrado.", "404 Not Found");
        int result = S_ISDIR(info.st_mode) ? rmdir(absolutePath(itemPath).c_str()) : unlink(absolutePath(itemPath).c_str());
        if (!result) updateSpace();
        disk.release();
        return message(request, result == 0 ? "Item excluído." : "Falha. Para pasta não vazia, confirme a exclusão de todo o conteúdo.", result == 0 ? "200 OK" : "409 Conflict");
    }
    if (path == "/api/sd/directory" && request->method == HTTP_POST) {
        String itemPath = canonicalPath(body["path"].as<String>());
        State value = snapshotState();
        if (!validPath(itemPath) || protectedPath(itemPath, value)) return message(request, "Caminho inválido ou protegido.", "400 Bad Request");
        if (!value.sdMounted || value.maintenance || value.download || recordingBusy(value)) return message(request, "SD ocupado.", "409 Conflict");
        Guard disk(sdMutex);
        if (snapshotState().download) return message(request, "Download ativo.", "409 Conflict");
        int result = mkdir(absolutePath(itemPath).c_str(), 0775);
        if (!result) updateSpace();
        disk.release();
        return message(request, result == 0 ? "Pasta criada." : "Nome já existente ou falha no SD.", result == 0 ? "200 OK" : "409 Conflict");
    }
    if (path == "/api/system") {
        State value = snapshotState();
        if (recordingBusy(value) || value.maintenance || value.download || otaActive) return message(request, "Finalize operações antes de alterar o sistema.", "409 Conflict");
        String newHostname = body["hostname"] | hostname;
        if (newHostname.length() < 1 || newHostname.length() > 32 || newHostname[0] == '-' || newHostname.endsWith("-")) return message(request, "Nome mDNS inválido.", "400 Bad Request");
        for (size_t index = 0; index < newHostname.length(); ++index) if (!isalnum(static_cast<unsigned char>(newHostname[index])) && newHostname[index] != '-') return message(request, "Use letras, números e hífen.", "400 Bad Request");
        {
            Guard lock(preferencesMutex);
            preferences.putString("mdns", newHostname);
        }
        rebootAt = millis() + 1500;
        return message(request, "Configuração salva. Reiniciando.");
    }
    return message(request, "Rota não encontrada.", "404 Not Found");
}


void httpWorker(void *argument) {
    auto *request = static_cast<WebRequest *>(argument);
    if (parseRequest(request)) {
        controlHandler(request);
    } else {
        message(request, "Requisição HTTP inválida, incompleta ou muito grande.", "400 Bad Request");
    }
    request->client.stop();
    delete request;
    --httpWorkers;
    vTaskDelete(nullptr);
}

void startServers() {
    httpServer.begin();
    httpServer.setNoDelay(true);
}

void serviceHttpClients() {
    WiFiClient client = httpServer.available();
    if (!client) return;
    client.setNoDelay(true);
    if (httpWorkers.load() >= MAX_HTTP_WORKERS) {
        client.print("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        client.stop();
        return;
    }
    auto *request = new (std::nothrow) WebRequest(client);
    if (!request) {
        client.stop();
        return;
    }
    ++httpWorkers;
    if (xTaskCreatePinnedToCore(httpWorker, "http-client", 12288, request, 1, nullptr, 1) != pdPASS) {
        --httpWorkers;
        client.stop();
        delete request;
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    stateMutex = xSemaphoreCreateMutex();
    commandMutex = xSemaphoreCreateMutex();
    servicesMutex = xSemaphoreCreateMutex();
    sdMutex = xSemaphoreCreateMutex();
    preferencesMutex = xSemaphoreCreateMutex();
    ledMutex = xSemaphoreCreateMutex();
    storageCommandQueue = xQueueCreate(1, sizeof(StorageCommand));
    saveQueue = xQueueCreate(2, sizeof(SaveItem));
    cameraQueue = xQueueCreate(2, sizeof(CameraCommand));
    if (!stateMutex || !commandMutex || !servicesMutex || !sdMutex || !preferencesMutex || !ledMutex || !storageCommandQueue || !saveQueue || !cameraQueue) { Serial.println("FATAL: recursos RTOS"); delay(5000); ESP.restart(); }
    pinMode(33, OUTPUT);
    digitalWrite(33, HIGH);
    ledcSetup(LED_CHANNEL, 5000, 10);
    ledcAttachPin(4, LED_CHANNEL);
    ledcWrite(LED_CHANNEL, 0);
    preferences.begin("cam-pro", false);
    bool hasServicePreferences = preferences.isKey("svc_cfg");
    uint32_t storedServices = hasServicePreferences ? preferences.getUInt("svc_cfg", RC_SERVICE_DEFAULT) : RC_SERVICE_DEFAULT;
    serviceConfiguration.store(rc_service_load(storedServices));
    servicePreferencesValid = hasServicePreferences && rc_service_valid(storedServices);
    String savedName = preferences.getString("mdns", "webcam");
    strlcpy(hostname, savedName.c_str(), sizeof(hostname));
    if (preferences.isKey("admin_pw")) preferences.remove("admin_pw");
    state.ledPct = constrain(preferences.getInt("led_pct", 25), 1, 100);
    state.autoFlash = preferences.getBool("flash_auto", false);
    state.profile.resolution = preferences.getInt("cam_res", FRAMESIZE_VGA);
    if (preferences.getInt("schema", 0) < 2 && preferences.isKey("cam_res")) ++state.profile.resolution;
    if (!validResolution(state.profile.resolution)) state.profile.resolution = FRAMESIZE_VGA;
    state.profile.quality = constrain(preferences.getInt("cam_qual", 16), 10, 63);
    state.profile.brightness = constrain(preferences.getInt("cam_brt", 0), -2, 2);
    state.profile.contrast = constrain(preferences.getInt("cam_con", 0), -2, 2);
    state.profile.saturation = constrain(preferences.getInt("cam_sat", 0), -2, 2);
    state.profile.vflip = preferences.getBool("cam_vf", false);
    state.profile.mirror = preferences.getBool("cam_hm", false);
    Serial.printf("\nRemoteCam %s | reset=%d | PSRAM=%u\n", VERSION, int(esp_reset_reason()), ESP.getPsramSize());
    bool memoryReady = psramFound();
    if (memoryReady) {
        for (int index = 0; index < SLOT_COUNT; ++index) {
            frames[index].data = static_cast<uint8_t *>(heap_caps_malloc(SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!frames[index].data) memoryReady = false;
        }
    }
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    esp_err_t cameraResult = memoryReady ? esp_camera_init(&config) : ESP_ERR_NO_MEM;
    state.ready = cameraResult == ESP_OK && applyProfile(state.profile);
    if (state.ready) {
        warmup(4);
        persistProfile(state.profile);
        Serial.printf("CAMERA OK sensor=0x%04x res=%d qualidade=%d\n", esp_camera_sensor_get()->id.PID, state.profile.resolution, state.profile.quality);
    } else {
        snprintf(state.error, sizeof(state.error), "Câmera/PSRAM indisponível (0x%x).", cameraResult);
        Serial.println(state.error);
    }
    WiFi.setHostname(hostname);
    WiFiManager manager;
    manager.setConfigPortalTimeout(180);
    manager.setConnectTimeout(30);
    if (!manager.autoConnect("ESP32-RemoteCam-Config")) ESP.restart();
    WiFi.setSleep(false);
    serviceBootId = esp_random();
    WiFi.setAutoReconnect(true);
    MDNS.begin(hostname);
    MDNS.addService("http", "tcp", 80);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    state.maintenance = true;
    bool tasksReady = xTaskCreatePinnedToCore(storageTask, "sd-writer", 10240, nullptr, 1, nullptr, 1) == pdPASS;
    tasksReady = tasksReady && xTaskCreatePinnedToCore(cameraTask, "camera-owner", 8192, nullptr, 2, nullptr, 1) == pdPASS;
    if (!tasksReady) { Serial.println("FATAL: tasks"); delay(5000); ESP.restart(); }
    startServers();
    startFtpServer();
    Serial.printf("PAINEL: http://%s/ | http://%s.local/\n", WiFi.localIP().toString().c_str(), hostname);
    Serial.println("HTTP pronto: porta 80; /stream e /snapshot diretos, sob demanda.");
}

void loop() {
    serviceHttpClients();
    serviceFtpClients();
    uint32_t reboot = rebootAt.load();
    if (reboot && static_cast<int32_t>(millis() - reboot) >= 0) {
        ledcWrite(LED_CHANNEL, 0);
        ESP.restart();
    }
    uint32_t changed = ledChangedAt.load();
    if (changed && millis() - changed > 1200 && ledChangedAt.compare_exchange_strong(changed, 0)) {
        State value = snapshotState();
        Guard lock(preferencesMutex);
        preferences.putInt("led_pct", value.ledPct);
        preferences.putBool("flash_auto", value.autoFlash);
    }
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 15000) {
        lastLog = millis();
        State value = snapshotState();
        Serial.printf("RADIO RSSI=%d dBm\n", WiFi.RSSI());
        Serial.printf("STATUS heap=%u psram=%u captura=%lu envio=%lu erros=%lu SD=%d TL=%s fotos=%lu\n", ESP.getFreeHeap(), ESP.getFreePsram(), static_cast<unsigned long>(value.captured), static_cast<unsigned long>(value.sent), static_cast<unsigned long>(value.captureErrors), value.sdMounted, value.phase, static_cast<unsigned long>(value.saved));
    }
    delay(20);
}
