#include "Config.hpp"
#include "utils/StringUtils.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>

/** @brief Checks `method` against this location's configured `methods` list. */
bool Location::methodAllowed(const std::string& method) const {
    for (size_t i = 0; i < methods.size(); ++i) {
        if (methods[i] == method)
            return true;
    }
    return false;
}

/**
 * @brief Longest-prefix location match, nginx style.
 * @param reqPath Request path to match (already slash-collapsed).
 * @return The best-matching Location, or 0 if nothing matches (only
 *         possible if the config has no "/" catch-all).
 */
const Location* ServerConfig::matchLocation(const std::string& reqPath) const {
    const Location* best = 0;
    size_t bestLen = 0;
    for (size_t i = 0; i < locations.size(); ++i) {
        const std::string& p = locations[i].path;
        bool matches = false;
        if (reqPath == p) {
            matches = true;
        } else if (p.size() > 1 && p[p.size() - 1] == '/' && reqPath == p.substr(0, p.size() - 1)) {
            // A location declared with a trailing slash ("/directory/")
            // must still match the request path without it ("/directory"),
            // so handle_request() can see it resolves to a directory and
            // issue the standard slash-redirect instead of 404ing via the
            // catch-all "/" location.
            matches = true;
        } else if (su::startsWith(reqPath, p)) {
            // Prefix match must land on a path boundary: "/kapouet" matches
            // "/kapouet/x" but not "/kapouetXYZ". A location path of "/" is
            // the universal catch-all.
            if (p == "/" || (p.size() > 0 && p[p.size() - 1] == '/'))
                matches = true;
            else if (reqPath.size() > p.size() && reqPath[p.size()] == '/')
                matches = true;
        }
        if (matches && p.size() >= bestLen) {
            bestLen = p.size();
            best = &locations[i];
        }
    }
    return best;
}

namespace {

/**
 * @brief Splits raw config text into tokens.
 *
 * '{' and '}' are always their own token, everything else is whitespace-
 * separated, and '#' starts a line comment.
 *
 * @param text Whole config file contents.
 * @return Flat token stream for parseServer()/parseLocation() to walk.
 */
std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    bool inComment = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inComment) {
            if (c == '\n')
                inComment = false;
            continue;
        }
        if (c == '#') {
            inComment = true;
            continue;
        }
        if (c == '{' || c == '}') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            tokens.push_back(std::string(1, c));
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        tokens.push_back(cur);
    return tokens;
}

/**
 * @brief Parses one "location <path> { ... }" block.
 * @param tok Full token stream.
 * @param i   Index of the location's path token on entry; advanced past
 *            the block's closing "}" on return.
 * @param loc Filled in with whatever directives were found.
 */
void parseLocation(const std::vector<std::string>& tok, size_t& i, Location& loc) {
    // tok[i] is the location path, tok[i+1] must be "{"
    loc.path = tok[i++];
    if (i >= tok.size() || tok[i] != "{")
        throw std::runtime_error("expected '{' after location " + loc.path);
    ++i;
    while (i < tok.size() && tok[i] != "}") {
        const std::string& directive = tok[i++];
        if (directive == "root") {
            if (i >= tok.size()) throw std::runtime_error("root: missing value");
            loc.root = tok[i++];
        } else if (directive == "index") {
            if (i >= tok.size()) throw std::runtime_error("index: missing value");
            loc.index = tok[i++];
        } else if (directive == "autoindex") {
            if (i >= tok.size()) throw std::runtime_error("autoindex: missing value");
            loc.autoindex = (tok[i++] == "on");
        } else if (directive == "methods") {
            while (i < tok.size() && tok[i] != "}" &&
                   !(tok[i].size() > 0 &&
                     (tok[i] == "root" || tok[i] == "index" || tok[i] == "autoindex" ||
                      tok[i] == "return" || tok[i] == "upload_store" || tok[i] == "cgi" ||
                      tok[i] == "client_max_body_size"))) {
                loc.methods.push_back(tok[i++]);
            }
        } else if (directive == "client_max_body_size") {
            if (i >= tok.size()) throw std::runtime_error("client_max_body_size: missing value");
            bool ok = false;
            long v = su::toLong(tok[i++], ok);
            if (!ok || v < 0) throw std::runtime_error("client_max_body_size: invalid value");
            loc.client_max_body_size = static_cast<size_t>(v);
        } else if (directive == "return") {
            if (i >= tok.size()) throw std::runtime_error("return: missing code");
            bool ok = false;
            loc.redirect_code = static_cast<int>(su::toLong(tok[i++], ok));
            if (!ok) throw std::runtime_error("return: invalid status code");
            if (i >= tok.size()) throw std::runtime_error("return: missing target");
            loc.redirect_target = tok[i++];
        } else if (directive == "upload_store") {
            if (i >= tok.size()) throw std::runtime_error("upload_store: missing value");
            loc.upload_enabled = true;
            loc.upload_store = tok[i++];
        } else if (directive == "cgi") {
            if (i + 1 >= tok.size()) throw std::runtime_error("cgi: expected extension and interpreter path");
            std::string ext = tok[i++];
            std::string interp = tok[i++];
            loc.cgi_extensions[ext] = interp;
        } else {
            throw std::runtime_error("unknown location directive: " + directive);
        }
    }
    if (i >= tok.size())
        throw std::runtime_error("unterminated location block for " + loc.path);
    ++i;  // consume "}"
}

/**
 * @brief Parses one "server { ... }" block.
 * @param tok Full token stream.
 * @param i   Index of the block's opening "{" on entry; advanced past the
 *            closing "}" on return.
 * @param srv Filled in with whatever directives/locations were found.
 */
void parseServer(const std::vector<std::string>& tok, size_t& i, ServerConfig& srv) {
    // tok[i] is "{"
    ++i;
    while (i < tok.size() && tok[i] != "}") {
        const std::string& directive = tok[i++];
        if (directive == "listen") {
            if (i >= tok.size()) throw std::runtime_error("listen: missing value");
            std::string val = tok[i++];
            size_t colon = val.find(':');
            bool ok = false;
            if (colon == std::string::npos) {
                srv.host = "0.0.0.0";
                srv.port = static_cast<int>(su::toLong(val, ok));
            } else {
                srv.host = val.substr(0, colon);
                srv.port = static_cast<int>(su::toLong(val.substr(colon + 1), ok));
            }
            if (!ok) throw std::runtime_error("listen: invalid port in '" + val + "'");
        } else if (directive == "server_name") {
            if (i >= tok.size()) throw std::runtime_error("server_name: missing value");
            srv.server_name = tok[i++];
        } else if (directive == "client_max_body_size") {
            if (i >= tok.size()) throw std::runtime_error("client_max_body_size: missing value");
            bool ok = false;
            long v = su::toLong(tok[i++], ok);
            if (!ok || v < 0) throw std::runtime_error("client_max_body_size: invalid value");
            srv.client_max_body_size = static_cast<size_t>(v);
        } else if (directive == "error_page") {
            if (i + 1 >= tok.size()) throw std::runtime_error("error_page: expected code and path");
            bool ok = false;
            int code = static_cast<int>(su::toLong(tok[i++], ok));
            if (!ok) throw std::runtime_error("error_page: invalid status code");
            srv.error_pages[code] = tok[i++];
        } else if (directive == "location") {
            if (i >= tok.size()) throw std::runtime_error("location: missing path");
            Location loc;
            parseLocation(tok, i, loc);
            srv.locations.push_back(loc);
        } else {
            throw std::runtime_error("unknown server directive: " + directive);
        }
    }
    if (i >= tok.size())
        throw std::runtime_error("unterminated server block");
    ++i;  // consume "}"
}

}  // namespace

/**
 * @brief Loads and parses a webserv config file.
 * @param path Path to the config file.
 * @return One ServerConfig per top-level "server{}" block.
 * @throws std::runtime_error on any malformed input, so a bad config
 *         file fails startup instead of crashing later.
 */
std::vector<ServerConfig> Config::load(const std::string& path) {
    std::ifstream file(path.c_str());
    if (!file.is_open())
        throw std::runtime_error("cannot open config file: " + path);
    std::ostringstream buf;
    buf << file.rdbuf();
    std::vector<std::string> tok = tokenize(buf.str());

    std::vector<ServerConfig> servers;
    size_t i = 0;
    while (i < tok.size()) {
        if (tok[i] != "server")
            throw std::runtime_error("expected top-level 'server' block, got '" + tok[i] + "'");
        ++i;
        if (i >= tok.size() || tok[i] != "{")
            throw std::runtime_error("expected '{' after 'server'");
        ServerConfig srv;
        parseServer(tok, i, srv);
        if (srv.locations.empty())
            throw std::runtime_error("server block has no location{} entries");
        servers.push_back(srv);
    }
    if (servers.empty())
        throw std::runtime_error("no server{} block found in config");
    return servers;
}
