#include <cerrno>
#include <chrono>
#include <cstdio>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{

bool rejects(const std::string& server, const std::vector<std::string>& options)
{
    std::vector<std::string> arguments = {server, "--backend", "/missing/backend.so", "--plugin", "/missing/plugin.so", "--source",
            "simulated0"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0)
    {
        return false;
    }
    if (child == 0)
    {
        execv(server.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
        {
            return WIFEXITED(status) && WEXITSTATUS(status) == 2;
        }
        if (waited < 0 && errno != EINTR)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        return 2;
    }
    const std::string server = argv[1];
    const std::vector<std::vector<std::string>> invalid = {
            {"--media-mode", "rtp-jpeg"},
            {"--media-mode", "rtp-jpeg", "--media-host", "not-an-ip", "--media-port", "5004"},
            {"--media-mode", "rtp-jpeg", "--media-host", "0.0.0.0", "--media-port", "5004"},
            {"--media-mode", "rtp-jpeg", "--media-host", "0.0.0.1", "--media-port", "5004"},
            {"--media-mode", "rtp-jpeg", "--media-host", "224.0.0.1", "--media-port", "5004"},
            {"--media-mode", "rtp-jpeg", "--media-host", "240.0.0.1", "--media-port", "5004"},
            {"--media-mode", "rtp-jpeg", "--media-host", "127.0.0.1", "--media-port", "0"},
            {"--media-mode", "rtp-jpeg", "--media-host", "127.0.0.1", "--media-port", "65536"},
            {"--media-mode", "rtp-jpeg", "--media-host", "127.0.0.1", "--media-port", "abc"},
            {"--media-mode", "local", "--media-host", "127.0.0.1"},
            {"--media-port", "5004"}};
    for (const auto& options : invalid)
    {
        if (!rejects(server, options))
        {
            std::fprintf(stderr, "M7 server option test failed for invalid media configuration\n");
            return 1;
        }
    }
    return 0;
}
