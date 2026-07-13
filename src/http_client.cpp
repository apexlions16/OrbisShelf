#include "http_client.hpp"

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace orbisshelf {
namespace {

const char* kUserAgent = "OrbisShelf/0.1 (PlayStation 4 Homebrew)";
const size_t kNetPoolSize = 64 * 1024;
const size_t kSslPoolSize = 512 * 1024;
const size_t kHttpPoolSize = 512 * 1024;
const int kMaxRedirects = 6;

struct RequestHandles {
    int tmpl;
    int conn;
    int req;
    RequestHandles() : tmpl(-1), conn(-1), req(-1) {}
};

void close_request(RequestHandles& handles) {
    if (handles.req >= 0) sceHttpDeleteRequest(handles.req);
    if (handles.conn >= 0) sceHttpDeleteConnection(handles.conn);
    if (handles.tmpl >= 0) sceHttpDeleteTemplate(handles.tmpl);
    handles.req = handles.conn = handles.tmpl = -1;
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string lower(const std::string& value) {
    std::string result(value);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
    return result;
}

bool header_value(const char* headers, size_t header_size, const char* name, std::string& value) {
    if (!headers || !header_size) return false;
    const std::string block(headers, header_size);
    std::istringstream stream(block);
    std::string line;
    const std::string wanted = lower(name);
    while (std::getline(stream, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower(trim(line.substr(0, colon))) == wanted) {
            value = trim(line.substr(colon + 1));
            return true;
        }
    }
    return false;
}

std::string origin_of(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    const size_t slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? url : url.substr(0, slash);
}

std::string resolve_redirect(const std::string& current, const std::string& location) {
    if (location.compare(0, 8, "https://") == 0 || location.compare(0, 7, "http://") == 0) return location;
    if (!location.empty() && location[0] == '/') return origin_of(current) + location;
    const size_t slash = current.rfind('/');
    return slash == std::string::npos ? location : current.substr(0, slash + 1) + location;
}

bool open_get(int http_context, const std::string& url, const std::string& bearer_token,
              RequestHandles& handles, int32_t& status, std::string& redirect, std::string& error) {
    handles.tmpl = sceHttpCreateTemplate(http_context, kUserAgent, ORBIS_HTTP_VERSION_1_1, 1);
    if (handles.tmpl < 0) { error = "sceHttpCreateTemplate failed"; return false; }
    handles.conn = sceHttpCreateConnectionWithURL(handles.tmpl, url.c_str(), true);
    if (handles.conn < 0) { error = "sceHttpCreateConnectionWithURL failed"; return false; }
    handles.req = sceHttpCreateRequestWithURL(handles.conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (handles.req < 0) { error = "sceHttpCreateRequestWithURL failed"; return false; }

    if (!bearer_token.empty()) {
        const std::string auth = "Bearer " + bearer_token;
        const int header_result = sceHttpAddRequestHeader(handles.req, "Authorization", auth.c_str(), 1);
        if (header_result < 0) { error = "failed to add Authorization header"; return false; }
    }
    const int send_result = sceHttpSendRequest(handles.req, 0, 0);
    if (send_result < 0) { error = "sceHttpSendRequest failed"; return false; }
    if (sceHttpGetStatusCode(handles.req, &status) < 0) { error = "sceHttpGetStatusCode failed"; return false; }

    if (status >= 300 && status < 400) {
        char* headers = 0;
        size_t header_size = 0;
        if (sceHttpGetAllResponseHeaders(handles.req, &headers, &header_size) < 0 ||
            !header_value(headers, header_size, "location", redirect)) {
            error = "redirect response did not contain Location";
            return false;
        }
    }
    return true;
}

bool open_final_get(int http_context, const std::string& initial_url, const std::string& bearer_token,
                    RequestHandles& handles, int32_t& status, std::string& final_url, std::string& error) {
    std::string url = initial_url;
    for (int redirects = 0; redirects <= kMaxRedirects; ++redirects) {
        std::string location;
        const std::string scoped_token = origin_of(url) == origin_of(initial_url) ? bearer_token : std::string();
        if (!open_get(http_context, url, scoped_token, handles, status, location, error)) {
            close_request(handles);
            return false;
        }
        if (status < 300 || status >= 400) {
            final_url = url;
            return true;
        }
        close_request(handles);
        url = resolve_redirect(url, location);
        if (url.compare(0, 8, "https://") != 0) {
            error = "refused non-HTTPS redirect";
            return false;
        }
    }
    error = "too many HTTP redirects";
    return false;
}

std::string status_error(int32_t status) {
    std::ostringstream out;
    out << "HTTP request failed with status " << status;
    return out.str();
}

} // namespace

HttpClient::HttpClient() : net_pool_(-1), ssl_context_(-1), http_context_(-1) {}
HttpClient::~HttpClient() { shutdown(); }

bool HttpClient::initialize(std::string& error) {
    if (http_context_ >= 0) return true;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0 ||
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0) {
        error = "failed to load network system modules";
        return false;
    }
    sceNetInit();
    net_pool_ = sceNetPoolCreate("OrbisShelfNet", kNetPoolSize, 0);
    if (net_pool_ < 0) { error = "sceNetPoolCreate failed"; shutdown(); return false; }
    ssl_context_ = sceSslInit(kSslPoolSize);
    if (ssl_context_ < 0) { error = "sceSslInit failed"; shutdown(); return false; }
    http_context_ = sceHttpInit(net_pool_, ssl_context_, kHttpPoolSize);
    if (http_context_ < 0) { error = "sceHttpInit failed"; shutdown(); return false; }
    return true;
}

void HttpClient::shutdown() {
    if (http_context_ >= 0) sceHttpTerm(http_context_);
    if (ssl_context_ >= 0) sceSslTerm(ssl_context_);
    if (net_pool_ >= 0) sceNetPoolDestroy(net_pool_);
    http_context_ = ssl_context_ = net_pool_ = -1;
}

bool HttpClient::get_text(const std::string& url, size_t max_bytes, std::string& output,
                          std::string& error, const std::string& bearer_token) {
    RequestHandles handles;
    int32_t status = 0;
    std::string final_url;
    if (!open_final_get(http_context_, url, bearer_token, handles, status, final_url, error)) return false;
    if (status != 200) { error = status_error(status); close_request(handles); return false; }

    output.clear();
    std::vector<char> buffer(32 * 1024);
    for (;;) {
        const int count = sceHttpReadData(handles.req, &buffer[0], static_cast<uint32_t>(buffer.size()));
        if (count < 0) { error = "sceHttpReadData failed"; close_request(handles); return false; }
        if (count == 0) break;
        if (output.size() + static_cast<size_t>(count) > max_bytes) {
            error = "HTTP response exceeded size limit";
            close_request(handles);
            return false;
        }
        output.append(&buffer[0], static_cast<size_t>(count));
    }
    close_request(handles);
    return true;
}

bool HttpClient::download(const std::string& url, const char* destination, ProgressCallback callback,
                          void* user, uint64_t& downloaded_bytes, std::string& error,
                          const std::string& bearer_token) {
    RequestHandles handles;
    int32_t status = 0;
    std::string final_url;
    if (!open_final_get(http_context_, url, bearer_token, handles, status, final_url, error)) return false;
    if (status != 200 && status != 206) { error = status_error(status); close_request(handles); return false; }

    int content_length_type = 0;
    size_t content_length = 0;
    if (sceHttpGetResponseContentLength(handles.req, &content_length_type, &content_length) < 0)
        content_length = 0;

    FILE* file = fopen(destination, "wb");
    if (!file) { error = "cannot create destination file"; close_request(handles); return false; }

    downloaded_bytes = 0;
    std::vector<uint8_t> buffer(256 * 1024);
    bool ok = true;
    for (;;) {
        const int count = sceHttpReadData(handles.req, &buffer[0], static_cast<uint32_t>(buffer.size()));
        if (count < 0) { error = "sceHttpReadData failed"; ok = false; break; }
        if (count == 0) break;
        const size_t written = fwrite(&buffer[0], 1, static_cast<size_t>(count), file);
        if (written != static_cast<size_t>(count)) { error = "failed to write downloaded PKG"; ok = false; break; }
        downloaded_bytes += written;
        if (callback) callback(downloaded_bytes, static_cast<uint64_t>(content_length), user);
    }
    if (fclose(file) != 0 && ok) { error = "failed to flush downloaded PKG"; ok = false; }
    close_request(handles);
    if (!ok) remove(destination);
    return ok;
}

} // namespace orbisshelf
