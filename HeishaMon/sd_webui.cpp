#include "sd_webui.h"
#include "custom_version.h"

#if defined(ESP32)

#include <ArduinoJson.h>
#include <SD_MMC.h>
#include <dirent.h>
#include <errno.h>
#include <mbedtls/sha256.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern void log_message(char *string);

namespace {

constexpr uint8_t ROUTE_SD_WEBUI_FILE = 200;
constexpr uint8_t ROUTE_SD_WEBUI_STATUS = 201;
constexpr uint8_t ROUTE_SD_WEBUI_UPLOAD = 202;

constexpr size_t TAR_BLOCK_SIZE = 512;
constexpr size_t FILE_CHUNK_SIZE = 2048;
constexpr size_t MAX_WEBUI_PACKAGE_SIZE = 2U * 1024U * 1024U;
constexpr size_t MAX_WEBUI_FILE_SIZE = 512U * 1024U;
constexpr uint8_t MAX_WEBUI_FILES = 32;

constexpr const char *WEBUI_ROOT = "/sd/heishamon/webui";
constexpr const char *WEBUI_ACTIVE_FILE = "/sd/heishamon/webui/active.txt";

enum class RequestKind : uint8_t {
  FILE,
  STATUS,
  UPLOAD
};

enum class TarState : uint8_t {
  HEADER,
  DATA,
  PADDING,
  COMPLETE,
  ERROR
};

struct SdWebUiRequest {
  RequestKind kind = RequestKind::FILE;
  char relativePath[64] = {0};
  bool pageRequest = false;
  FILE *file = nullptr;
  size_t remaining = 0;

  char targetSlot = 'a';
  TarState tarState = TarState::HEADER;
  uint8_t *tarHeader = nullptr;
  size_t tarHeaderUsed = 0;
  size_t tarDataRemaining = 0;
  size_t tarPaddingRemaining = 0;
  size_t uploadedBytes = 0;
  uint8_t zeroBlocks = 0;
  uint8_t extractedFiles = 0;
  bool uploadFieldSeen = false;
  char error[128] = {0};
};

char activeSlot = '\0';
char activeVersion[32] = {0};
char activeCustomVersion[32] = {0};
char activeBaseVersion[32] = {0};
bool activeChecked = false;
bool cardPreviouslyReady = false;

static const char recoveryPage[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HeishaMon Web UI Recovery</title><style>
body{font-family:Arial,sans-serif;max-width:720px;margin:48px auto;padding:0 20px;background:#f4f6f8;color:#24303a}
.box{background:#fff;border:1px solid #dce2e7;border-radius:10px;padding:24px;box-shadow:0 3px 14px #0001}
a{display:inline-block;margin:8px 12px 0 0;padding:10px 16px;border-radius:6px;background:#198ec1;color:#fff;text-decoration:none}
code{background:#eef1f3;padding:2px 5px;border-radius:4px}</style></head><body><div class="box">
<h1>Custom Web UI unavailable</h1><p>The active Web UI package is missing or invalid, or the SD card is unavailable.</p>
<p>Open the firmware page and upload <code>heishamon-webui.tar</code>. The embedded HeishaMon pages remain available.</p>
<a href="/firmware">Firmware &amp; Web UI Update</a><a href="/">Embedded Home</a><a href="/settings">Settings</a>
</div></body></html>)HTML";

struct PageRoute {
  const char *uri;
  const char *file;
};

constexpr PageRoute PAGE_ROUTES[] = {
  {"/dashboard", "dashboard.html"},
  {"/wpsettings", "wpsettings.html"},
  {"/scheduler", "scheduler.html"},
  {"/externalsensors", "externalsensors.html"},
  {"/hardware", "hardware.html"},
  {"/diagnostics", "diagnostics.html"},
  {"/history", "history.html"},
};

constexpr const char *REQUIRED_FILES[] = {
  "styles.css", "common.js", "websocket.js",
  "dashboard.html", "dashboard.js", "wpsettings.html", "wpsettings.js",
  "scheduler.html", "scheduler.js", "externalsensors.html", "externalsensors.js",
  "hardware.html", "hardware.js",
  "diagnostics.html", "history.html"
};

bool cardReady() {
  return SD_MMC.cardType() != CARD_NONE;
}

void logLine(const char *format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  log_message(line);
}

bool ensureDirectory(const char *path) {
  struct stat info = {};
  if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
  return mkdir(path, 0775) == 0 || errno == EEXIST;
}

bool ensureWebUiDirectories() {
  return ensureDirectory("/sd/heishamon") && ensureDirectory(WEBUI_ROOT) &&
    ensureDirectory("/sd/heishamon/webui/a") &&
    ensureDirectory("/sd/heishamon/webui/b");
}

void slotPath(char slot, const char *relativePath, char *path, size_t pathSize) {
  snprintf(path, pathSize, "%s/%c/%s", WEBUI_ROOT, slot, relativePath);
}

bool safePackagePath(const char *path) {
  if (path == nullptr || path[0] == '\0' || strlen(path) >= 64) return false;
  if (path[0] == '.' || strchr(path, '/') != nullptr || strchr(path, '\\') != nullptr) return false;
  for (const char *cursor = path; *cursor; cursor++) {
    char value = *cursor;
    if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-')) return false;
  }
  return true;
}

bool clearSlot(char slot) {
  char directory[48];
  snprintf(directory, sizeof(directory), "%s/%c", WEBUI_ROOT, slot);
  DIR *root = opendir(directory);
  if (root == nullptr) return ensureDirectory(directory);
  bool ok = true;
  while (dirent *entry = readdir(root)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (!safePackagePath(entry->d_name)) {
      ok = false;
      continue;
    }
    char path[128];
    slotPath(slot, entry->d_name, path, sizeof(path));
    if (unlink(path) != 0 && errno != ENOENT) ok = false;
  }
  closedir(root);
  return ok;
}

uint32_t parseOctal(const uint8_t *value, size_t length, bool *valid = nullptr) {
  uint32_t result = 0;
  bool any = false;
  for (size_t index = 0; index < length; index++) {
    uint8_t digit = value[index];
    if (digit == 0 || digit == ' ') {
      if (any) break;
      continue;
    }
    if (digit < '0' || digit > '7') {
      if (valid) *valid = false;
      return 0;
    }
    any = true;
    result = (result << 3) + (digit - '0');
  }
  if (valid) *valid = any;
  return result;
}

bool allZero(const uint8_t *data, size_t length) {
  for (size_t index = 0; index < length; index++) if (data[index] != 0) return false;
  return true;
}

void failUpload(SdWebUiRequest *request, const char *format, ...) {
  if (request == nullptr || request->tarState == TarState::ERROR) return;
  va_list args;
  va_start(args, format);
  vsnprintf(request->error, sizeof(request->error), format, args);
  va_end(args);
  request->tarState = TarState::ERROR;
  if (request->file != nullptr) {
    fclose(request->file);
    request->file = nullptr;
  }
}

bool startTarEntry(SdWebUiRequest *request) {
  if (allZero(request->tarHeader, TAR_BLOCK_SIZE)) {
    request->zeroBlocks++;
    request->tarHeaderUsed = 0;
    if (request->zeroBlocks >= 2) request->tarState = TarState::COMPLETE;
    return true;
  }
  request->zeroBlocks = 0;
  if (memcmp(request->tarHeader + 257, "ustar", 5) != 0) {
    failUpload(request, "Package is not an uncompressed USTAR archive");
    return false;
  }
  bool checksumValid = false;
  uint32_t expectedChecksum = parseOctal(request->tarHeader + 148, 8, &checksumValid);
  uint32_t calculatedChecksum = 0;
  for (size_t index = 0; index < TAR_BLOCK_SIZE; index++) {
    calculatedChecksum += index >= 148 && index < 156 ? ' ' : request->tarHeader[index];
  }
  if (!checksumValid || expectedChecksum != calculatedChecksum) {
    failUpload(request, "USTAR header checksum mismatch");
    return false;
  }
  char name[101] = {0};
  memcpy(name, request->tarHeader, 100);
  if (!safePackagePath(name)) {
    failUpload(request, "Unsafe package path");
    return false;
  }
  uint8_t type = request->tarHeader[156];
  if (type != 0 && type != '0') {
    failUpload(request, "Only regular files are allowed in the package");
    return false;
  }
  bool sizeValid = false;
  uint32_t size = parseOctal(request->tarHeader + 124, 12, &sizeValid);
  if (!sizeValid || size > MAX_WEBUI_FILE_SIZE) {
    failUpload(request, "Invalid or oversized Web UI file");
    return false;
  }
  if (request->extractedFiles >= MAX_WEBUI_FILES) {
    failUpload(request, "Too many files in Web UI package");
    return false;
  }
  char path[128];
  slotPath(request->targetSlot, name, path, sizeof(path));
  request->file = fopen(path, "wb");
  if (request->file == nullptr) {
    failUpload(request, "Could not create %s: %d", name, errno);
    return false;
  }
  request->extractedFiles++;
  request->tarDataRemaining = size;
  request->tarPaddingRemaining = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
  request->tarHeaderUsed = 0;
  if (size == 0) {
    fclose(request->file);
    request->file = nullptr;
    request->tarState = request->tarPaddingRemaining ? TarState::PADDING : TarState::HEADER;
  } else {
    request->tarState = TarState::DATA;
  }
  return true;
}

bool feedTar(SdWebUiRequest *request, const uint8_t *data, size_t length) {
  if (request == nullptr || data == nullptr || request->tarState == TarState::ERROR) return false;
  request->uploadedBytes += length;
  if (request->uploadedBytes > MAX_WEBUI_PACKAGE_SIZE) {
    failUpload(request, "Web UI package exceeds 2 MB");
    return false;
  }
  size_t position = 0;
  while (position < length && request->tarState != TarState::ERROR) {
    if (request->tarState == TarState::COMPLETE) return true;
    if (request->tarState == TarState::HEADER) {
      size_t count = min(length - position, TAR_BLOCK_SIZE - request->tarHeaderUsed);
      memcpy(request->tarHeader + request->tarHeaderUsed, data + position, count);
      request->tarHeaderUsed += count;
      position += count;
      if (request->tarHeaderUsed == TAR_BLOCK_SIZE && !startTarEntry(request)) return false;
    } else if (request->tarState == TarState::DATA) {
      size_t count = min(length - position, request->tarDataRemaining);
      if (count > 0 && (request->file == nullptr || fwrite(data + position, 1, count, request->file) != count)) {
        failUpload(request, "Could not write Web UI package: %d", errno);
        return false;
      }
      request->tarDataRemaining -= count;
      position += count;
      if (request->tarDataRemaining == 0) {
        if (request->file != nullptr && fclose(request->file) != 0) {
          request->file = nullptr;
          failUpload(request, "Could not finish Web UI file: %d", errno);
          return false;
        }
        request->file = nullptr;
        request->tarState = request->tarPaddingRemaining ? TarState::PADDING : TarState::HEADER;
      }
    } else if (request->tarState == TarState::PADDING) {
      size_t count = min(length - position, request->tarPaddingRemaining);
      request->tarPaddingRemaining -= count;
      position += count;
      if (request->tarPaddingRemaining == 0) request->tarState = TarState::HEADER;
    }
  }
  return request->tarState != TarState::ERROR;
}

bool sha256File(const char *path, char output[65], size_t *sizeOut) {
  FILE *file = fopen(path, "rb");
  if (file == nullptr) return false;
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  if (mbedtls_sha256_starts(&context, 0) != 0) {
    fclose(file);
    mbedtls_sha256_free(&context);
    return false;
  }
  uint8_t buffer[2048];
  size_t total = 0;
  bool ok = true;
  while (true) {
    size_t count = fread(buffer, 1, sizeof(buffer), file);
    if (count > 0) {
      total += count;
      if (mbedtls_sha256_update(&context, buffer, count) != 0) {
        ok = false;
        break;
      }
    }
    if (count < sizeof(buffer)) {
      if (ferror(file)) ok = false;
      break;
    }
    yield();
  }
  uint8_t digest[32];
  if (ok && mbedtls_sha256_finish(&context, digest) != 0) ok = false;
  mbedtls_sha256_free(&context);
  fclose(file);
  if (!ok) return false;
  for (size_t index = 0; index < sizeof(digest); index++) sprintf(output + index * 2, "%02x", digest[index]);
  output[64] = '\0';
  if (sizeOut) *sizeOut = total;
  return true;
}

bool validateSlot(char slot, char *version, size_t versionSize,
    char *customVersion, size_t customVersionSize, char *baseVersion,
    size_t baseVersionSize, char *error, size_t errorSize) {
  char manifestPath[128];
  slotPath(slot, "manifest.json", manifestPath, sizeof(manifestPath));
  FILE *file = fopen(manifestPath, "rb");
  if (file == nullptr) {
    snprintf(error, errorSize, "manifest.json missing");
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    snprintf(error, errorSize, "Could not read manifest");
    return false;
  }
  long fileSize = ftell(file);
  rewind(file);
  if (fileSize <= 0 || fileSize > 16384) {
    fclose(file);
    snprintf(error, errorSize, "Manifest size is invalid");
    return false;
  }
  char *buffer = (char *)malloc((size_t)fileSize + 1);
  if (buffer == nullptr) {
    fclose(file);
    snprintf(error, errorSize, "Out of memory validating manifest");
    return false;
  }
  bool readOk = fread(buffer, 1, (size_t)fileSize, file) == (size_t)fileSize;
  fclose(file);
  buffer[fileSize] = '\0';
  JsonDocument document;
  DeserializationError jsonError = deserializeJson(document, buffer, (size_t)fileSize);
  free(buffer);
  if (!readOk || jsonError || document["format"].as<int>() != 1 ||
      !document["version"].is<const char *>() ||
      !document["customVersion"].is<const char *>() ||
      !document["baseVersion"].is<const char *>() ||
      !document["files"].is<JsonArray>()) {
    snprintf(error, errorSize, "Manifest content is invalid");
    return false;
  }
  snprintf(version, versionSize, "%s", document["version"].as<const char *>());
  snprintf(customVersion, customVersionSize, "%s",
    document["customVersion"].as<const char *>());
  snprintf(baseVersion, baseVersionSize, "%s", document["baseVersion"].as<const char *>());
  bool requiredFound[sizeof(REQUIRED_FILES) / sizeof(REQUIRED_FILES[0])] = {false};
  JsonArray files = document["files"].as<JsonArray>();
  if (files.size() == 0 || files.size() > MAX_WEBUI_FILES) {
    snprintf(error, errorSize, "Manifest file list is invalid");
    return false;
  }
  for (JsonObject entry : files) {
    const char *relativePath = entry["path"];
    const char *expectedHash = entry["sha256"];
    size_t expectedSize = entry["size"] | (size_t)SIZE_MAX;
    if (!safePackagePath(relativePath) || expectedHash == nullptr || strlen(expectedHash) != 64 ||
        expectedSize == SIZE_MAX || expectedSize > MAX_WEBUI_FILE_SIZE) {
      snprintf(error, errorSize, "Manifest file entry is invalid");
      return false;
    }
    char path[128], actualHash[65];
    size_t actualSize = 0;
    slotPath(slot, relativePath, path, sizeof(path));
    if (!sha256File(path, actualHash, &actualSize) || actualSize != expectedSize ||
        strcasecmp(actualHash, expectedHash) != 0) {
      snprintf(error, errorSize, "Validation failed for %.48s", relativePath);
      return false;
    }
    for (size_t index = 0; index < sizeof(REQUIRED_FILES) / sizeof(REQUIRED_FILES[0]); index++) {
      if (strcmp(relativePath, REQUIRED_FILES[index]) == 0) requiredFound[index] = true;
    }
  }
  for (bool found : requiredFound) {
    if (!found) {
      snprintf(error, errorSize, "Required Web UI file missing");
      return false;
    }
  }
  return true;
}

bool writeActiveSlot(char slot) {
  const char *temporary = "/sd/heishamon/webui/active.tmp";
  const char *backup = "/sd/heishamon/webui/active.bak";
  FILE *file = fopen(temporary, "wb");
  if (file == nullptr) return false;
  bool ok = fputc(slot, file) != EOF && fputc('\n', file) != EOF;
  if (fclose(file) != 0) ok = false;
  if (!ok) {
    unlink(temporary);
    return false;
  }
  unlink(backup);
  bool hadActive = rename(WEBUI_ACTIVE_FILE, backup) == 0;
  if (!hadActive && errno != ENOENT) {
    unlink(temporary);
    return false;
  }
  if (rename(temporary, WEBUI_ACTIVE_FILE) != 0) {
    if (hadActive) rename(backup, WEBUI_ACTIVE_FILE);
    unlink(temporary);
    return false;
  }
  unlink(backup);
  return true;
}

void loadActiveSlot() {
  activeChecked = true;
  activeSlot = '\0';
  activeVersion[0] = '\0';
  activeCustomVersion[0] = '\0';
  activeBaseVersion[0] = '\0';
  if (!cardReady() || !ensureWebUiDirectories()) return;
  FILE *file = fopen(WEBUI_ACTIVE_FILE, "rb");
  if (file == nullptr) {
    const char *backup = "/sd/heishamon/webui/active.bak";
    if (rename(backup, WEBUI_ACTIVE_FILE) == 0) file = fopen(WEBUI_ACTIVE_FILE, "rb");
  }
  if (file == nullptr) return;
  int value = fgetc(file);
  fclose(file);
  if (value != 'a' && value != 'b') return;
  char error[96] = {0};
  if (!validateSlot((char)value, activeVersion, sizeof(activeVersion),
      activeCustomVersion, sizeof(activeCustomVersion), activeBaseVersion,
      sizeof(activeBaseVersion), error, sizeof(error))) {
    logLine("[WEBUI] Active slot %c invalid: %s", (char)value, error);
    return;
  }
  activeSlot = (char)value;
  logLine("[WEBUI] SD Web UI %s active in slot %c", activeVersion, activeSlot);
}

const char *contentType(const char *path) {
  const char *extension = strrchr(path, '.');
  if (extension == nullptr) return "application/octet-stream";
  if (strcmp(extension, ".html") == 0) return "text/html";
  if (strcmp(extension, ".css") == 0) return "text/css";
  if (strcmp(extension, ".js") == 0) return "application/javascript";
  if (strcmp(extension, ".json") == 0) return "application/json";
  return "application/octet-stream";
}

void destroyRequest(SdWebUiRequest *request) {
  if (request == nullptr) return;
  if (request->file != nullptr) fclose(request->file);
  delete[] request->tarHeader;
  delete request;
}

void sendRecovery(struct webserver_t *client, SdWebUiRequest *request) {
  webserver_send(client, 503, (char *)"text/html", strlen_P(recoveryPage));
  webserver_send_content_P(client, recoveryPage, strlen_P(recoveryPage));
  destroyRequest(request);
  client->userdata = nullptr;
}

void serveFile(struct webserver_t *client, SdWebUiRequest *request) {
  if (request == nullptr) return;
  if (client->content == 0) {
    if (!activeChecked) loadActiveSlot();
    if (activeSlot == '\0') {
      if (request->pageRequest) sendRecovery(client, request);
      else {
        webserver_send(client, 404, (char *)"text/plain", 13);
        webserver_send_content_P(client, PSTR("404 Not found"), 13);
        destroyRequest(request);
        client->userdata = nullptr;
      }
      return;
    }
    char path[128];
    slotPath(activeSlot, request->relativePath, path, sizeof(path));
    request->file = fopen(path, "rb");
    if (request->file == nullptr || fseek(request->file, 0, SEEK_END) != 0) {
      if (request->pageRequest) sendRecovery(client, request);
      else {
        webserver_send(client, 404, (char *)"text/plain", 13);
        webserver_send_content_P(client, PSTR("404 Not found"), 13);
        destroyRequest(request);
        client->userdata = nullptr;
      }
      return;
    }
    long size = ftell(request->file);
    rewind(request->file);
    if (size < 0) {
      sendRecovery(client, request);
      return;
    }
    request->remaining = (size_t)size;
    webserver_send(client, 200, (char *)contentType(request->relativePath), 0);
  }
  if (request->file == nullptr) return;
  char chunk[FILE_CHUNK_SIZE];
  size_t count = min(request->remaining, sizeof(chunk));
  if (count > 0) count = fread(chunk, 1, count, request->file);
  if (count > 0) {
    webserver_send_content(client, chunk, count);
    request->remaining -= count;
  }
  if (request->remaining == 0 || count == 0) {
    destroyRequest(request);
    client->userdata = nullptr;
  }
}

void sendStatus(struct webserver_t *client, SdWebUiRequest *request) {
  if (client->content != 0) return;
  if (!activeChecked && cardReady()) loadActiveSlot();
  JsonDocument document;
  document["sdReady"] = cardReady();
  document["active"] = activeSlot == 'a' || activeSlot == 'b';
  char slotText[2] = {activeSlot, '\0'};
  if (activeSlot == 'a' || activeSlot == 'b') document["slot"] = slotText;
  else document["slot"] = nullptr;
  if (activeVersion[0]) document["version"] = activeVersion;
  else document["version"] = nullptr;
  if (activeCustomVersion[0]) document["customVersion"] = activeCustomVersion;
  else document["customVersion"] = nullptr;
  if (activeBaseVersion[0]) document["baseVersion"] = activeBaseVersion;
  else document["baseVersion"] = nullptr;
  document["firmwareVersion"] = CUSTOM_FIRMWARE_VERSION;
  document["expectedWebUiVersion"] = CUSTOM_WEBUI_VERSION;
  document["releaseMatchesFirmware"] = activeCustomVersion[0] != '\0' &&
    strcmp(activeCustomVersion, CUSTOM_FIRMWARE_VERSION) == 0;
  document["exactVersionMatchesFirmware"] = activeVersion[0] != '\0' &&
    strcmp(activeVersion, CUSTOM_WEBUI_VERSION) == 0;
  document["uploadPath"] = "/webui/upload";
  size_t length = measureJson(document);
  char *response = (char *)malloc(length + 1);
  if (response == nullptr) {
    webserver_send(client, 503, (char *)"text/plain", 13);
    webserver_send_content_P(client, PSTR("Out of memory"), 13);
  } else {
    serializeJson(document, response, length + 1);
    webserver_send(client, 200, (char *)"application/json", length);
    webserver_send_content(client, response, length);
    free(response);
  }
  destroyRequest(request);
  client->userdata = nullptr;
}

void finishUpload(struct webserver_t *client, SdWebUiRequest *request) {
  if (client->content != 0 || request == nullptr) return;
  char message[192];
  bool success = false;
  if (!request->uploadFieldSeen) {
    snprintf(message, sizeof(message), "ERROR: no Web UI package received");
  } else if (request->tarState == TarState::ERROR) {
    snprintf(message, sizeof(message), "ERROR: %s", request->error);
  } else if (request->tarState != TarState::COMPLETE) {
    snprintf(message, sizeof(message), "ERROR: incomplete Web UI package");
  } else {
    char version[32] = {0};
    char customVersion[32] = {0};
    char baseVersion[32] = {0};
    char validationError[128] = {0};
    if (!validateSlot(request->targetSlot, version, sizeof(version),
        customVersion, sizeof(customVersion), baseVersion, sizeof(baseVersion),
        validationError, sizeof(validationError))) {
      snprintf(message, sizeof(message), "ERROR: %s", validationError);
    } else if (!writeActiveSlot(request->targetSlot)) {
      snprintf(message, sizeof(message), "ERROR: could not activate Web UI slot");
    } else {
      activeSlot = request->targetSlot;
      snprintf(activeVersion, sizeof(activeVersion), "%s", version);
      snprintf(activeCustomVersion, sizeof(activeCustomVersion), "%s", customVersion);
      snprintf(activeBaseVersion, sizeof(activeBaseVersion), "%s", baseVersion);
      activeChecked = true;
      success = true;
      snprintf(message, sizeof(message), "OK: Web UI %s activated in slot %c",
        activeVersion, activeSlot);
      logLine("[WEBUI] %s", message + 4);
    }
  }
  webserver_send(client, success ? 200 : 400, (char *)"text/plain", strlen(message));
  webserver_send_content(client, message, strlen(message));
  destroyRequest(request);
  client->userdata = nullptr;
}

SdWebUiRequest *createRequest(RequestKind kind) {
  SdWebUiRequest *request = new (std::nothrow) SdWebUiRequest();
  if (request != nullptr) {
    request->kind = kind;
    if (kind == RequestKind::UPLOAD) {
      request->tarHeader = new (std::nothrow) uint8_t[TAR_BLOCK_SIZE]();
      if (request->tarHeader == nullptr) {
        delete request;
        request = nullptr;
      }
    }
  }
  return request;
}

}  // namespace

void sdWebUiBegin() {
  activeSlot = '\0';
  activeVersion[0] = '\0';
  activeCustomVersion[0] = '\0';
  activeBaseVersion[0] = '\0';
  activeChecked = false;
  cardPreviouslyReady = false;
}

void sdWebUiLoop() {
  bool ready = cardReady();
  if (ready && !cardPreviouslyReady) loadActiveSlot();
  if (!ready && cardPreviouslyReady) {
    activeSlot = '\0';
    activeVersion[0] = '\0';
    activeCustomVersion[0] = '\0';
    activeBaseVersion[0] = '\0';
    activeChecked = false;
  }
  cardPreviouslyReady = ready;
}

bool sdWebUiHandleUri(struct webserver_t *client, const char *uri) {
  if (client == nullptr || uri == nullptr) return false;
  const char *relativePath = nullptr;
  bool pageRequest = false;
  for (const PageRoute &route : PAGE_ROUTES) {
    if (strcmp(uri, route.uri) == 0) {
      relativePath = route.file;
      pageRequest = true;
      break;
    }
  }
  if (relativePath == nullptr && strncmp(uri, "/webui/", 7) == 0 &&
      strcmp(uri, "/webui/status") != 0 && strcmp(uri, "/webui/upload") != 0) {
    relativePath = uri + 7;
    if (!safePackagePath(relativePath)) return false;
  }
  if (relativePath != nullptr) {
    SdWebUiRequest *request = createRequest(RequestKind::FILE);
    if (request == nullptr) return false;
    snprintf(request->relativePath, sizeof(request->relativePath), "%s", relativePath);
    request->pageRequest = pageRequest;
    client->userdata = request;
    client->route = ROUTE_SD_WEBUI_FILE;
    return true;
  }
  if (strcmp(uri, "/webui/status") == 0) {
    SdWebUiRequest *request = createRequest(RequestKind::STATUS);
    if (request == nullptr) return false;
    client->userdata = request;
    client->route = ROUTE_SD_WEBUI_STATUS;
    return true;
  }
  if (strcmp(uri, "/webui/upload") == 0) {
    SdWebUiRequest *request = createRequest(RequestKind::UPLOAD);
    if (request == nullptr) return false;
    if (!activeChecked && cardReady()) loadActiveSlot();
    request->targetSlot = activeSlot == 'a' ? 'b' : 'a';
    if (client->method != 1) failUpload(request, "Web UI upload requires POST");
    else if (!cardReady() || !ensureWebUiDirectories()) failUpload(request, "SD card is not ready");
    else if (!clearSlot(request->targetSlot)) failUpload(request, "Could not prepare inactive Web UI slot");
    client->userdata = request;
    client->route = ROUTE_SD_WEBUI_UPLOAD;
    return true;
  }
  return false;
}

bool sdWebUiHandleArgs(struct webserver_t *client, struct arguments_t *args) {
  if (client == nullptr || client->route != ROUTE_SD_WEBUI_UPLOAD) return false;
  SdWebUiRequest *request = (SdWebUiRequest *)client->userdata;
  if (request == nullptr || args == nullptr) return true;
  if (strcmp((char *)args->name, "webui") == 0) {
    request->uploadFieldSeen = true;
    feedTar(request, args->value, args->len);
  }
  return true;
}

bool sdWebUiHandleWrite(struct webserver_t *client) {
  if (client == nullptr) return false;
  SdWebUiRequest *request = (SdWebUiRequest *)client->userdata;
  switch (client->route) {
    case ROUTE_SD_WEBUI_FILE:
      serveFile(client, request);
      return true;
    case ROUTE_SD_WEBUI_STATUS:
      sendStatus(client, request);
      return true;
    case ROUTE_SD_WEBUI_UPLOAD:
      finishUpload(client, request);
      return true;
    default:
      return false;
  }
}

bool sdWebUiHandleHeader(struct webserver_t *client, struct header_t *header) {
  if (client == nullptr || header == nullptr || client->route != ROUTE_SD_WEBUI_FILE) return false;
  SdWebUiRequest *request = (SdWebUiRequest *)client->userdata;
  const char *cache = request != nullptr && !request->pageRequest ?
    "Cache-Control: public, max-age=86400\r\n" :
    "Cache-Control: no-cache\r\n";
  header->ptr += snprintf((char *)header->buffer, 512 - header->ptr, "%s", cache);
  return true;
}

bool sdWebUiHandleClose(struct webserver_t *client) {
  if (client == nullptr || (client->route != ROUTE_SD_WEBUI_FILE &&
      client->route != ROUTE_SD_WEBUI_STATUS && client->route != ROUTE_SD_WEBUI_UPLOAD)) return false;
  SdWebUiRequest *request = (SdWebUiRequest *)client->userdata;
  if (request != nullptr && request->kind == RequestKind::UPLOAD &&
      request->tarState != TarState::COMPLETE) clearSlot(request->targetSlot);
  destroyRequest(request);
  client->userdata = nullptr;
  return true;
}

#else

void sdWebUiBegin() {}
void sdWebUiLoop() {}
bool sdWebUiHandleUri(struct webserver_t *, const char *) { return false; }
bool sdWebUiHandleArgs(struct webserver_t *, struct arguments_t *) { return false; }
bool sdWebUiHandleWrite(struct webserver_t *) { return false; }
bool sdWebUiHandleHeader(struct webserver_t *, struct header_t *) { return false; }
bool sdWebUiHandleClose(struct webserver_t *) { return false; }

#endif
