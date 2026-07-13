#pragma once

#include <orbis/Bgft.h>
#include <stdint.h>
#include <string>

namespace orbisshelf {

class PackageInstaller {
public:
    PackageInstaller();
    ~PackageInstaller();

    bool initialize(std::string& error, int32_t& error_code);
    void shutdown();
    bool enqueue(const char* pkg_path, const std::string& display_name, int32_t& task_id,
                 std::string& error, int32_t& error_code);
    bool progress(int32_t task_id, uint64_t& transferred, uint64_t& total,
                  int32_t& install_error, std::string& error);

private:
    OrbisBgftInitParams bgft_init_params_;
    bool bgft_initialized_;
    bool app_inst_initialized_;
    int32_t user_id_;
};

} // namespace orbisshelf
