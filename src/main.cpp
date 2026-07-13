#include "catalog.hpp"
#include "http_client.hpp"
#include "pixel_font.hpp"
#include "pkg_installer.hpp"
#include "sha256.hpp"

#include <SDL2/SDL.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

using orbisshelf::CatalogItem;

const int kWidth = 1920;
const int kHeight = 1080;
const char* kCatalogUrl = "https://raw.githubusercontent.com/apexlions16/OrbisShelf/main/catalog/catalog.json";
const char* kBundledCatalog = "/app0/catalog/catalog.json";
const char* kCachedCatalog = "/data/OrbisShelf/catalog.json";
const char* kTokenPath = "/data/OrbisShelf/hf_token.txt";
const char* kDownloadDirectory = "/data/OrbisShelf/downloads";

struct SharedState {
    pthread_mutex_t mutex;
    std::vector<CatalogItem> items;
    bool job_running;
    std::string status;
    uint64_t current;
    uint64_t total;
    int32_t last_error_code;

    SharedState() : job_running(false), status("STARTING"), current(0), total(0), last_error_code(0) {
        pthread_mutex_init(&mutex, 0);
    }
    ~SharedState() { pthread_mutex_destroy(&mutex); }
};

enum JobType { JobRefresh, JobInstall };

struct JobArgs {
    SharedState* state;
    JobType type;
    CatalogItem item;
};

void ensure_directories() {
    mkdir("/data/OrbisShelf", 0777);
    mkdir(kDownloadDirectory, 0777);
}

std::string trim(std::string value) {
    while (!value.empty() && (value[value.size()-1] == '\r' || value[value.size()-1] == '\n' || value[value.size()-1] == ' ' || value[value.size()-1] == '\t')) value.resize(value.size()-1);
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' || value[begin] == '\r' || value[begin] == '\n')) ++begin;
    return value.substr(begin);
}

std::string optional_hf_token() {
    std::string value, error;
    if (!orbisshelf::load_text_file(kTokenPath, value, error)) return std::string();
    return trim(value);
}

void set_status(SharedState* state, const std::string& status, bool running, int32_t error_code = 0) {
    pthread_mutex_lock(&state->mutex);
    state->status = status;
    state->job_running = running;
    state->last_error_code = error_code;
    if (!running) { state->current = 0; state->total = 0; }
    pthread_mutex_unlock(&state->mutex);
}

void progress_callback(uint64_t current, uint64_t total, void* user) {
    SharedState* state = static_cast<SharedState*>(user);
    pthread_mutex_lock(&state->mutex);
    state->current = current;
    state->total = total;
    pthread_mutex_unlock(&state->mutex);
}

bool save_text(const char* path, const std::string& value) {
    std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    return file.good();
}

std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    std::ostringstream out;
    if (unit == 0) out << static_cast<uint64_t>(value);
    else { out.setf(std::ios::fixed); out.precision(1); out << value; }
    out << ' ' << units[unit];
    return out.str();
}

void* job_main(void* raw) {
    JobArgs* args = static_cast<JobArgs*>(raw);
    SharedState* state = args->state;
    const JobType type = args->type;
    const CatalogItem item = args->item;
    delete args;

    orbisshelf::HttpClient http;
    std::string error;
    if (!http.initialize(error)) {
        set_status(state, "NETWORK INIT FAILED: " + error, false);
        return 0;
    }

    if (type == JobRefresh) {
        set_status(state, "REFRESHING CATALOG", true);
        std::string json;
        if (!http.get_text(kCatalogUrl, 2 * 1024 * 1024, json, error)) {
            set_status(state, "CATALOG REFRESH FAILED: " + error, false);
            return 0;
        }
        std::vector<CatalogItem> parsed;
        if (!orbisshelf::parse_catalog(json, parsed, error)) {
            set_status(state, "CATALOG INVALID: " + error, false);
            return 0;
        }
        save_text(kCachedCatalog, json);
        pthread_mutex_lock(&state->mutex);
        state->items.swap(parsed);
        state->status = "CATALOG UPDATED";
        state->job_running = false;
        state->current = state->total = 0;
        pthread_mutex_unlock(&state->mutex);
        return 0;
    }

    set_status(state, "DOWNLOADING " + item.name, true);
    const std::string pkg_path = std::string(kDownloadDirectory) + "/" + item.id + "-" + item.version + ".pkg";
    uint64_t downloaded = 0;
    const std::string token = optional_hf_token();
    if (!http.download(item.pkg_url, pkg_path.c_str(), progress_callback, state, downloaded, error, token)) {
        set_status(state, "DOWNLOAD FAILED: " + error, false);
        return 0;
    }
    if (item.size_bytes && downloaded != item.size_bytes) {
        remove(pkg_path.c_str());
        set_status(state, "SIZE CHECK FAILED", false);
        return 0;
    }
    if (!item.sha256.empty()) {
        set_status(state, "VERIFYING SHA256", true);
        std::string digest;
        if (!orbisshelf::sha256_file(pkg_path.c_str(), digest, error) || digest != item.sha256) {
            remove(pkg_path.c_str());
            set_status(state, "SHA256 CHECK FAILED", false);
            return 0;
        }
    }

    set_status(state, "REGISTERING INSTALL TASK", true);
    orbisshelf::PackageInstaller installer;
    int32_t error_code = 0;
    int32_t task_id = -1;
    if (!installer.enqueue(pkg_path.c_str(), item.name, task_id, error, error_code)) {
        set_status(state, "INSTALL START FAILED: " + error, false, error_code);
        return 0;
    }

    set_status(state, "INSTALLING " + item.name, true);
    for (;;) {
        uint64_t transferred = 0, total = 0;
        int32_t install_error = 0;
        if (!installer.progress(task_id, transferred, total, install_error, error)) {
            set_status(state, "INSTALL STATUS FAILED: " + error, false);
            return 0;
        }
        progress_callback(transferred, total, state);
        if (install_error != 0) {
            set_status(state, "INSTALL FAILED", false, install_error);
            return 0;
        }
        if (total > 0 && transferred >= total) break;
        sceKernelUsleep(500000);
    }
    remove(pkg_path.c_str());
    set_status(state, "INSTALL COMPLETE: " + item.name, false);
    return 0;
}

bool start_job(SharedState& state, JobType type, const CatalogItem* item) {
    pthread_mutex_lock(&state.mutex);
    if (state.job_running) { pthread_mutex_unlock(&state.mutex); return false; }
    state.job_running = true;
    state.current = state.total = 0;
    state.status = type == JobRefresh ? "STARTING CATALOG REFRESH" : "STARTING DOWNLOAD";
    pthread_mutex_unlock(&state.mutex);

    JobArgs* args = new JobArgs;
    args->state = &state;
    args->type = type;
    if (item) args->item = *item;
    pthread_t thread;
    if (pthread_create(&thread, 0, job_main, args) != 0) {
        delete args;
        set_status(&state, "COULD NOT CREATE WORKER THREAD", false);
        return false;
    }
    pthread_detach(thread);
    return true;
}

void load_initial_catalog(SharedState& state) {
    std::string json, error;
    if (!orbisshelf::load_text_file(kCachedCatalog, json, error))
        orbisshelf::load_text_file(kBundledCatalog, json, error);
    std::vector<CatalogItem> parsed;
    if (!json.empty() && orbisshelf::parse_catalog(json, parsed, error)) {
        state.items.swap(parsed);
        state.status = "LOCAL CATALOG LOADED";
    } else {
        state.status = "NO VALID LOCAL CATALOG";
    }
}

void fill(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

std::string truncate_text(const std::string& value, size_t max_chars) {
    if (value.size() <= max_chars) return value;
    if (max_chars < 4) return value.substr(0, max_chars);
    return value.substr(0, max_chars - 3) + "...";
}

void render(SDL_Renderer* renderer, SharedState& state, int selected) {
    const SDL_Color background = {13, 18, 28, 255};
    const SDL_Color panel = {24, 32, 46, 255};
    const SDL_Color selected_color = {45, 112, 196, 255};
    const SDL_Color white = {240, 244, 250, 255};
    const SDL_Color muted = {150, 164, 184, 255};
    const SDL_Color accent = {82, 193, 170, 255};
    fill(renderer, 0, 0, kWidth, kHeight, background);
    fill(renderer, 0, 0, kWidth, 120, panel);
    orbisshelf::draw_text(renderer, 70, 38, 7, "ORBISSHELF", white);
    orbisshelf::draw_text(renderer, 1510, 48, 3, "X INSTALL   TRIANGLE REFRESH", muted);

    std::vector<CatalogItem> items;
    std::string status;
    uint64_t current = 0, total = 0;
    int32_t error_code = 0;
    pthread_mutex_lock(&state.mutex);
    items = state.items;
    status = state.status;
    current = state.current;
    total = state.total;
    error_code = state.last_error_code;
    pthread_mutex_unlock(&state.mutex);

    if (items.empty()) {
        orbisshelf::draw_text(renderer, 120, 260, 5, "NO ENABLED PACKAGES IN CATALOG", white);
        orbisshelf::draw_text(renderer, 120, 330, 3, "ADD AN ITEM TO CATALOG/CATALOG.JSON AND PRESS TRIANGLE", muted);
    } else {
        const int visible = 10;
        int first = std::max(0, selected - visible + 1);
        if (selected < first) first = selected;
        for (int row = 0; row < visible && first + row < static_cast<int>(items.size()); ++row) {
            const int index = first + row;
            const int y = 150 + row * 78;
            fill(renderer, 70, y, 1780, 66, index == selected ? selected_color : panel);
            orbisshelf::draw_text(renderer, 100, y + 14, 4, truncate_text(items[index].name, 48), white);
            std::string meta = items[index].type + "  V" + items[index].version;
            if (items[index].size_bytes) meta += "  " + human_bytes(items[index].size_bytes);
            orbisshelf::draw_text(renderer, 1320, y + 20, 3, meta, index == selected ? white : muted);
        }
    }

    fill(renderer, 0, 940, kWidth, 140, panel);
    orbisshelf::draw_text(renderer, 70, 968, 3, truncate_text(status, 95), white);
    if (error_code) {
        std::ostringstream code;
        code << "ERROR 0X";
        code.setf(std::ios::hex, std::ios::basefield);
        code.width(8); code.fill('0'); code << static_cast<uint32_t>(error_code);
        orbisshelf::draw_text(renderer, 1510, 968, 3, code.str(), white);
    }
    if (current || total) {
        const int bar_x = 70, bar_y = 1025, bar_w = 1780, bar_h = 20;
        fill(renderer, bar_x, bar_y, bar_w, bar_h, background);
        int filled = 0;
        if (total) filled = static_cast<int>((static_cast<double>(current) / static_cast<double>(total)) * bar_w);
        fill(renderer, bar_x, bar_y, std::max(4, std::min(bar_w, filled)), bar_h, accent);
        std::string progress = human_bytes(current);
        if (total) progress += " / " + human_bytes(total);
        orbisshelf::draw_text(renderer, 1430, 995, 3, progress, muted);
    }
}

} // namespace

int main(int, char**) {
    setvbuf(stdout, 0, _IONBF, 0);
    ensure_directories();
    SharedState state;
    load_initial_catalog(state);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) return 1;
    SDL_Window* window = SDL_CreateWindow("OrbisShelf", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight, 0);
    if (!window) return 2;
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    if (!renderer) return 3;
    SDL_Joystick* joystick = SDL_NumJoysticks() > 0 ? SDL_JoystickOpen(0) : 0;

    start_job(state, JobRefresh, 0);
    int selected = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            bool up = false, down = false, choose = false, refresh = false, quit = false;
            if (event.type == SDL_QUIT) quit = true;
            if (event.type == SDL_KEYDOWN) {
                up = event.key.keysym.sym == SDLK_UP;
                down = event.key.keysym.sym == SDLK_DOWN;
                choose = event.key.keysym.sym == SDLK_RETURN;
                refresh = event.key.keysym.sym == SDLK_r;
                quit = event.key.keysym.sym == SDLK_ESCAPE;
            } else if (event.type == SDL_JOYHATMOTION) {
                up = (event.jhat.value & SDL_HAT_UP) != 0;
                down = (event.jhat.value & SDL_HAT_DOWN) != 0;
            } else if (event.type == SDL_JOYBUTTONDOWN) {
                choose = event.jbutton.button == 1;
                quit = event.jbutton.button == 2;
                refresh = event.jbutton.button == 3;
            }

            pthread_mutex_lock(&state.mutex);
            const int count = static_cast<int>(state.items.size());
            const bool busy = state.job_running;
            CatalogItem chosen;
            if (count && selected >= 0 && selected < count) chosen = state.items[selected];
            pthread_mutex_unlock(&state.mutex);

            if (up && count) selected = (selected + count - 1) % count;
            if (down && count) selected = (selected + 1) % count;
            if (refresh && !busy) start_job(state, JobRefresh, 0);
            if (choose && count && !busy) start_job(state, JobInstall, &chosen);
            if (quit && !busy) running = false;
        }

        pthread_mutex_lock(&state.mutex);
        const int count = static_cast<int>(state.items.size());
        pthread_mutex_unlock(&state.mutex);
        if (count == 0) selected = 0;
        else if (selected >= count) selected = count - 1;

        render(renderer, state, selected);
        SDL_UpdateWindowSurface(window);
        SDL_Delay(16);
    }

    if (joystick) SDL_JoystickClose(joystick);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
