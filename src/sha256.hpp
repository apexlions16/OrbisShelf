#pragma once

#include <string>

namespace orbisshelf {
bool sha256_file(const char* path, std::string& digest, std::string& error);
}
