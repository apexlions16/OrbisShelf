#include "pkg_installer.hpp"

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/UserService.h>

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace orbisshelf {
namespace {
const size_t kBgftHeapSize = 1024 * 1024;

std::string code_message(const char* operation, int32_t code) {
    std::ostringstream out;
    out << operation << " failed (0x";
    out.setf(std::ios::hex, std::ios::basefield);
    out.width(8);
    out.fill('0');
    out << static_cast<uint32_t>(code) << ")";
    return out.str();
}
} // namespace

PackageInstaller::PackageInstaller()
    : bgft_heap_(0), bgft_initialized_(false), app_inst_initialized_(false), user_id_(0) {}

PackageInstaller::~PackageInstaller() { shutdown(); }

bool PackageInstaller::initialize(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (bgft_initialized_ && app_inst_initialized_) return true;

    sceUserServiceInitialize(0);
    const int32_t user_result = sceUserServiceGetInitialUser(&user_id_);
    if (user_result < 0) user_id_ = 0;

    int32_t result = sceAppInstUtilInitialize();
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilInitialize", result);
        return false;
    }
    app_inst_initialized_ = true;

    bgft_heap_ = std::malloc(kBgftHeapSize);
    if (!bgft_heap_) {
        error = "cannot allocate BGFT heap";
        shutdown();
        return false;
    }
    std::memset(bgft_heap_, 0, kBgftHeapSize);
    OrbisBgftInitParams init_params;
    std::memset(&init_params, 0, sizeof(init_params));
    init_params.heap = bgft_heap_;
    init_params.heapSize = kBgftHeapSize;

    result = sceBgftServiceIntInit(&init_params);
    if (result != 0) {
        error_code = result;
        error = code_message("sceBgftServiceIntInit", result);
        shutdown();
        return false;
    }
    bgft_initialized_ = true;
    return true;
}

void PackageInstaller::shutdown() {
    if (bgft_initialized_) sceBgftServiceIntTerm();
    if (app_inst_initialized_) sceAppInstUtilTerminate();
    if (bgft_heap_) std::free(bgft_heap_);
    bgft_heap_ = 0;
    bgft_initialized_ = false;
    app_inst_initialized_ = false;
}

bool PackageInstaller::enqueue(const char* pkg_path, const std::string& display_name, int32_t& task_id,
                               std::string& error, int32_t& error_code) {
    error_code = 0;
    if (!initialize(error, error_code)) return false;

    char title_id[16];
    std::memset(title_id, 0, sizeof(title_id));
    int32_t is_app = 0;
    int32_t result = sceAppInstUtilGetTitleIdFromPkg(pkg_path, title_id, &is_app);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilGetTitleIdFromPkg", result);
        return false;
    }

    OrbisBgftDownloadParamEx params;
    std::memset(&params, 0, sizeof(params));
    params.params.userId = user_id_;
    params.params.entitlementType = 5;
    params.params.id = "";
    params.params.contentUrl = pkg_path;
    params.params.contentExUrl = "";
    params.params.contentName = display_name.c_str();
    params.params.iconPath = "/app0/sce_sys/icon0.png";
    params.params.skuId = "";
    params.params.option = ORBIS_BGFT_TASK_OPT_INVISIBLE;
    params.params.playgoScenarioId = "0";
    params.params.releaseDate = "";
    params.params.packageType = "";
    params.params.packageSubType = "";
    params.params.packageSize = 0;
    params.slot = 0;

    task_id = -1;
    result = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&params, &task_id);
    if (result != 0) {
        error_code = result;
        error = code_message("sceBgftServiceIntDownloadRegisterTaskByStorageEx", result);
        return false;
    }
    result = sceBgftServiceIntDownloadStartTask(task_id);
    if (result != 0) {
        sceBgftServiceIntDownloadUnregisterTask(task_id);
        error_code = result;
        error = code_message("sceBgftServiceIntDownloadStartTask", result);
        return false;
    }
    return true;
}

bool PackageInstaller::progress(int32_t task_id, uint64_t& transferred, uint64_t& total,
                                int32_t& install_error, std::string& error) {
    OrbisBgftTaskProgress value;
    std::memset(&value, 0, sizeof(value));
    const int32_t result = sceBgftServiceIntDownloadGetProgress(task_id, &value);
    if (result != 0) {
        error = code_message("sceBgftServiceIntDownloadGetProgress", result);
        return false;
    }
    transferred = value.transferredTotal ? value.transferredTotal : value.transferred;
    total = value.lengthTotal ? value.lengthTotal : value.length;
    install_error = value.errorResult;
    return true;
}

} // namespace orbisshelf
