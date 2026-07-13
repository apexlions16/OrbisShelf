#include "pkg_installer.hpp"

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/UserService.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace orbisshelf {
namespace {
const size_t kBgftHeapSize = 1024 * 1024;
const char* kInstallStagePath = "/data/OrbisShelf/install_stage.txt";

std::string code_message(const char* operation, int32_t code) {
    std::ostringstream out;
    out << operation << " failed (0x";
    out.setf(std::ios::hex, std::ios::basefield);
    out.width(8);
    out.fill('0');
    out << static_cast<uint32_t>(code) << ")";
    return out.str();
}

void mark_stage(const char* stage, int32_t code = 0) {
    FILE* file = std::fopen(kInstallStagePath, "wb");
    if (!file) return;
    if (code == 0) {
        std::fprintf(file, "%s\n", stage);
    } else {
        std::fprintf(file, "%s: 0x%08x\n", stage, static_cast<uint32_t>(code));
    }
    std::fclose(file);
}

uint64_t choose_total(const OrbisBgftTaskProgress& value) {
    return value.lengthTotal ? value.lengthTotal : value.length;
}

uint64_t choose_transferred(const OrbisBgftTaskProgress& value) {
    return value.transferredTotal ? value.transferredTotal : value.transferred;
}

} // namespace

PackageInstaller::PackageInstaller()
    : bgft_initialized_(false), app_inst_initialized_(false), user_id_(0) {
    std::memset(&bgft_init_params_, 0, sizeof(bgft_init_params_));
}

PackageInstaller::~PackageInstaller() { shutdown(); }

bool PackageInstaller::initialize(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (bgft_initialized_ && app_inst_initialized_) return true;

    mark_stage("initializing user service");
    sceUserServiceInitialize(0);
    const int32_t user_result = sceUserServiceGetInitialUser(&user_id_);
    if (user_result < 0) user_id_ = 0;

    if (!app_inst_initialized_) {
        mark_stage("before sceAppInstUtilInitialize");
        const int32_t result = sceAppInstUtilInitialize();
        if (result != 0) {
            mark_stage("sceAppInstUtilInitialize failed", result);
            error_code = result;
            error = code_message("sceAppInstUtilInitialize", result);
            return false;
        }
        app_inst_initialized_ = true;
        mark_stage("sceAppInstUtilInitialize complete");
    }

    if (!bgft_initialized_) {
        bgft_init_params_.heapSize = kBgftHeapSize;
        bgft_init_params_.heap = std::malloc(bgft_init_params_.heapSize);
        if (!bgft_init_params_.heap) {
            error = "cannot allocate BGFT heap";
            mark_stage("BGFT heap allocation failed");
            return false;
        }
        std::memset(bgft_init_params_.heap, 0, bgft_init_params_.heapSize);

        // BGFT keeps this parameter block after initialization on some firmware.
        // It is therefore a persistent class member rather than a stack local.
        mark_stage("before sceBgftServiceIntInit");
        const int32_t result = sceBgftServiceIntInit(&bgft_init_params_);
        if (result != 0) {
            mark_stage("sceBgftServiceIntInit failed", result);
            error_code = result;
            error = code_message("sceBgftServiceIntInit", result);
            std::free(bgft_init_params_.heap);
            std::memset(&bgft_init_params_, 0, sizeof(bgft_init_params_));
            return false;
        }
        bgft_initialized_ = true;
        mark_stage("sceBgftServiceIntInit complete");
    }

    return true;
}

void PackageInstaller::shutdown() {
    if (bgft_initialized_) {
        sceBgftServiceIntTerm();
        bgft_initialized_ = false;
    }
    if (app_inst_initialized_) {
        sceAppInstUtilTerminate();
        app_inst_initialized_ = false;
    }
    if (bgft_init_params_.heap) std::free(bgft_init_params_.heap);
    std::memset(&bgft_init_params_, 0, sizeof(bgft_init_params_));
    user_id_ = 0;
}

bool PackageInstaller::enqueue(const char* pkg_path, const std::string& display_name, int32_t& task_id,
                               std::string& error, int32_t& error_code) {
    error_code = 0;
    if (!initialize(error, error_code)) return false;

    char title_id[16];
    std::memset(title_id, 0, sizeof(title_id));
    int32_t is_app = 0;
    mark_stage("before sceAppInstUtilGetTitleIdFromPkg");
    int32_t result = sceAppInstUtilGetTitleIdFromPkg(pkg_path, title_id, &is_app);
    if (result != 0) {
        mark_stage("sceAppInstUtilGetTitleIdFromPkg failed", result);
        error_code = result;
        error = code_message("sceAppInstUtilGetTitleIdFromPkg", result);
        return false;
    }
    mark_stage("PKG title id read");

    // Match the minimal local-storage registration pattern used by established
    // PS4 homebrew installers. Unused pointers stay null from memset.
    OrbisBgftDownloadParamEx params;
    std::memset(&params, 0, sizeof(params));
    params.params.userId = user_id_;
    params.params.entitlementType = 5;
    params.params.id = "";
    params.params.contentUrl = pkg_path;
    params.params.contentName = display_name.c_str();
    params.params.iconPath = "/app0/sce_sys/icon0.png";
    params.params.option = ORBIS_BGFT_TASK_OPT_INVISIBLE;
    params.params.playgoScenarioId = "0";
    params.slot = 0;

    task_id = -1;
    mark_stage("before BGFT register by storage");
    result = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&params, &task_id);
    if (result != 0) {
        mark_stage("BGFT register failed", result);
        error_code = result;
        error = code_message("sceBgftServiceIntDownloadRegisterTaskByStorageEx", result);
        return false;
    }

    // The non-Int start/progress pair is the path used by working local PKG
    // installers, while registration itself remains the ByStorageEx call.
    mark_stage("before BGFT start task");
    result = sceBgftServiceDownloadStartTask(task_id);
    if (result != 0) {
        sceBgftServiceIntDownloadUnregisterTask(task_id);
        mark_stage("BGFT start failed", result);
        error_code = result;
        error = code_message("sceBgftServiceDownloadStartTask", result);
        return false;
    }
    mark_stage("BGFT install task started");
    return true;
}

bool PackageInstaller::progress(int32_t task_id, uint64_t& transferred, uint64_t& total,
                                int32_t& install_error, std::string& error) {
    OrbisBgftTaskProgress value;
    std::memset(&value, 0, sizeof(value));
    const int32_t result = sceBgftServiceDownloadGetProgress(task_id, &value);
    if (result != 0) {
        error = code_message("sceBgftServiceDownloadGetProgress", result);
        return false;
    }

    install_error = value.errorResult;
    transferred = choose_transferred(value);
    total = choose_total(value);

    // Some firmware reports local PKG installation only as a percentage.
    if (total == 0) {
        int32_t percent = value.localCopyPercent;
        if (percent <= 0) percent = value.preparingPercent;
        percent = std::max<int32_t>(0, std::min<int32_t>(100, percent));
        transferred = static_cast<uint64_t>(percent);
        total = 100;
    }
    return true;
}

} // namespace orbisshelf
