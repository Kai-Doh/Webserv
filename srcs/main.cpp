// Entry point: loads the config, then hands the whole run to the Core
// Server's single-poll() event loop (core/Server.hpp) until SIGINT/SIGTERM.
// This replaces harness_main.cpp, the throwaway stand-in used while the
// HTTP+CGI half was developed against a real Core Server that didn't exist
// yet -- Server now drives try_parse_request()/handle_request() the same
// way, plus the CGI pipe integration that was missing from it
// (see core/ServerCgi.cpp).
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
    // Subject p.8: "a configuration file, provided as an argument on the
    // command line, or available in a default path" -- so a missing
    // argument falls back to DEFAULT_CONFIG_PATH instead of a hard error.
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
