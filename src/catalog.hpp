#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace orbisshelf {

struct CatalogItem {
    std::string id;
    std::string name;
    std::string title_id;
    std::string type;
    std::string version;
    std::string pkg_url;
    std::string sha256;
    uint64_t size_bytes;
    bool enabled;
    std::string notes;
};

bool parse_catalog(const std::string& json, std::vector<CatalogItem>& items, std::string& error);
bool load_text_file(const char* path, std::string& content, std::string& error);

} // namespace orbisshelf
