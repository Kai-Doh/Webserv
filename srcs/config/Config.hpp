#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>
#include <map>
#include <cstddef>

struct Location {
    std::string path;
    std::string root;
    std::vector<std::string> methods;
    bool autoindex;
    std::string index;
    std::string redirect_target;
    int redirect_code;
    bool upload_enabled;
    std::string upload_store;
    std::map<std::string, std::string> cgi_extensions;
    std::map<int, std::string> error_pages;

    size_t client_max_body_size;
    static const size_t NO_BODY_SIZE_OVERRIDE = static_cast<size_t>(-1);

    Location()
        : autoindex(false), redirect_code(0), upload_enabled(false),
          client_max_body_size(NO_BODY_SIZE_OVERRIDE) {}

    bool methodAllowed(const std::string& method) const;
};

struct ServerConfig {
    std::string host;
    int port;
    std::string server_name;
    size_t client_max_body_size;
    std::map<int, std::string> error_pages;
    std::vector<Location> locations;

    ServerConfig() : port(8080), client_max_body_size(1 * 1024 * 1024) {}

    const Location* matchLocation(const std::string& reqPath) const;
};

class Config {
public:
    static std::vector<ServerConfig> load(const std::string& path);
};

#endif
