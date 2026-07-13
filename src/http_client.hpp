#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace orbisshelf {

typedef void (*ProgressCallback)(uint64_t downloaded, uint64_t total, void* user);

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    bool initialize(std::string& error);
    void shutdown();
    bool get_text(const std::string& url, size_t max_bytes, std::string& output, std::string& error,
                  const std::string& bearer_token = std::string());
    bool download(const std::string& url, const char* destination, ProgressCallback callback, void* user,
                  uint64_t& downloaded_bytes, std::string& error,
                  const std::string& bearer_token = std::string());

private:
    int net_pool_;
    int ssl_context_;
    int http_context_;
};

} // namespace orbisshelf
