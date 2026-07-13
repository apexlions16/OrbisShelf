#include "pkg_installer.hpp"

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace orbisshelf {
namespace {
const size_t kBgftHeapSize = 1024 * 1024;

// libSceAppInstUtil and libSceBgft are privileged system modules that are not
// auto-loaded for homebrew even when their stub libraries are linked. Under a
// HEN/GoldHEN environment they must be brought in with sceKernelLoadStartModule,
// NOT through sceSysmoduleLoadModuleInternal, which enforces an authority check
// that a homebrew process fails with ENOEXEC (0x80020008). SystemService must
// load before AppInstUtil so AppInstUtil's imports resolve; otherwise the load
// fails with ENOENT (0x80020002). This ordered set matches the known-working
// sequence in barisyild/airpsx.
struct RequiredModule {
    const char* name;
    const char* label;
};
const RequiredModule kRequiredModules[] = {
    {"libSceSystemService.sprx", "SYSTEM_SERVICE"},
    {"libSceAppInstUtil.sprx", "APP_INST_UTIL"},
    {"libSceSysUtil.sprx", "SYS_UTIL"},
    {"libSceBgft.sprx", "BGFT"},
};

// Loads one system module, trying the direct /system path first and falling
// back to the sandbox-resolved path. Depending on the HEN setup the app may see
// the common libraries under /system/common/lib or only under its per-boot
// sandbox mount (/<random_word>/common/lib). Returns the module handle (>= 0)
// on success or the last negative error code.
int32_t load_start_module(const char* name) {
    char path[256];

    std::snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    int32_t handle = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, 0, 0, 0, 0));
    if (handle >= 0) return handle;

    const char* sandbox_word = sceKernelGetFsSandboxRandomWord();
    if (sandbox_word && sandbox_word[0]) {
        std::snprintf(path, sizeof(path), "/%s/common/lib/%s", sandbox_word, name);
        const int32_t alt = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, 0, 0, 0, 0));
        if (alt >= 0) return alt;
        return alt;
    }

    return handle;
}

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
      modules_loaded_(false), user_id_(0) {}

PackageInstaller::~PackageInstaller() { shutdown(); }

bool PackageInstaller::load_system_modules(std::string& error, int32_t& error_code) {
    if (modules_loaded_) return true;

    for (size_t i = 0; i < sizeof(kRequiredModules) / sizeof(kRequiredModules[0]); ++i) {
        const int32_t handle = load_start_module(kRequiredModules[i].name);
        if (handle < 0) {
            error_code = handle;
            error = std::string("MODULE LOAD ") + kRequiredModules[i].label;
            return false;
        }
    }

    modules_loaded_ = true;
    return true;
}

bool PackageInstaller::initialize_app_inst_util(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (app_inst_initialized_) return true;
    if (!load_system_modules(error, error_code)) return false;

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
