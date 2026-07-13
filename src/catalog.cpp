#include "catalog.hpp"
#include "mini_json.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

namespace orbisshelf {
namespace {

bool read_string(const Json& object, const char* key, bool required, std::string& out, std::string& error) {
    const Json* value = object.get(key);
    if (!value) {
        if (required) error = std::string("missing string field: ") + key;
        return !required;
    }
    if (!value->is_string()) {
        error = std::string("field must be a string: ") + key;
        return false;
    }
    out = value->as_string();
    return true;
}

bool read_bool(const Json& object, const char* key, bool& out, std::string& error) {
    const Json* value = object.get(key);
    if (!value || !value->is_bool()) {
        error = std::string("field must be a boolean: ") + key;
        return false;
    }
    out = value->as_bool();
    return true;
}

bool valid_type(const std::string& value) {
    return value == "app" || value == "game" || value == "update" || value == "dlc";
}

} // namespace

bool parse_catalog(const std::string& json, std::vector<CatalogItem>& items, std::string& error) {
    try {
        const Json root = parse_json(json);
        if (!root.is_object()) { error = "catalog root must be an object"; return false; }
        const Json* schema = root.get("schema_version");
        if (!schema || !schema->is_number() || static_cast<int>(schema->as_number()) != 1) {
            error = "unsupported schema_version";
            return false;
        }
        const Json* list = root.get("items");
        if (!list || !list->is_array()) { error = "items must be an array"; return false; }

        std::vector<CatalogItem> parsed;
        const std::vector<Json>& values = list->as_array();
        for (size_t i = 0; i < values.size(); ++i) {
            const Json& value = values[i];
            if (!value.is_object()) { error = "catalog item must be an object"; return false; }
            CatalogItem item;
            item.size_bytes = 0;
            item.enabled = false;
            if (!read_string(value, "id", true, item.id, error) ||
                !read_string(value, "name", true, item.name, error) ||
                !read_string(value, "title_id", false, item.title_id, error) ||
                !read_string(value, "type", true, item.type, error) ||
                !read_string(value, "version", true, item.version, error) ||
                !read_string(value, "pkg_url", true, item.pkg_url, error) ||
                !read_string(value, "sha256", false, item.sha256, error) ||
                !read_string(value, "notes", false, item.notes, error) ||
                !read_bool(value, "enabled", item.enabled, error)) return false;

            const Json* size = value.get("size_bytes");
            if (size) {
                if (!size->is_number() || size->as_number() < 0.0 || std::floor(size->as_number()) != size->as_number()) {
                    error = "size_bytes must be a non-negative integer";
                    return false;
                }
                item.size_bytes = static_cast<uint64_t>(size->as_number());
            }
            if (item.id.empty() || item.name.empty() || item.pkg_url.compare(0, 8, "https://") != 0) {
                error = "item id/name/pkg_url is invalid";
                return false;
            }
            if (!valid_type(item.type)) { error = "unsupported package type: " + item.type; return false; }
            if (item.sha256.size() != 0 && item.sha256.size() != 64) { error = "sha256 must contain 64 hexadecimal characters"; return false; }
            if (item.enabled) parsed.push_back(item);
        }
        items.swap(parsed);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool load_text_file(const char* path, std::string& content, std::string& error) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) { error = std::string("cannot open ") + path; return false; }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) { error = std::string("cannot read ") + path; return false; }
    content = buffer.str();
    return true;
}

} // namespace orbisshelf
