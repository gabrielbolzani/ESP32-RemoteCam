#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_err.h>
#include <http_parser.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <algorithm>
#include <vector>
#include <errno.h>

// Um worker por conexão. Um stream longo não bloqueia status, snapshot ou SD.
// O parser HTTP é o http_parser distribuído pelo próprio SDK Espressif.
struct HttpHeader {
    String name;
    String value;
};

struct WebRequest {
    WiFiClient client;
    http_method method = HTTP_GET;
    String uri;
    size_t content_len = 0;
    size_t bodyRead = 0;
    std::vector<uint8_t> bodyPrefix;
    size_t prefixRead = 0;
    std::vector<HttpHeader> headers;
    std::vector<HttpHeader> responseHeaders;
    String responseStatus = "200 OK";
    String responseType = "application/json; charset=utf-8";
    String parserField;
    String parserValue;
    bool parserReadingValue = false;
    bool headersSent = false;
    bool responseChunked = false;
    bool http11 = true;
    bool incomingChunked = false;
    bool failed = false;
    uint32_t startedAt = millis();

    explicit WebRequest(WiFiClient connection) : client(connection) {
        headers.reserve(12);
        responseHeaders.reserve(6);
    }

    String header(const char *name) const {
        for (const auto &item : headers) {
            if (item.name.equalsIgnoreCase(name)) return item.value;
        }
        return "";
    }

    bool flushParsedHeader() {
        if (parserField.isEmpty()) return true;
        if (headers.size() >= 24) return false;
        parserValue.trim();
        headers.push_back({parserField, parserValue});
        parserField = "";
        parserValue = "";
        return true;
    }
};

static constexpr int RECEIVE_TIMEOUT = -3;

inline int socketWrite(WebRequest *request, const char *bytes, size_t length) {
    size_t offset = 0;
    uint32_t progress = millis();
    while (offset < length && request->client.connected()) {
        size_t count = std::min(length - offset, size_t(4096));
        int written = send(request->client.fd(), bytes + offset, count, MSG_DONTWAIT);
        if (written > 0) {
            offset += written;
            progress = millis();
        } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            request->failed = true;
            return -1;
        }
        if (millis() - progress > 4000) {
            request->failed = true;
            return -1;
        }
        vTaskDelay(1);
    }
    if (offset != length) request->failed = true;
    return offset == length ? int(offset) : -1;
}

inline esp_err_t setResponseStatus(WebRequest *request, const char *status) {
    request->responseStatus = status;
    return ESP_OK;
}

inline esp_err_t setResponseType(WebRequest *request, const char *type) {
    request->responseType = type;
    return ESP_OK;
}

inline esp_err_t setResponseHeader(WebRequest *request, const char *name, const char *value) {
    if (request->headersSent || request->responseHeaders.size() >= 12) return ESP_FAIL;
    request->responseHeaders.push_back({String(name), String(value)});
    return ESP_OK;
}

inline esp_err_t sendHeaders(WebRequest *request, bool knownLength, size_t length = 0) {
    if (request->headersSent) return ESP_OK;
    String block;
    block.reserve(640);
    block += request->http11 ? "HTTP/1.1 " : "HTTP/1.0 ";
    block += request->responseStatus + "\r\nContent-Type: " + request->responseType + "\r\n";
    if (knownLength) {
        block += "Content-Length: " + String(length) + "\r\n";
    } else if (request->http11) {
        request->responseChunked = true;
        block += "Transfer-Encoding: chunked\r\n";
    }
    block += "Connection: close\r\n";
    for (const auto &item : request->responseHeaders) {
        block += item.name + ": " + item.value + "\r\n";
    }
    block += "\r\n";
    request->headersSent = true;
    return socketWrite(request, block.c_str(), block.length()) < 0 ? ESP_FAIL : ESP_OK;
}

inline esp_err_t sendResponse(WebRequest *request, const char *body, size_t length) {
    if (sendHeaders(request, true, length) != ESP_OK) return ESP_FAIL;
    if (request->method == HTTP_HEAD || length == 0) return ESP_OK;
    return socketWrite(request, body, length) < 0 ? ESP_FAIL : ESP_OK;
}

inline esp_err_t sendChunk(WebRequest *request, const char *body, size_t length) {
    if (sendHeaders(request, false) != ESP_OK) return ESP_FAIL;
    if (!request->responseChunked) {
        if (!body || !length) return ESP_OK;
        return socketWrite(request, body, length) < 0 ? ESP_FAIL : ESP_OK;
    }
    if (!body || !length) {
        return socketWrite(request, "0\r\n\r\n", 5) < 0 ? ESP_FAIL : ESP_OK;
    }
    char sizeLine[24];
    int count = snprintf(sizeLine, sizeof(sizeLine), "%x\r\n", unsigned(length));
    if (socketWrite(request, sizeLine, count) < 0) return ESP_FAIL;
    if (socketWrite(request, body, length) < 0) return ESP_FAIL;
    return socketWrite(request, "\r\n", 2) < 0 ? ESP_FAIL : ESP_OK;
}

inline int receiveBody(WebRequest *request, char *buffer, size_t length) {
    length = std::min(length, request->content_len - request->bodyRead);
    if (!length) return 0;
    size_t prefixed = request->bodyPrefix.size() - request->prefixRead;
    if (prefixed) {
        size_t count = std::min(prefixed, length);
        memcpy(buffer, request->bodyPrefix.data() + request->prefixRead, count);
        request->prefixRead += count;
        request->bodyRead += count;
        return count;
    }
    uint32_t start = millis();
    while (request->client.connected()) {
        int count = recv(request->client.fd(), buffer, length, MSG_DONTWAIT);
        if (count > 0) {
            request->bodyRead += count;
            return count;
        }
        if (count == 0) return -1;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return -1;
        if (millis() - start >= 4000) return RECEIVE_TIMEOUT;
        vTaskDelay(1);
    }
    return -1;
}

inline bool parseRequest(WebRequest *request) {
    String received;
    received.reserve(1024);
    int headerEnd = -1;
    uint32_t started = millis();
    while (request->client.connected() && millis() - started < 5000) {
        char buffer[256];
        int count = recv(request->client.fd(), buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) {
            received.concat(buffer, count);
            headerEnd = received.indexOf("\r\n\r\n");
            if (headerEnd >= 0) break;
            if (received.length() > 4096) return false;
        } else if (count == 0) {
            return false;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
        vTaskDelay(1);
    }
    if (headerEnd < 0 || headerEnd > 4092) return false;
    const uint8_t *receivedBytes = reinterpret_cast<const uint8_t *>(received.c_str());
    request->bodyPrefix.assign(receivedBytes + headerEnd + 4, receivedBytes + received.length());
    http_parser parser;
    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = request;
    http_parser_settings settings = {};
    settings.on_url = [](http_parser *parser, const char *data, size_t length) {
        auto *request = static_cast<WebRequest *>(parser->data);
        request->uri.concat(data, length);
        return request->uri.length() > 512 ? -1 : 0;
    };
    settings.on_header_field = [](http_parser *parser, const char *data, size_t length) {
        auto *request = static_cast<WebRequest *>(parser->data);
        if (request->parserReadingValue && !request->flushParsedHeader()) return -1;
        request->parserReadingValue = false;
        request->parserField.concat(data, length);
        return request->parserField.length() > 64 ? -1 : 0;
    };
    settings.on_header_value = [](http_parser *parser, const char *data, size_t length) {
        auto *request = static_cast<WebRequest *>(parser->data);
        request->parserReadingValue = true;
        request->parserValue.concat(data, length);
        return request->parserValue.length() > 2048 ? -1 : 0;
    };
    settings.on_headers_complete = [](http_parser *parser) {
        auto *request = static_cast<WebRequest *>(parser->data);
        if (!request->flushParsedHeader()) return -1;
        request->method = static_cast<http_method>(parser->method);
        request->http11 = parser->http_major == 1 && parser->http_minor == 1;
        if (parser->http_major != 1 || parser->http_minor > 1) return -1;
        request->incomingChunked = (parser->flags & F_CHUNKED) != 0;
        uint64_t length = parser->content_length;
        request->content_len = length == UINT64_MAX ? 0 : size_t(length);
        if (length != UINT64_MAX && length > 2 * 1024 * 1024) return -1;
        return 1; // Apenas cabeçalhos; o worker lê o corpo por demanda.
    };
    size_t consumed = http_parser_execute(&parser, &settings, received.c_str(), headerEnd + 4);
    if (HTTP_PARSER_ERRNO(&parser) != HPE_OK || consumed != size_t(headerEnd + 4)) return false;
    if (request->uri.isEmpty() || request->uri[0] != '/') return false;
    if (request->incomingChunked) return false;
    if (request->header("Expect").equalsIgnoreCase("100-continue")) {
        if (socketWrite(request, "HTTP/1.1 100 Continue\r\n\r\n", 25) < 0) return false;
    }
    return true;
}
