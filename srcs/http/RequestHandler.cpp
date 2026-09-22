#include "connection.hpp"
#include "RequestHandler.hpp"
#include "cgi/CgiHandler.hpp"
#include "HttpStatus.hpp"
#include "utils/StringUtils.hpp"

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

/**
 * @brief Joins a location's filesystem root with a request's leftover path.
 *
 * Alias-style, per the subject's own /kapouet example: `rel` (whatever's
 * left of the URL after the matched location prefix) is appended straight
 * onto `root`, not the full original URL -- so an exact-match location
 * (empty rel) just resolves to `root` itself.
 *
 * @param root Location's configured root directory (or, for an exact-path
 *             location, the exact file it should serve).
 * @param rel  Remainder of the request path after the location prefix.
 * @return Filesystem path to stat()/open().
 */
std::string joinPath(const std::string& root, const std::string& rel) {
    if (rel.empty())
        return root;
    if (!root.empty() && root[root.size() - 1] == '/' && rel[0] == '/')
        return root.substr(0, root.size() - 1) + rel;
    if ((root.empty() || root[root.size() - 1] != '/') && (rel.empty() || rel[0] != '/'))
        return root + "/" + rel;
    return root + rel;
}

/**
 * @brief Directory-traversal guard: rejects any ".." path segment.
 * @param rel Path relative to a location's root (never the raw URL).
 * @return true if `rel` contains a literal ".." segment.
 */
bool hasDotDotSegment(const std::string& rel) {
    std::vector<std::string> segs = su::split(rel, '/');
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i] == "..")
            return true;
    }
    return false;
}

/**
 * @brief Extracts a path's extension, dot included (".py", ".html", ...).
 * @return Empty string if there's no dot in the last path component.
 */
std::string extensionOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "";
    return path.substr(dot);
}

/** @brief Returns everything after the last '/' (or the whole string if there is none). */
std::string basenameOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

/**
 * @brief Finds a CGI script hiding behind trailing PATH_INFO segments.
 *
 * RFC 3875: a request like /cgi-bin/script.py/extra/thing names the script
 * /cgi-bin/script.py with "/extra/thing" passed to it as extra path info,
 * not a 404. Walks `rel` one segment at a time looking for the first
 * boundary that's an existing regular file with a configured CGI
 * extension; everything past it becomes pathInfo.
 *
 * Only called as a fallback when the whole of `rel` doesn't already
 * resolve to a file, so the common case (no trailing extra path) never
 * pays for the walk.
 *
 * @param loc          Location being matched, for its root + cgi_extensions.
 * @param rel          Path remainder to search, one '/'-segment at a time.
 * @param scriptFsPath Set to the resolved script's filesystem path on success.
 * @param pathInfo     Set to whatever's left after the script (RFC 3875 PATH_INFO).
 * @param interpreter  Set to the interpreter configured for the matched extension.
 * @return true if a script boundary was found.
 */
bool resolveCgiScript(const Location& loc, const std::string& rel, std::string& scriptFsPath,
                       std::string& pathInfo, std::string& interpreter) {
    std::vector<std::string> segs = su::split(rel, '/');
    std::string builtRel;
    for (size_t i = 0; i < segs.size(); ++i) {
        builtRel += "/" + segs[i];
        std::string candidate = joinPath(loc.root, builtRel);
        struct stat st;
        if (stat(candidate.c_str(), &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;
        std::map<std::string, std::string>::const_iterator it =
            loc.cgi_extensions.find(extensionOf(candidate));
        if (it == loc.cgi_extensions.end())
            return false;
        scriptFsPath = candidate;
        interpreter = it->second;
        std::string remaining;
        for (size_t j = i + 1; j < segs.size(); ++j)
            remaining += "/" + segs[j];
        pathInfo = remaining;
        return true;
    }
    return false;
}

/**
 * @brief Reads a regular file whole and writes it out as a 200 response.
 * @param conn Connection to write the response into.
 * @param path Filesystem path of the file (already known to exist).
 * @param st   stat() result for `path`, so we don't have to call it twice.
 */
void serveFile(Connection& conn, const std::string& path, const struct stat& st) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.is_open()) {
        request_handler::writeErrorResponse(conn, 403);
        return;
    }
    std::string body;
    body.resize(static_cast<size_t>(st.st_size));
    if (st.st_size > 0)
        file.read(&body[0], st.st_size);
    conn.status_code = 200;
    request_handler::writeResponse(conn, 200, http_status::mimeType(path), body);
}

/**
 * @brief Builds a plain directory-listing page (autoindex on).
 * @param conn    Connection to write the response into.
 * @param fsDir   Filesystem directory to list.
 * @param reqPath Original request path, used for the page title and the
 *                "../" parent link.
 */
void serveAutoindex(Connection& conn, const std::string& fsDir, const std::string& reqPath) {
    DIR* dir = opendir(fsDir.c_str());
    if (dir == 0) {
        request_handler::writeErrorResponse(conn, 403);
        return;
    }
    std::string body = "<!DOCTYPE html>\n<html><head><title>Index of " + reqPath +
                        "</title></head><body>\n<h1>Index of " + reqPath + "</h1>\n<ul>\n";
    if (reqPath != "/")
        body += "<li><a href=\"../\">../</a></li>\n";
    struct dirent* entry;
    while ((entry = readdir(dir)) != 0) {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = joinPath(fsDir, name);
        struct stat st;
        std::string suffix;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            suffix = "/";
        body += "<li><a href=\"" + name + suffix + "\">" + name + suffix + "</a></li>\n";
    }
    closedir(dir);
    body += "</ul>\n<hr><p>webserv</p>\n</body></html>\n";
    conn.status_code = 200;
    request_handler::writeResponse(conn, 200, "text/html", body);
}

}

namespace request_handler {

/**
 * @brief Assembles a full HTTP response (status line + headers + body).
 * @param conn        Connection to write into (conn.write_buffer, reset to
 *                     start sending from byte 0).
 * @param code        Status code.
 * @param contentType MIME type, or empty to omit the header entirely
 *                     (only valid when `body` is also empty).
 * @param body        Response body. Silently dropped for HEAD requests
 *                     (see comment below) but still counted in
 *                     Content-Length.
 * @param extraHeaders Pre-formatted extra header lines ("Name: value\r\n"
 *                      each), appended as-is before the blank line.
 */
void writeResponse(Connection& conn, int code, const std::string& contentType,
                    const std::string& body, const std::string& extraHeaders) {
    std::ostringstream out;
    std::string version = conn.http_version.empty() ? "HTTP/1.1" : conn.http_version;
    out << version << " " << code << " " << http_status::reasonPhrase(code) << "\r\n";
    out << "Server: webserv/1.0\r\n";
    out << "Connection: " << (conn.keep_alive ? "keep-alive" : "close") << "\r\n";
    if (!contentType.empty())
        out << "Content-Type: " << contentType << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << extraHeaders;
    out << "\r\n";
    conn.write_buffer = out.str();
    if (conn.method != "HEAD")
        conn.write_buffer += body;
    conn.bytes_written = 0;
}

/**
 * @brief Writes an error response, using the configured error_page for
 *        `code` if one exists and is readable, else the built-in default.
 * @param conn Connection to write into.
 * @param code Status code (404, 403, 500, ...).
 */
void writeErrorResponse(Connection& conn, int code) {
    if (conn.server_conf) {
        std::map<int, std::string>::const_iterator it = conn.server_conf->error_pages.find(code);
        if (it != conn.server_conf->error_pages.end()) {
            std::ifstream file(it->second.c_str(), std::ios::binary);
            if (file.is_open()) {
                std::ostringstream buf;
                buf << file.rdbuf();
                writeResponse(conn, code, http_status::mimeType(it->second), buf.str());
                return;
            }
        }
    }
    writeResponse(conn, code, "text/html", http_status::defaultErrorBody(code));
}

}

/**
 * @brief Routes a fully-parsed request and writes a response into conn.
 *
 * Order of operations matters here: bail out on a parse error first, then
 * match a location, apply redirects/method checks, then branch on method
 * (DELETE / CGI / POST-upload / GET), falling through to plain static-file
 * or directory handling for everything else. A CGI dispatch leaves
 * conn.state as CGI_RUNNING instead of writing a response directly -- the
 * core loop drives it the rest of the way via CgiHandler.
 *
 * @param conn Connection with method/path/headers/body already filled in
 *             by try_parse_request(); conn.write_buffer (or conn.state)
 *             is set on return.
 */
void handle_request(Connection& conn) {
    if (conn.status_code != 0) {
        request_handler::writeErrorResponse(conn, conn.status_code);
        return;
    }
    if (!conn.server_conf) {
        request_handler::writeErrorResponse(conn, 500);
        return;
    }

    const Location* loc = conn.server_conf->matchLocation(conn.path);
    if (!loc) {
        conn.status_code = 404;
        request_handler::writeErrorResponse(conn, 404);
        return;
    }

    if (!loc->redirect_target.empty()) {
        int code = loc->redirect_code ? loc->redirect_code : 302;
        conn.status_code = code;
        request_handler::writeResponse(conn, code, "text/html", "",
                                        "Location: " + loc->redirect_target + "\r\n");
        return;
    }

    if (!loc->methodAllowed(conn.method)) {
        std::string allow;
        for (size_t i = 0; i < loc->methods.size(); ++i) {
            allow += loc->methods[i];
            if (i + 1 < loc->methods.size())
                allow += ", ";
        }
        conn.status_code = 405;
        request_handler::writeResponse(conn, 405, "text/html", http_status::defaultErrorBody(405),
                                        "Allow: " + allow + "\r\n");
        return;
    }

    if (loc->root.empty()) {
        request_handler::writeErrorResponse(conn, 500);
        return;
    }

    std::string rel = (conn.path.size() >= loc->path.size())
                           ? conn.path.substr(loc->path.size())
                           : "";
    if (hasDotDotSegment(rel)) {
        conn.status_code = 403;
        request_handler::writeErrorResponse(conn, 403);
        return;
    }
    std::string fsPath = joinPath(loc->root, rel);

    struct stat st;
    bool exists = (stat(fsPath.c_str(), &st) == 0);

    if (conn.method == "DELETE") {
        if (!exists) {
            request_handler::writeErrorResponse(conn, 404);
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            request_handler::writeErrorResponse(conn, 403);
            return;
        }
        if (std::remove(fsPath.c_str()) != 0) {
            request_handler::writeErrorResponse(conn, 403);
            return;
        }
        conn.status_code = 204;
        request_handler::writeResponse(conn, 204, "", "");
        return;
    }

    std::string cgiScriptFsPath;
    std::string cgiInterpreter;
    std::string cgiPathInfo;
    bool isCgi = false;
    bool cgiTargetMustExist = true;

    if (exists && S_ISREG(st.st_mode)) {
        std::map<std::string, std::string>::const_iterator cgiIt =
            loc->cgi_extensions.find(extensionOf(fsPath));
        if (cgiIt != loc->cgi_extensions.end()) {
            cgiScriptFsPath = fsPath;
            cgiInterpreter = cgiIt->second;
            isCgi = true;
        }
    } else if (!loc->cgi_extensions.empty()) {
        std::map<std::string, std::string>::const_iterator directIt =
            loc->cgi_extensions.find(extensionOf(fsPath));
        if (directIt != loc->cgi_extensions.end()) {
            cgiScriptFsPath = fsPath;
            cgiInterpreter = directIt->second;
            isCgi = true;
            cgiTargetMustExist = false;
        } else {
            isCgi = resolveCgiScript(*loc, rel, cgiScriptFsPath, cgiPathInfo, cgiInterpreter);
        }
    }

    if (isCgi) {
        if (cgiTargetMustExist && access(cgiScriptFsPath.c_str(), R_OK) != 0) {
            request_handler::writeErrorResponse(conn, 403);
            return;
        }
        conn.cgi_path_info = cgiPathInfo;
        if (cgi_handler::start(conn, cgiScriptFsPath, cgiInterpreter, *loc))
            conn.state = CGI_RUNNING;
        return;
    }

    if (conn.method == "POST") {
        if (!loc->upload_enabled) {
            conn.status_code = 200;
            request_handler::writeResponse(conn, 200, "text/plain", "OK\n");
            return;
        }
        std::string filename = basenameOf(rel);
        if (filename.empty()) {
            std::ostringstream gen;
            gen << "upload_" << static_cast<long>(std::time(0)) << "_" << conn.fd;
            filename = gen.str();
        }
        std::string dest = joinPath(loc->upload_store, filename);
        std::ofstream out(dest.c_str(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            request_handler::writeErrorResponse(conn, 500);
            return;
        }
        if (!conn.body.empty())
            out.write(conn.body.data(), static_cast<std::streamsize>(conn.body.size()));
        out.close();
        conn.status_code = 201;
        request_handler::writeResponse(conn, 201, "text/plain", "Created\n",
                                        "Location: " + conn.path + "\r\n");
        return;
    }

    if (!exists) {
        request_handler::writeErrorResponse(conn, 404);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (conn.path.empty() || conn.path[conn.path.size() - 1] != '/') {
            std::string target = conn.path + "/";
            if (!conn.query_string.empty())
                target += "?" + conn.query_string;
            conn.status_code = 301;
            request_handler::writeResponse(conn, 301, "text/html", "",
                                            "Location: " + target + "\r\n");
            return;
        }
        if (!loc->index.empty()) {
            std::string idx = joinPath(fsPath, loc->index);
            struct stat ist;
            if (stat(idx.c_str(), &ist) == 0 && S_ISREG(ist.st_mode)) {
                serveFile(conn, idx, ist);
                return;
            }
        }
        if (loc->autoindex) {
            serveAutoindex(conn, fsPath, conn.path);
            return;
        }
        request_handler::writeErrorResponse(conn, 404);
        return;
    }

    serveFile(conn, fsPath, st);
}
