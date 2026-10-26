#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <print>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {
    sigset_t g_orig_mask;

    class Fd {
        int fd_ = -1;

    public:

        Fd() = default;
        explicit Fd(int fd) : fd_(fd) {}
        ~Fd() { reset(); }

        Fd(Fd &&other) noexcept :fd_(std::exchange(other.fd_, -1)) {}
        Fd &operator=(Fd &&other) noexcept {
            if (this != &other){
                reset();
                fd_=std::exchange(other.fd_, -1);
            }
            return *this;
        }

        [[nodiscard]] int get() const { return fd_; }

        void reset() {
            if (fd_ != -1){
                close(fd_);
                fd_ = -1;
            }
        }
    };

    struct CmdResult {
        int exit_code;
        std::string output;
    };

    [[nodiscard]] std::error_code errno_code(int error = errno) {
        return{error, std::generic_category()};
    }

    bool setClionOpaque(bool on) {
        std::string cmd = std::string("hyprctl -q eval 'CLION_OPAQUE:set_enabled(")
                        + (on ? "true" : "false") + ")'";
        return std::system(cmd.c_str()) == 0;
    }

    [[nodiscard]] std::expected<CmdResult, std::error_code> run(const std::vector<std::string> &args) {
        std::vector<char*> argv;
        for (const auto &arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        int fds[2];
        if (pipe2(fds, O_CLOEXEC) == -1){
            throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
        }

        Fd read_end(fds[0]);
        Fd write_end(fds[1]);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDERR_FILENO);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setsigmask(&attr, &g_orig_mask);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

        pid_t pid;
        int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);

        if (rc != 0){
            return std::unexpected(errno_code(rc));
        }

        write_end.reset();
        std::string out;
        std::array<char, 4096> buf;

        for (;;){
            ssize_t ret = read(read_end.get(), buf.data(), buf.size());
            if (ret > 0){
                out.append(buf.data(), static_cast<size_t>(ret));
            } else if (ret == 0 || errno != EINTR){
                break;
            }
        }
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}

        return CmdResult{WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(out)};
    }
}

namespace omarchy {

    [[nodiscard]] std::expected<void, std::string> call(std::vector<std::string> args) {
        args.insert(args.begin(), "omarchy-shell");
        auto result = run(args);
        if (!result){
            return std::unexpected(std::format("could not run omarchy-shell: {}", result.error().message()));
        }
        if (result->exit_code != 0){
            std::string_view output = result->output;
            while (output.ends_with('\n')){
                output.remove_suffix(1);
            }
            return std::unexpected(std::format("omarchy-shell failed (exit {}): {}", result->exit_code, output));
        }
        return {};
    }

    void ping() {
        if (auto result = call({"shell", "ping"}); !result){
            throw std::runtime_error(result.error());
        }
    }

    void set(const std::string_view path) {
        if (auto result = call({"background", "set", std::string(path)}); !result){
            std::println(stderr, "could not set {}: {}", path, result.error());
        }
    }
    
    void restore() {
        if (auto result = call({"background", "refresh"}); !result){
            throw std::runtime_error(result.error());
        }
    }
}

class RestoreImage {
public:
    RestoreImage() { omarchy::ping(); }
    ~RestoreImage() {
        try{
            omarchy::restore();
        } catch (std::exception &e){
            std::println(stderr, "could not restore! Error: {}", e.what());
        }
        if (!setClionOpaque(false)){
            std::println(stderr, "could not reset CLion opacity!");
        }
    }

    RestoreImage(const RestoreImage &) = delete;
    RestoreImage &operator=(const RestoreImage &) = delete;
};

[[nodiscard]] bool isImage(const fs::path &path) {
    static constexpr std::array<std::string_view, 5> extensions = {
        ".jpg", ".jpeg", ".png", ".tif", ".bmp"
    };
    std::string e = path.extension().string();
    std::ranges::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::contains(extensions, e);
}

[[nodiscard]] std::vector<fs::path> getImages(std::span<const char* const> roots) {
    std::vector<fs::path> images;
    for (const char *root : roots){
        fs::path path = fs::absolute(root);
        std::error_code ec;
        if (fs::is_directory(path, ec)){
            for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)){
                std::error_code ec_entry;
                if (it->is_regular_file(ec_entry) && isImage(it->path())){
                    images.emplace_back(it->path());
                }
            }
        } else if (fs::is_regular_file(path, ec) && isImage(path)){
            images.emplace_back(path);
        }
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::ranges::shuffle(images, gen);
    return images;
}

[[nodiscard]] std::size_t getElementCount(std::span<const char* const> roots) {
    size_t size = 0;
    for (const char *root : roots){
        fs::path path = fs::absolute(root);
        std::error_code ec;
        if (fs::is_directory(path, ec)){
            for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)){
                std::error_code ec_entry;
                if (it->is_regular_file(ec_entry) && isImage(it->path())){
                    size++;
                }
            }
        }
    }
    return size;
}

int main(int argc, char** argv) {
    if (argc < 3){
        std::println(stderr, "usage: {} <seconds> <directory|image>...", argv[0]);
        return 2;
    }

    const std::string_view interval_beg = argv[1];
    const char *interval_end = interval_beg.data() + interval_beg.size();

    double seconds = 0.0;
    auto [parsed_end, parsed_ec] = std::from_chars(interval_beg.data(), interval_beg.data() + interval_beg.size(), seconds);
    if (parsed_ec != std::errc{} || parsed_end != interval_end || !(seconds > 0.0 && seconds < 1e9)) {
        std::println(stderr, "invalid interval!");
        return 2;
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);

    struct sigaction ign{};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGHUP, &ign, nullptr);

    pthread_sigmask(SIG_BLOCK, &sigs, &g_orig_mask);
    auto images = getImages(std::span<const char* const>(argv + 2, static_cast<std::size_t>(argc - 2)));
    if (images.empty()){
        std::println(stderr, "no images!");
        return 1;
    }
    const std::chrono::duration<double> interval_d{seconds};
    const auto whole = std::chrono::duration_cast<std::chrono::seconds>(interval_d);
    const auto fraction = std::chrono::duration_cast<std::chrono::nanoseconds>(interval_d - whole);
    const timespec interval {
        .tv_sec = static_cast<time_t>(whole.count()),
        .tv_nsec = static_cast<long>(fraction.count())
    };

    try{
        RestoreImage guard;
        setClionOpaque(true);
        for (std::size_t i = 0;; i = (i + 1) % images.size()){
            omarchy::set(images[i].native());
            std::size_t size = 0;
            try{
                size = getElementCount(std::span<const char* const>(argv + 2, static_cast<std::size_t>(argc - 2)));
            } catch (std::exception &e){
                omarchy::restore();
            } catch (...){
                std::println(stderr, "could not restore! ");
            }
            if (images.size() != size && size != 0) {
                images = getImages(std::span<const char* const>(argv + 2, static_cast<std::size_t>(argc - 2)));
            }
            if (size == 0) {
                try{
                    omarchy::restore();
                } catch (std::exception &e){
                    std::println(stderr, "could not restore! Error: {}", e.what());
                }
            }
            int sig;
            while ((sig = sigtimedwait(&sigs, nullptr, &interval)) == -1 && errno == EINTR) { }

            if (sig > 0) {
                break;
            }
            if (images.size() != size && size != 0) {
                images = getImages(std::span<const char* const>(argv + 2, static_cast<std::size_t>(argc - 2)));
            }
            if (size == 0) {
                try{
                    omarchy::restore();
                } catch (std::exception &e){
                    std::println(stderr, "could not restore! Error: {}", e.what());
                }
            }
        }
    } catch (std::exception &e){
        std::println(stderr, "Error: {}", e.what());
        return 1;
    }


    return 0;
}
