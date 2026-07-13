#include "pkg_installer.hpp"

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

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
    : bgft_heap_(0), bgft_initialized_(false), app_inst_initialized_(false),
      app_inst_module_loaded_(false), user_id_(0) {}

PackageInstaller::~PackageInstaller() { shutdown(); }

bool PackageInstaller::initialize_app_inst_util(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (app_inst_initialized_) return true;

    if (!app_inst_module_loaded_) {
        const uint32_t module_result =
            sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL);
        if (module_result != 0) {
            error_code = static_cast<int32_t>(module_result);
            error = "APPINST MODULE LOAD";
            return false;
        }
        app_inst_module_loaded_ = true;
    }

    const int32_t result = sceAppInstUtilInitialize();
    if (result != 0) {
        error_code = result;
        error = "APPINST INITIALIZE";
        return false;
    }
    app_inst_initialized_ = true;
    return true;
}

bool PackageInstaller::initialize(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (bgft_initialized_ && app_inst_initialized_) return true;
    if (!initialize_app_inst_util(error, error_code)) return false;

    sceUserServiceInitialize(0);
    const int32_t user_result = sceUserServiceGetInitialUser(&user_id_);
    if (user_result < 0) user_id_ = 0;

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

    const int32_t result = sceBgftServiceIntInit(&init_params);
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

bool PackageInstaller::install_local(const char* pkg_path, std::string& title_id,
                                     std::string& error, int32_t& error_code) {
    error_code = 0;
    title_id.clear();
    if (!initialize_app_inst_util(error, error_code)) return false;

    char pkg_title_id[16];
    std::memset(pkg_title_id, 0, sizeof(pkg_title_id));
    int32_t is_app = 0;
    int32_t result = sceAppInstUtilGetTitleIdFromPkg(pkg_path, pkg_title_id, &is_app);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilGetTitleIdFromPkg", result);
        return false;
    }

    int32_t already_exists = 0;
    result = sceAppInstUtilAppExists(pkg_title_id, &already_exists);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilAppExists", result);
        return false;
    }
    if (already_exists != 0) {
        error = "title is already installed; automatic overwrite is disabled";
        return false;
    }

    result = sceAppInstUtilAppInstallPkg(pkg_path, 0);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilAppInstallPkg", result);
        return false;
    }

    title_id = pkg_title_id;
    return true;
}

bool PackageInstaller::query_installation(const std::string& title_id, bool& exists, bool& updating,
                                          std::string& error, int32_t& error_code) {
    error_code = 0;
    exists = false;
    updating = true;
    if (!initialize_app_inst_util(error, error_code)) return false;

    int32_t exists_value = 0;
    int32_t result = sceAppInstUtilAppExists(title_id.c_str(), &exists_value);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilAppExists", result);
        return false;
    }
    exists = exists_value != 0;

    int32_t updating_value = 0;
    result = sceAppInstUtilAppIsInUpdating(title_id.c_str(), &updating_value);
    if (result == 0) {
        updating = updating_value != 0;
    } else {
        updating = !exists;
    }
    return true;
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
