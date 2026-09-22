#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>
#include <vector>
#include <map>
#include <cstddef>

// Minimal, nginx-inspired configuration model.
//
// NOTE ON SCOPE: parsing the configuration file is officially the Core
// Server teammate's responsibility (see guide_jour1_HTTP_CGI.md, part 5,
// "Parser le fichier de configuration"). This parser exists only so the
// HTTP + CGI code (RequestHandler, CgiHandler) has something real to route
// against while being developed and tested in isolation. The *shape* of
// ServerConfig/Location below is the actual contract handle_request()
// relies on -- keep it, even if the parsing implementation itself gets
// replaced later.

struct Location {
    std::string path;                          // e.g. "/kapouet"
    std::string root;                           // filesystem root, e.g. "www"
    std::vector<std::string> methods;            // allowed methods, e.g. GET/POST/DELETE
    bool autoindex;
    std::string index;                           // default file served for a directory
    std::string redirect_target;                 // if non-empty: always redirect here
    int redirect_code;
    bool upload_enabled;
    std::string upload_store;                    // where uploaded files are written
    std::map<std::string, std::string> cgi_extensions;  // ".py" -> "/usr/bin/python3"

    Location()
        : autoindex(false), redirect_code(0), upload_enabled(false) {}

    bool methodAllowed(const std::string& method) const;
};

struct ServerConfig {
    std::string host;
    int port;
    std::string server_name;
    size_t client_max_body_size;
    std::map<int, std::string> error_pages;       // status -> path to custom body
    std::vector<Location> locations;

    ServerConfig() : port(8080), client_max_body_size(1 * 1024 * 1024) {}

    // Longest-prefix match, nginx style. Returns 0 if nothing matches.
    const Location* matchLocation(const std::string& reqPath) const;
};

class Config {
public:
    // Throws std::runtime_error with a human-readable message on any
    // malformed input -- a config file must never crash the server.
    static std::vector<ServerConfig> load(const std::string& path);
};

#endif
