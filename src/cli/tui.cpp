#include "cli/tui.hpp"

#include <curses.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include "cli/cli.hpp"
#include "cli/library.hpp"
#include "cli/workshop.hpp"
#include "core/fd.hpp"

extern char** environ;

namespace glint::cli {
namespace {
using nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr short normal = 1, accent = 2, muted = 3, highlighted = 4, error_color = 5, success = 6;

std::string clean(std::string text) {
    for (char& c : text)
        if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return text;
}

void backspace(std::string& value) {
    if (value.empty()) return;
    std::size_t at = value.size() - 1;
    while (at > 0 && (static_cast<unsigned char>(value[at]) & 0xc0) == 0x80) --at;
    value.erase(at);
}

void text_at(int y, int x, const std::string& text, short color = normal, int limit = -1) {
    if (y < 0 || y >= LINES || x < 0 || x >= COLS) return;
    int room = std::min(limit < 0 ? COLS - x - 1 : limit, COLS - x - 1);
    if (room <= 0) return;
    wattr_set(stdscr, A_NORMAL, color, nullptr);
    move(y, x);
    std::mbstate_t state{};
    const std::string safe = clean(text);
    const char* at = safe.data();
    std::size_t left = safe.size();
    int used = 0;
    while (left) {
        wchar_t wc = 0;
        std::size_t n = std::mbrtowc(&wc, at, left, &state);
        if (n == static_cast<std::size_t>(-1) || n == static_cast<std::size_t>(-2) || n == 0) {
            wc = L'?';
            n = 1;
            state = {};
        }
        int width = std::max(0, wcwidth(wc));
        if (used + width > room) break;
        addnwstr(&wc, 1);
        used += width;
        at += n;
        left -= n;
    }
}

class Screen {
public:
    Screen() {
        std::setlocale(LC_ALL, "");
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        define_key("\033[A", KEY_UP);
        define_key("\033[B", KEY_DOWN);
        define_key("\033[C", KEY_RIGHT);
        define_key("\033[D", KEY_LEFT);
        curs_set(0);
        set_escdelay(30);
        timeout(100);
        if (has_colors() && std::getenv("NO_COLOR") == nullptr) {
            start_color();
            use_default_colors();
            init_pair(normal, COLOR_WHITE, -1);
            init_pair(accent, COLORS >= 256 ? 147 : COLOR_MAGENTA, -1);
            init_pair(muted, COLORS >= 256 ? 245 : COLOR_CYAN, -1);
            init_pair(highlighted, COLOR_BLACK, COLORS >= 256 ? 147 : COLOR_MAGENTA);
            init_pair(error_color, COLORS >= 256 ? 210 : COLOR_RED, -1);
            init_pair(success, COLORS >= 256 ? 115 : COLOR_GREEN, -1);
        }
    }
    ~Screen() { endwin(); }
};

class Process {
public:
    ~Process() { stop(); }
    bool start(const std::vector<std::string>& args, bool capture = true) {
        stop();
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC) != 0) return false;
        UniqueFd read_fd(pipes[0]), write_fd(pipes[1]);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (capture)
            posix_spawn_file_actions_adddup2(&actions, write_fd.get(), STDOUT_FILENO);
        else
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        int error = posix_spawnp(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (error != 0) {
            pid_ = -1;
            return false;
        }
        fcntl(read_fd.get(), F_SETFL, O_NONBLOCK);
        fd_ = std::move(read_fd);
        started_ = Clock::now();
        return true;
    }
    bool poll() {
        if (pid_ < 0) return false;
        char bytes[8192];
        for (;;) {
            ssize_t count = ::read(fd_.get(), bytes, sizeof(bytes));
            if (count <= 0) break;
            if (data.size() + static_cast<std::size_t>(count) > 1024 * 1024) {
                stop();
                return true;
            }
            data.append(bytes, static_cast<std::size_t>(count));
        }
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            for (;;) {
                ssize_t count = ::read(fd_.get(), bytes, sizeof(bytes));
                if (count <= 0) break;
                if (data.size() + static_cast<std::size_t>(count) > 1024 * 1024) break;
                data.append(bytes, static_cast<std::size_t>(count));
            }
            pid_ = -1;
            fd_.reset();
            ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            return true;
        }
        if (Clock::now() - started_ > std::chrono::seconds(10)) {
            stop();
            return true;
        }
        return false;
    }
    void stop() {
        if (pid_ >= 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
        fd_.reset();
        data.clear();
        ok = false;
    }
    bool running() const { return pid_ >= 0; }
    std::string data;
    bool ok = false;

private:
    pid_t pid_ = -1;
    UniqueFd fd_;
    Clock::time_point started_{};
};

struct Option {
    std::string key, label, value;
    json property;
};

struct Modal {
    bool& flag;
    explicit Modal(bool& value) : flag(value) { flag = true; }
    ~Modal() { flag = false; }
};

bool use_kitty_images() {
    if (const char* mode = std::getenv("GLINT_TUI_IMAGES"))
        return std::string_view(mode) == "kitty";
    if (std::getenv("TMUX") || std::getenv("STY")) return false;
    const char* term = std::getenv("TERM");
    const char* program = std::getenv("TERM_PROGRAM");
    return std::getenv("KITTY_WINDOW_ID") || (program && std::string_view(program) == "ghostty") ||
           (term &&
            (std::string_view(term) == "xterm-kitty" || std::string_view(term) == "xterm-ghostty"));
}

std::string shown(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "on" : "off";
    return value.dump();
}

std::string encode(const std::string& value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

class App {
public:
    ~App() {
        cancellation_.request_stop();
        delete_kitty_image();
    }
    int run() {
        load_library();
        refresh_state();
        bool running = true;
        while (running) {
            tick();
            draw();
            int key = getch();
            if (key == ERR) continue;
            switch (key) {
                case 'q':
                case 27: running = false; break;
                case KEY_RESIZE: preview_key_.clear(); break;
                case '\t': tab_ = (tab_ + 1) % 4; break;
                case KEY_BTAB: tab_ = (tab_ + 3) % 4; break;
                case '1':
                case '2':
                case '3':
                case '4': tab_ = key - '1'; break;
                case KEY_UP:
                case 'k': move_selection(-1); break;
                case KEY_DOWN:
                case 'j': move_selection(1); break;
                case KEY_NPAGE: move_selection(10); break;
                case KEY_PPAGE: move_selection(-10); break;
                case '/':
                    if (tab_ == 0) search();
                    break;
                case 'o': choose_output(); break;
                case 'r':
                    load_library();
                    refresh_state();
                    break;
                case 'p':
                    if (tab_ != 3)
                        action({{"op", "pause"}, {"output", target_}, {"reason", "request"}});
                    break;
                case 'u':
                    if (tab_ != 3)
                        action({{"op", "resume"}, {"output", target_}, {"reason", "request"}});
                    break;
                case 'f':
                    if (tab_ == 0) open_file();
                    break;
                case 'e':
                    if (tab_ == 0) {
                        tab_ = 2;
                        refresh_state();
                    }
                    break;
                case '\n':
                case KEY_ENTER: activate(); break;
                default: break;
            }
        }
        return 0;
    }

private:
    json library_ = json::array(), outputs_ = json::array(), status_ = json::object(),
         statuses_ = json::object(), props_ = json::array();
    std::vector<std::size_t> filtered_;
    std::string query_, target_ = "*", message_ = "Loading displays…";
    int tab_ = 0;
    std::array<int, 4> selected_{};
    bool online_ = false, failed_ = false, task_is_refresh_ = false;
    std::future<json> task_;
    std::stop_source cancellation_;
    Process preview_, opener_;
    std::string preview_key_, pixels_;
    int preview_w_ = 0, preview_h_ = 0;
    int preview_x_ = 0, preview_y_ = 0, preview_cols_ = 0, preview_rows_ = 0;
    bool kitty_ = use_kitty_images(), kitty_visible_ = false, modal_ = false;
    std::map<int, short> pairs_;
    Clock::time_point library_checked_ = Clock::now();

    bool busy() const { return task_.valid(); }
    std::string settings_output() const { return target_; }
    static json snapshot(const std::string& output, std::stop_token stop) {
        json response = request_daemon({{"op", "outputs"}}, stop);
        if (!response.value("ok", false)) return response;
        json result = {{"ok", true}, {"outputs", response.value("result", json::array())}};
        const auto states = request_daemon({{"op", "status"}}, stop);
        if (!states.value("ok", false)) return states;
        result["statuses"] = states.value("result", json::object());
        std::string chosen = output;
        if (chosen == "*" || chosen.empty()) {
            for (const auto& o : result["outputs"]) {
                if (chosen == "*" || chosen.empty()) chosen = o.value("name", "");
                if (o.value("wallpaper", json::object()).contains("path")) {
                    chosen = o.value("name", "");
                    break;
                }
            }
        }
        if (!chosen.empty()) {
            result["status"] = result["statuses"].value(chosen, json::object());
            const auto global = request_daemon({{"op", "settings"}}, stop);
            if (global.value("ok", false) && global.contains("result") && global["result"].is_object()) {
                if (!result["status"].contains("options")) result["status"]["options"] = json::object();
                result["status"]["options"].update(global["result"]);
            }
            bool same_project = true;
            const auto path = result["status"].value("path", json());
            if (output == "*") {
                for (const auto& state : result["statuses"])
                    if (state.value("path", json()).is_string() && state["path"] != path)
                        same_project = false;
            }
            if (same_project) {
                auto p = request_daemon({{"op", "props"}, {"output", chosen}}, stop);
                result["props"] =
                    p.value("ok", false) ? p.value("result", json::array()) : json::array();
            }
        }
        return result;
    }
    void refresh_state() {
        if (busy()) return;
        task_is_refresh_ = true;
        task_ =
            std::async(std::launch::async, [target = target_, stop = cancellation_.get_token()] {
                return snapshot(target, stop);
            });
    }
    void action(const json& request) {
        if (busy()) {
            message_ = "Waiting for the previous request…";
            return;
        }
        task_is_refresh_ = false;
        message_ = request.value("op", "") == "set" ? "Loading wallpaper…" : "Saving…";
        failed_ = false;
        task_ = std::async(std::launch::async, [request, stop = cancellation_.get_token()] {
            return request_daemon(request, stop);
        });
    }
    void edit_setting(const Option& option, const json& value) {
        if (busy()) return;
        if (!option.property.is_object() && option.key != "speed" && option.key != "hwdec") {
            action({{"op", "settings"}, {"set", {{option.key, value}}}});
            return;
        }
        task_is_refresh_ = false;
        message_ = target_ == "*" ? "Saving on all displays…" : "Saving…";
        failed_ = false;
        task_ = std::async(std::launch::async, [target = target_, option, value,
                                                path = status_.value("path", json()),
                                                stop = cancellation_.get_token()] {
            auto response = request_daemon({{"op", "status"}}, stop);
            if (!response.value("ok", false)) return response;
            const auto states = response.value("result", json::object());
            json requests;
            if (option.property.is_object()) {
                requests = json::array();
                for (const auto& [name, state] : states.items()) {
                    if (target != "*" && target != name) continue;
                    if (!state.value("path", json()).is_string()) continue;
                    if (state["path"] != path)
                        return json{
                            {"ok", false},
                            {"error", "Wallpaper changed. Refresh before editing its settings."}};
                    requests.push_back(
                        {{"op", "props"}, {"output", name}, {"set", {{option.key, value}}}});
                }
            } else
                requests = tui_setting_requests(target, states, option.key, value);
            if (requests.empty())
                return json{{"ok", false},
                            {"error", "No running wallpapers on the selected displays"}};
            std::string failures;
            for (const auto& request : requests) {
                if (stop.stop_requested()) break;
                const auto result = request_daemon(request, stop);
                if (!result.value("ok", false)) {
                    if (!failures.empty()) failures += "; ";
                    failures += request.value("output", "") + ": " +
                                result.value("error", "Request failed");
                }
            }
            return failures.empty() ? json{{"ok", true}} : json{{"ok", false}, {"error", failures}};
        });
    }
    void tick() {
        if (opener_.poll() && !opener_.ok) {
            message_ = "Steam could not be opened. Check that Steam and xdg-utils are installed.";
            failed_ = true;
        }
        if (preview_.poll()) {
            if (preview_.ok &&
                preview_.data.size() == static_cast<std::size_t>(preview_w_ * preview_h_ * 3))
                pixels_ = preview_.data;
        }
        if (busy() && task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            json result;
            try {
                result = task_.get();
            } catch (const std::exception& e) {
                result = {{"ok", false}, {"error", e.what()}};
            }
            if (!result.value("ok", false)) {
                message_ = result.value("error", "Request failed");
                failed_ = true;
                if (task_is_refresh_) online_ = false;
            } else if (task_is_refresh_) {
                online_ = true;
                outputs_ = result.value("outputs", json::array());
                status_ = result.value("status", json::object());
                statuses_ = result.value("statuses", json::object());
                props_ = result.value("props", json::array());
                if (message_ == "Loading displays…" || failed_) message_ = "Ready";
                failed_ = false;
            } else {
                message_ = "Saved";
                failed_ = false;
                refresh_state();
            }
        }
        if (tab_ == 3 && Clock::now() - library_checked_ > std::chrono::seconds(5)) load_library();
    }
    void load_library() {
        std::string error;
        library_ = list_installed_library(workshop_path(), download_path(), "", error);
        if (!error.empty()) {
            message_ = error;
            failed_ = true;
        }
        library_checked_ = Clock::now();
        filter();
    }
    void filter() {
        filtered_.clear();
        std::string q = query_;
        std::ranges::transform(q, q.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (std::size_t i = 0; i < library_.size(); ++i) {
            const auto& item = library_[i];
            if (item.value("type", "") != "scene" && item.value("type", "") != "video") continue;
            std::string hay = item.value("title", "") + " " + item.value("id", "") + " " +
                              item.value("tags", json::array()).dump();
            std::ranges::transform(hay, hay.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (q.empty() || hay.find(q) != std::string::npos) filtered_.push_back(i);
        }
        selected_[0] =
            std::clamp(selected_[0], 0, std::max(0, static_cast<int>(filtered_.size()) - 1));
        preview_key_.clear();
    }
    const json* picked() const {
        if (filtered_.empty()) return nullptr;
        return &library_[filtered_[static_cast<std::size_t>(
            std::clamp(selected_[0], 0, static_cast<int>(filtered_.size()) - 1))]];
    }
    std::vector<Option> options() const {
        if (!status_.contains("path") || !status_["path"].is_string()) return {};
        const json values = status_.value("options", json::object());
        std::vector<Option> list = {
            {"fps",
             "Frame rate",
             values.value("fps", json(0)) == 0 ? "Display refresh rate"
                                               : shown(values["fps"]) + " FPS",
             {}},
            {"scaling", "Scaling", values.value("scaling", "fill"), {}},
            {"transition", "Transition", values.value("transition", "fade"), {}},
            {"duration",
             "Transition duration",
             shown(values.value("duration", json(300))) + " ms",
             {}},
            {"volume",
             "Sound volume",
             std::to_string(static_cast<int>(values.value("volume", 0.0) * 100)) + "%",
             {}},
            {"mute", "Mute", values.value("mute", false) ? "on" : "off", {}},
            {"color", "Background color", values.value("color", "#000000"), {}}};
        bool videos_only = status_.value("kind", "") == "video";
        if (target_ == "*")
            for (const auto& state : statuses_)
                if (state.value("path", json()).is_string() && state.value("kind", "") != "video")
                    videos_only = false;
        if (videos_only) {
            list.push_back(
                {"speed", "Playback speed", shown(values.value("speed", json(1))) + "×", {}});
            list.push_back({"hwdec", "Video decoding", values.value("hwdec", "auto"), {}});
        }
        for (const auto& p : props_) {
            const std::string type = p.value("type", "");
            if (!p.value("visible", true) || type == "text" || type == "group") continue;
            list.push_back({p.value("name", ""), p.value("text", p.value("name", "")),
                            shown(p.value("value", json())), p});
        }
        if (target_ == "*") {
            const json defaults = {
                {"fps", 0},           {"scaling", "fill"}, {"transition", "fade"},
                {"duration", 300},    {"volume", 0},       {"mute", false},
                {"color", "#000000"}, {"speed", 1},        {"hwdec", "auto"}};
            for (auto& option : list) {
                if (option.key != "speed" && option.key != "hwdec") continue;
                if (!option.property.is_null()) continue;
                const auto first = values.value(option.key, defaults.at(option.key));
                for (const auto& state : statuses_) {
                    if (!state.value("path", json()).is_string()) continue;
                    if (state.value("options", json::object())
                            .value(option.key, defaults.at(option.key)) != first) {
                        option.value = "Mixed";
                        break;
                    }
                }
            }
        }
        return list;
    }
    int count() const {
        if (tab_ == 0) return static_cast<int>(filtered_.size());
        if (tab_ == 1) return static_cast<int>(outputs_.size()) + 1;
        if (tab_ == 2) return static_cast<int>(options().size());
        return 5;
    }
    void move_selection(int amount) {
        int n = count();
        if (n > 0)
            selected_[static_cast<std::size_t>(tab_)] =
                std::clamp(selected_[static_cast<std::size_t>(tab_)] + amount, 0, n - 1);
    }
    void header() {
        text_at(1, 3, "✦  glint", accent);
        text_at(1, 16, "wallpapers, in your terminal", muted);
        text_at(1, std::max(48, COLS - 18),
                busy()    ? "● working"
                : online_ ? "● connected"
                          : "○ offline",
                online_ ? success : muted);
        const std::array<std::string, 4> tabs = {"Library", "Displays", "Settings", "Workshop"};
        int x = 3;
        for (int i = 0; i < 4; ++i) {
            const std::string label =
                " " + std::to_string(i + 1) + " " + tabs[static_cast<std::size_t>(i)] + " ";
            text_at(3, x, label, i == tab_ ? highlighted : muted);
            x += static_cast<int>(label.size()) + 2;
        }
        text_at(5, 3,
                tab_ == 2
                    ? "Global playback · Project: " + (settings_output() == "*" ? "All displays" : settings_output())
                    : "Apply to: " + (target_ == "*" ? "all displays" : target_),
                accent);
        text_at(LINES - 3, 3, message_, failed_ ? error_color : muted);
        if (COLS < 100) {
            text_at(LINES - 2, 3,
                    tab_ == 0
                        ? "↑↓ select · Enter apply · Tab pages · / search · o display · q quit"
                        : "↑↓ select · Enter edit · Tab pages · o display · r refresh · q quit",
                    muted);
            return;
        }
        text_at(LINES - 2, 3,
                tab_ == 0 ? "↑↓ select   enter apply   / search   o display   e settings   f file  "
                            " r refresh   q quit"
                          : "↑↓ select   enter edit   tab next page   o display   p pause   u "
                            "resume   r refresh   q quit",
                muted);
    }
    void row(int y, int x, int width, const std::string& label, const std::string& detail,
             bool active) {
        if (active) {
            wattr_set(stdscr, A_NORMAL, highlighted, nullptr);
            mvhline(y, x, ' ', std::max(0, width));
        }
        text_at(y, x, (active ? " ❯ " : "   ") + label, active ? highlighted : normal, width);
        if (!detail.empty()) text_at(y + 1, x + 3, detail, muted, width - 3);
    }
    short pixel_pair(int fg, int bg) {
        int key = fg * 256 + bg;
        auto found = pairs_.find(key);
        if (found != pairs_.end()) return found->second;
        if (pairs_.size() >= static_cast<std::size_t>(std::min(COLOR_PAIRS - 32, 30000)))
            return normal;
        short pair = static_cast<short>(32 + pairs_.size());
        if (init_pair(pair, static_cast<short>(fg), static_cast<short>(bg)) == ERR) return normal;
        pairs_[key] = pair;
        return pair;
    }
    void delete_kitty_image() {
        if (!kitty_visible_) return;
        std::cout << "\033_Ga=d,d=I,i=" << static_cast<unsigned>(getpid()) << ";\033\\"
                  << std::flush;
        kitty_visible_ = false;
    }
    void render_kitty_image() {
        if (!kitty_ || kitty_visible_ || modal_ || pixels_.empty() || tab_ != 0 ||
            std::getenv("NO_COLOR"))
            return;
        std::cout << "\0337\033[" << preview_y_ + 1 << ';' << preview_x_ + 1 << 'H'
                  << kitty_image(pixels_, preview_w_, preview_h_, preview_cols_, preview_rows_,
                                 static_cast<unsigned>(getpid()))
                  << "\0338" << std::flush;
        kitty_visible_ = true;
    }
    static int color_at(const std::string& data, std::size_t offset) {
        const int r = static_cast<unsigned char>(data[offset]);
        const int g = static_cast<unsigned char>(data[offset + 1]);
        const int b = static_cast<unsigned char>(data[offset + 2]);
        auto cube = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
        if (std::max({r, g, b}) - std::min({r, g, b}) < 12)
            return 232 + std::clamp(((r + g + b) / 3 - 8) / 10, 0, 23);
        return 16 + 36 * cube(r) + 6 * cube(g) + cube(b);
    }
    void preview(const json& item, int x, int y, int width, int height) {
        if (width < 8 || height < 3) return;
        std::filesystem::path path(item.value("path", ""));
        path /= item.value("preview", "");
        std::string key =
            path.string() + ":" + std::to_string(width) + ":" + std::to_string(height);
        if (key != preview_key_) {
            delete_kitty_image();
            preview_key_ = key;
            pairs_.clear();
            pixels_.clear();
            preview_.stop();
            preview_w_ = kitty_ ? std::min(width * 10, 640) : width;
            preview_h_ = kitty_ ? std::min(height * 20, 400) : height * 2;
            preview_x_ = x;
            preview_y_ = y;
            preview_cols_ = width;
            preview_rows_ = height;
            std::error_code error;
            if (!item.value("preview", "").empty() &&
                std::filesystem::is_regular_file(path, error)) {
                const std::string size =
                    std::to_string(preview_w_) + ":" + std::to_string(preview_h_);
                preview_.start(
                    {"ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-protocol_whitelist",
                     "file,pipe", "-i", path.string(), "-frames:v", "1", "-vf",
                     "scale=" + size + ":force_original_aspect_ratio=decrease,pad=" + size +
                         ":(ow-iw)/2:(oh-ih)/2:color=0x12141f",
                     "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"});
            }
        }
        if (pixels_.empty() || (!kitty_ && (COLORS < 256 || !has_colors())) ||
            std::getenv("NO_COLOR")) {
            text_at(y + height / 2, x + 2,
                    preview_.running() ? "Loading preview…" : "Preview unavailable", muted,
                    width - 2);
            return;
        }
        if (kitty_) return;
        for (int row_y = 0; row_y < height; ++row_y) {
            for (int col = 0; col < width; ++col) {
                auto top = static_cast<std::size_t>(((row_y * 2) * width + col) * 3);
                auto bottom = top + static_cast<std::size_t>(width * 3);
                short pair = pixel_pair(color_at(pixels_, top), color_at(pixels_, bottom));
                wattr_set(stdscr, A_NORMAL, pair, nullptr);
                move(y + row_y, x + col);
                addwstr(L"▀");
            }
        }
    }
    void draw(bool present = true) {
        if (tab_ != 0 || modal_ || COLS < 76 || LINES < 22 || picked() == nullptr)
            delete_kitty_image();
        erase();
        if (COLS < 76 || LINES < 22) {
            text_at(1, 1, "glint · resize the terminal to at least 76 × 22", accent);
            text_at(3, 1, "q to quit", muted);
            if (present) refresh();
            return;
        }
        header();
        if (tab_ == 0) {
            const int left = std::max(30, COLS * 42 / 100);
            const int rows = std::max(1, (LINES - 12) / 3);
            const int first = (selected_[0] / rows) * rows;
            text_at(6, 3,
                    std::to_string(filtered_.size()) + " wallpapers" +
                        (query_.empty() ? "" : " · " + query_),
                    muted, left - 5);
            for (int n = first; n < std::min(first + rows, static_cast<int>(filtered_.size()));
                 ++n) {
                const auto& item = library_[filtered_[static_cast<std::size_t>(n)]];
                row(8 + (n - first) * 3, 2, left - 4, item.value("title", "Untitled"),
                    item.value("type", "") + " · " + item.value("id", ""), n == selected_[0]);
            }
            if (const auto* item = picked()) {
                const int right = left + 2, width = COLS - right - 3;
                preview(*item, right, 7, width, std::max(3, LINES - 16));
                text_at(LINES - 8, right, item->value("title", ""), accent, width);
                text_at(LINES - 6, right, "Enter to apply · e for current wallpaper settings",
                        muted, width);
            } else
                text_at(10, left + 2, "No installed wallpapers match this search.", muted);
        } else if (tab_ == 1) {
            row(8, 3, COLS - 7, "All displays",
                "Apply wallpaper settings to every connected display", selected_[1] == 0);
            int y = 11;
            for (std::size_t i = 0; i < outputs_.size() && y < LINES - 5; ++i, y += 3) {
                const auto& o = outputs_[i];
                const auto w = o.value("wallpaper", json::object());
                row(y, 3, COLS - 7,
                    o.value("name", "") + " · " + std::to_string(o.value("width", 0)) + " × " +
                        std::to_string(o.value("height", 0)),
                    w.value("state", "none") + " · " + w.value("path", "No wallpaper"),
                    static_cast<int>(i) + 1 == selected_[1]);
            }
        } else if (tab_ == 2) {
            if (target_ == "*" && props_.empty())
                text_at(6, 3,
                        "General options for all displays · o selects a display for its project "
                        "settings",
                        muted);
            const auto list = options();
            int rows = std::max(1, (LINES - 11) / 2);
            selected_[2] =
                std::clamp(selected_[2], 0, std::max(0, static_cast<int>(list.size()) - 1));
            int first = (selected_[2] / rows) * rows;
            for (int i = first; i < std::min(first + rows, static_cast<int>(list.size())); ++i) {
                const auto& option = list[static_cast<std::size_t>(i)];
                row(8 + (i - first) * 2, 3, COLS - 7, option.label + "   " + option.value, "",
                    i == selected_[2]);
            }
            if (list.empty())
                text_at(9, 4, "Select a display with an active wallpaper, then press r.", muted);
        } else {
            row(7, 3, COLS - 7, "Find wallpapers", "Search Wallpaper Engine's Workshop in Steam",
                selected_[3] == 0);
            row(9, 3, COLS - 7, "Open a Workshop item", "Paste an item ID or Steam Workshop link",
                selected_[3] == 1);
            row(11, 3, COLS - 7, "Download with SteamCMD",
                "Download by ID · Steam account and SteamCMD required", selected_[3] == 2);
            row(13, 3, COLS - 7, "Import downloaded folder",
                "Add an existing SteamCMD download to glint's library", selected_[3] == 3);
            row(15, 3, COLS - 7, "Refresh downloaded wallpapers",
                "Steam downloads subscriptions; glint reads the installed files",
                selected_[3] == 4);
            text_at(LINES - 5, 6,
                    "Steam handles login and ownership. Glint never asks for your password.",
                    muted);
        }
        if (present) {
            refresh();
            render_kitty_image();
        }
    }
    std::optional<std::string> prompt(const std::string& title, const std::string& initial = "") {
        Modal modal(modal_);
        std::string value = initial;
        for (;;) {
            draw(false);
            const int y = LINES / 2;
            wattr_set(stdscr, A_NORMAL, highlighted, nullptr);
            for (int row_y = y - 2; row_y <= y + 2; ++row_y)
                mvhline(row_y, 3, ' ', std::max(1, COLS - 6));
            text_at(y - 1, 5, title, highlighted, COLS - 10);
            text_at(y + 1, 5, value + "▏", highlighted, COLS - 10);
            refresh();
            wint_t key;
            int result = get_wch(&key);
            if (result == ERR) {
                tick();
                continue;
            }
            if (result == KEY_CODE_YES) {
                if (key == KEY_BACKSPACE) {
                    backspace(value);
                }
                continue;
            }
            if (key == 27) return std::nullopt;
            if (key == 21) {
                value.clear();
                continue;
            }
            if (key == '\n' || key == '\r') return value;
            if (key == 127 || key == 8) {
                backspace(value);
            } else if (key >= 32 && value.size() < 2048) {
                char bytes[MB_LEN_MAX];
                std::mbstate_t state{};
                std::size_t n = std::wcrtomb(bytes, static_cast<wchar_t>(key), &state);
                if (n != static_cast<std::size_t>(-1)) value.append(bytes, n);
            }
        }
    }
    std::optional<std::size_t> picker(const std::string& title,
                                      const std::vector<std::string>& items) {
        Modal modal(modal_);
        delete_kitty_image();
        if (items.empty()) return std::nullopt;
        int chosen = 0;
        for (;;) {
            erase();
            text_at(2, 4, "✦  " + title, accent);
            text_at(4, 4, "↑↓ select · enter confirm · esc back", muted);
            int rows = std::max(1, LINES - 9), first = (chosen / rows) * rows;
            for (int i = first; i < std::min(first + rows, static_cast<int>(items.size())); ++i)
                row(6 + i - first, 3, COLS - 7, items[static_cast<std::size_t>(i)], "",
                    i == chosen);
            refresh();
            int key = getch();
            if (key == ERR) {
                tick();
                continue;
            }
            if (key == 27 || key == 'q') return std::nullopt;
            if (key == KEY_UP || key == 'k')
                chosen =
                    (chosen + static_cast<int>(items.size()) - 1) % static_cast<int>(items.size());
            if (key == KEY_DOWN || key == 'j')
                chosen = (chosen + 1) % static_cast<int>(items.size());
            if (key == '\n' || key == KEY_ENTER) return static_cast<std::size_t>(chosen);
        }
    }
    void search() {
        auto value =
            prompt("Search installed wallpapers · Enter to search · Esc to cancel", query_);
        if (value) {
            query_ = *value;
            selected_[0] = 0;
            filter();
        }
    }
    void choose_output() {
        if (busy()) {
            message_ = "Wait for the current request before switching displays.";
            return;
        }
        std::vector<std::string> names = {"All displays"};
        for (const auto& o : outputs_) names.push_back(o.value("name", ""));
        auto choice = picker("Choose a display", names);
        if (choice) {
            target_ = *choice == 0 ? "*" : names[*choice];
            selected_[2] = 0;
            refresh_state();
        }
    }
    void open_file() {
        auto value = prompt("Path to an image, video or Wallpaper Engine project");
        if (value && !value->empty())
            action(
                {{"op", "set"}, {"output", target_}, {"path", resolve_wallpaper(*value).string()}});
    }
    void edit(const Option& option) {
        const json values = status_.value("options", json::object());
        json value;
        const std::string type =
            option.property.is_object() ? option.property.value("type", "") : "";
        if (type == "bool" || (option.property.is_null() && option.key == "mute"))
            value = !(type == "bool" ? option.property.value("value", false)
                                     : values.value("mute", false));
        else if (type == "combo") {
            std::vector<std::string> labels;
            std::vector<json> choices;
            for (const auto& o : option.property.value("options", json::array())) {
                labels.push_back(o.value("label", ""));
                choices.push_back(o.value("value", json()));
            }
            auto choice = picker(option.label, labels);
            if (!choice) return;
            value = choices[*choice];
        } else if (option.property.is_null() &&
                   (option.key == "scaling" || option.key == "transition" ||
                    option.key == "hwdec" || option.key == "fps")) {
            std::vector<std::string> choices;
            if (option.key == "scaling") choices = {"fill", "fit", "center", "tile", "stretch"};
            if (option.key == "transition") choices = {"fade", "none"};
            if (option.key == "hwdec") choices = {"auto", "vaapi", "no"};
            if (option.key == "fps")
                choices = {"Display refresh rate",
                           "24",
                           "30",
                           "60",
                           "90",
                           "120",
                           "144",
                           "165",
                           "240",
                           "Custom…"};
            auto choice = picker(option.label, choices);
            if (!choice) return;
            if (option.key == "fps") {
                if (*choice == 0)
                    value = 0;
                else if (*choice == choices.size() - 1) {
                    auto input = prompt("Frame rate · 0–1000 FPS", "60");
                    if (!input) return;
                    value = json::parse(*input, nullptr, false);
                } else
                    value = std::stoi(choices[*choice]);
            } else
                value = choices[*choice];
        } else {
            std::string hint = option.label;
            if (option.key == "volume" && option.property.is_null()) hint += " · 0–100%";
            if (type == "slider")
                hint += " · " + shown(option.property.value("min", json(0))) + " to " +
                        shown(option.property.value("max", json(1)));
            std::string initial = option.property.is_object()
                                      ? shown(option.property.value("value", json()))
                                      : shown(values.value(option.key, json()));
            if (option.key == "volume" && option.property.is_null())
                initial = std::to_string(static_cast<int>(values.value("volume", 0.0) * 100));
            auto input =
                prompt(hint + " · Enter to save · Esc to cancel", initial == "null" ? "" : initial);
            if (!input) return;
            if (type == "slider" ||
                (option.property.is_null() &&
                 (option.key == "duration" || option.key == "volume" || option.key == "speed"))) {
                value = json::parse(*input, nullptr, false);
                if (!value.is_number()) {
                    message_ = "Enter a number";
                    failed_ = true;
                    return;
                }
                if (option.key == "volume" && option.property.is_null())
                    value = value.get<double>() / 100.0;
            } else
                value = *input;
        }
        if (value.is_discarded()) {
            message_ = "Enter a valid number";
            failed_ = true;
            return;
        }
        if (option.property.is_null() && option.key == "mute" && target_ == "*") {
            bool all_muted = true;
            for (const auto& state : statuses_)
                if (state.value("path", json()).is_string())
                    all_muted =
                        all_muted && state.value("options", json::object()).value("mute", false);
            value = !all_muted;
        }
        edit_setting(option, value);
    }
    void open_steam(const std::string& url) {
        if (opener_.start({"xdg-open", url}, false))
            message_ = "Opening Steam…";
        else {
            message_ = "Could not open Steam. Install xdg-utils and start Steam.";
            failed_ = true;
        }
    }
    void activate() {
        if (busy() && tab_ != 3) {
            message_ = "Waiting for the current request…";
            return;
        }
        if (tab_ == 0) {
            if (const auto* item = picked())
                action({{"op", "set"}, {"output", target_}, {"path", (*item)["path"]}});
        } else if (tab_ == 1) {
            if (selected_[1] == 0) {
                target_ = "*";
                tab_ = 2;
                refresh_state();
            } else if (!outputs_.empty()) {
                target_ = outputs_[static_cast<std::size_t>(std::clamp(
                                       selected_[1] - 1, 0, static_cast<int>(outputs_.size()) - 1))]
                              .value("name", "");
                tab_ = 2;
                refresh_state();
            }
        } else if (tab_ == 2) {
            auto list = options();
            if (!list.empty())
                edit(list[static_cast<std::size_t>(
                    std::clamp(selected_[2], 0, static_cast<int>(list.size()) - 1))]);
        } else {
            if (selected_[3] == 0) {
                auto query = prompt("Search Steam Workshop");
                if (query)
                    open_steam(
                        "steam://openurl/https://steamcommunity.com/workshop/browse/"
                        "?appid=431960&searchtext=" +
                        encode(*query));
            } else if (selected_[3] == 1) {
                auto input = prompt("Workshop item ID or Steam Workshop link");
                if (!input) return;
                const auto id = workshop_item_id(*input);
                if (id.empty()) {
                    message_ = "Enter a valid Workshop item ID or steamcommunity.com link";
                    failed_ = true;
                } else
                    open_steam("steam://url/CommunityFilePage/" + id);
            } else if (selected_[3] == 2) {
                if (busy()) {
                    message_ = "Wait for the current request before downloading.";
                    return;
                }
                auto input = prompt("Workshop item ID or Steam Workshop link");
                if (!input) return;
                const auto id = workshop_item_id(*input);
                if (id.empty()) {
                    message_ = "Enter a valid Workshop item ID";
                    failed_ = true;
                    return;
                }
                auto account =
                    prompt("Steam account name · password and Steam Guard are entered in SteamCMD");
                if (!account || account->empty()) return;
                def_prog_mode();
                endwin();
                std::cout
                    << "\nSteamCMD download · sign in with an account that owns Wallpaper Engine.\n"
                    << std::flush;
                const auto error = download_workshop(id, *account);
                std::cout << (error.empty() ? "\nDownload added to glint's library." : "\n" + error)
                          << "\nPress Enter to return to glint." << std::flush;
                std::string line;
                std::getline(std::cin, line);
                reset_prog_mode();
                clearok(stdscr, TRUE);
                load_library();
                message_ = error.empty() ? "Download added to the library" : error;
                failed_ = !error.empty();
            } else if (selected_[3] == 3) {
                auto path = prompt("Path to the downloaded Workshop item folder (numeric item ID)");
                if (!path || path->empty()) return;
                const auto error =
                    register_workshop_download(resolve_wallpaper(*path), download_path());
                load_library();
                message_ = error.empty() ? "Downloaded wallpaper added to the library" : error;
                failed_ = !error.empty();
            } else {
                load_library();
                message_ = "Installed wallpapers refreshed";
            }
        }
    }
};
}  // namespace

int run_tui() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr,
                     "glint: the TUI needs an interactive terminal. Use glint list, choose or "
                     "--help for command-line use.\n");
        return 2;
    }
    const char* term = std::getenv("TERM");
    if (term == nullptr || std::string_view(term) == "dumb") {
        std::fprintf(
            stderr,
            "glint: use a terminal with color and cursor support (TERM is unset or dumb).\n");
        return 2;
    }
    Screen screen;
    App app;
    return app.run();
}
}  // namespace glint::cli
