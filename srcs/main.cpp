#include "core/Server.hpp"
#include "config/Config.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

/**
 * @brief Entry point: loads the config, then runs the server until
 *        SIGINT/SIGTERM (see Server::run()).
 * @param argc/argv Optional config file path (falls back to
 *                  conf/default.conf if omitted).
 * @return 0 on a clean shutdown, 1 on a startup failure (bad args, socket
 *         setup, or a malformed config).
 */
int main(int argc, char** argv) {
    if (argc > 2) {
        std::fprintf(stderr, "usage: %s [config file]\n", argv[0]);
        return 1;
    }
    const char* DEFAULT_CONFIG_PATH = "conf/default.conf";
    std::string configPath = (argc == 2) ? argv[1] : DEFAULT_CONFIG_PATH;

    std::vector<ServerConfig> configs;
    try {
        configs = Config::load(configPath);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "config error: %s\n", e.what());
        return 1;
    }

    for (size_t i = 0; i < configs.size(); ++i) {
        std::fprintf(stderr, "listening on %s:%d\n",
                      configs[i].host.empty() ? "0.0.0.0" : configs[i].host.c_str(),
                      configs[i].port);
    }

    try {
        Server server(configs);
        server.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
