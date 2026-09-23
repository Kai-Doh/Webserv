#include "connection.hpp"
#include "RequestParser.hpp"
#include "utils/StringUtils.hpp"
#include <cstdlib>
#include <cctype>
#include <cerrno>

namespace {

const size_t MAX_HEADER_SECTION = 8192;
const size_t MAX_URI_LENGTH = 8000;
const size_t MAX_HEADER_COUNT = 100;

}

namespace request_parser {

bool decodeChunked(Connection& conn, size_t bodyStart, size_t& totalConsumed, bool& malformed) {
    malformed = false;
    const std::string& data = conn.read_buffer;
    size_t pos = bodyStart + conn.chunked_scan_pos;

    while (true) {
        size_t lineEnd = data.find("\r\n", pos);
        if (lineEnd == std::string::npos)
            return false;

        std::string sizeLine = data.substr(pos, lineEnd - pos);
        size_t semi = sizeLine.find(';');
        if (semi != std::string::npos)
            sizeLine = sizeLine.substr(0, semi);
        sizeLine = su::trim(sizeLine);
        if (sizeLine.empty()) {
            malformed = true;
            return false;
        }

        char* endptr = 0;
        errno = 0;
        long chunkSizeLong = std::strtol(sizeLine.c_str(), &endptr, 16);
        if (endptr == sizeLine.c_str() || *endptr != '\0' || chunkSizeLong < 0 || errno == ERANGE) {
            malformed = true;
            return false;
        }
        size_t chunkSize = static_cast<size_t>(chunkSizeLong);
        size_t chunkDataStart = lineEnd + 2;

        if (chunkSize == 0) {
            size_t p = chunkDataStart;
            while (true) {
                size_t nl = data.find("\r\n", p);
                if (nl == std::string::npos)
                    return false;
                if (nl == p) {
                    p = nl + 2;
                    break;
                }
                p = nl + 2;
            }
            totalConsumed = p - bodyStart;
            return true;
        }

        if (data.size() < chunkDataStart + chunkSize + 2)
            return false;
        if (data.compare(chunkDataStart + chunkSize, 2, "\r\n") != 0) {
            malformed = true;
            return false;
        }
        conn.body.append(data, chunkDataStart, chunkSize);
        pos = chunkDataStart + chunkSize + 2;
        conn.chunked_scan_pos = pos - bodyStart;
    }
}

}

namespace {

/**
 * @brief Locates the header/body separator in a request buffer.
 * @param buf       Bytes accumulated so far for this request.
 * @param headerEnd Set to the offset where the header section ends.
 * @param sepLen    Set to 4 for "\r\n\r\n", 2 for the telnet-friendly "\n\n".
 * @return false if neither separator has shown up yet.
 */
bool findHeaderEnd(const std::string& buf, size_t& headerEnd, size_t& sepLen) {
    size_t crlf = buf.find("\r\n\r\n");
    size_t lf = buf.find("\n\n");
    if (crlf != std::string::npos && (lf == std::string::npos || crlf <= lf)) {
        headerEnd = crlf;
        sepLen = 4;
        return true;
    }
    if (lf != std::string::npos) {
        headerEnd = lf;
        sepLen = 2;
        return true;
    }
    return false;
}

/**
 * @brief Collapses runs of '/' into one, nginx/Apache-style.
 *
 * Without this, "//directory" and "/directory" are different strings as
 * far as location matching is concerned, so the doubled slash silently
 * falls through to the catch-all "/" location and 404s instead of hitting
 * the route it obviously meant.
 *
 * @param path Decoded request path.
 * @return Same path with consecutive slashes merged into one.
 */
std::string collapseSlashes(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && !out.empty() && out[out.size() - 1] == '/')
            continue;
        out += path[i];
    }
    return out;
}

/**
 * @brief Bails out of parsing with an error status.
 * @param conn          Connection being parsed.
 * @param code          Status code to report (400, 413, 414, 431, 505...).
 * @param consumedBytes How many bytes to drop from read_buffer -- not
 *                      always the whole thing, since a pipelined next
 *                      request might already be sitting right after it.
 * @param closeConn     If true, conn.keep_alive is forced off so the core
 *                      loop closes the socket after sending the error.
 */
void failParse(Connection& conn, int code, size_t consumedBytes, bool closeConn) {
    conn.status_code = code;
    conn.keep_alive = !closeConn;
    if (consumedBytes >= conn.read_buffer.size())
        conn.read_buffer.clear();
    else
        conn.read_buffer.erase(0, consumedBytes);
}

}

/**
 * @brief Tries to parse one full HTTP request out of conn.read_buffer.
 *
 * Handles Content-Length and chunked bodies, keep-alive/pipelining (there
 * can be leftover bytes for the *next* request once this one is consumed),
 * and reports every parse failure through conn.status_code instead of
 * throwing -- handle_request() is what actually turns that into a response.
 *
 * @param conn Connection whose read_buffer is consumed on success/failure,
 *             and whose method/path/headers/body fields get filled in.
 * @return true if a request (good or bad) was fully parsed and conn is
 *         ready for handle_request(); false if read_buffer isn't a
 *         complete request yet, so the caller should wait for more bytes.
 */
bool try_parse_request(Connection& conn) {
    std::string& buf = conn.read_buffer;

    if (!conn.headers_ready) {
        size_t headerEnd = 0;
        size_t sepLen = 0;
        if (!findHeaderEnd(buf, headerEnd, sepLen)) {
            if (buf.size() > MAX_HEADER_SECTION) {
                failParse(conn, 431, buf.size(), true);
                return true;
            }
            return false;
        }
        size_t firstLineEnd = buf.find('\n', 0);
        if (firstLineEnd == std::string::npos || firstLineEnd > headerEnd)
            firstLineEnd = headerEnd;
        if (firstLineEnd > MAX_URI_LENGTH + 32) {
            failParse(conn, 414, headerEnd + sepLen, true);
            return true;
        }
        if (headerEnd > MAX_HEADER_SECTION) {
            failParse(conn, 431, headerEnd + sepLen, true);
            return true;
        }

        std::string headSection = buf.substr(0, headerEnd);
        size_t bodyStart = headerEnd + sepLen;

        std::vector<std::string> lines = su::split(headSection, '\n');
        if (lines.empty()) {
            failParse(conn, 400, bodyStart, true);
            return true;
        }
        if (lines.size() > MAX_HEADER_COUNT + 1) {
            failParse(conn, 431, bodyStart, true);
            return true;
        }

        std::string requestLine = su::trim(lines[0]);
        std::vector<std::string> parts = su::split(requestLine, ' ');
        if (parts.size() != 3) {
            failParse(conn, 400, bodyStart, true);
            return true;
        }
        std::string method = parts[0];
        std::string target = parts[1];
        std::string version = parts[2];

        if (target.size() > MAX_URI_LENGTH) {
            failParse(conn, 414, bodyStart, true);
            return true;
        }
        if (target.empty() || target[0] != '/') {
            failParse(conn, 400, bodyStart, true);
            return true;
        }
        if (!su::startsWith(version, "HTTP/")) {
            failParse(conn, 400, bodyStart, true);
            return true;
        }
        if (version != "HTTP/1.1" && version != "HTTP/1.0") {
            failParse(conn, 505, bodyStart, true);
            return true;
        }

        std::string rawPath = target;
        std::string queryString;
        size_t qpos = target.find('?');
        if (qpos != std::string::npos) {
            rawPath = target.substr(0, qpos);
            queryString = target.substr(qpos + 1);
        }

        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> envNames;
        for (size_t i = 1; i < lines.size(); ++i) {
            std::string line = su::trim(lines[i]);
            if (line.empty())
                continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos) {
                failParse(conn, 400, bodyStart, true);
                return true;
            }
            // RFC 7230 3.2.4: no whitespace is allowed between the field
            // name and the colon -- some implementations strip it silently,
            // others don't, and that disagreement is a known request-
            // smuggling vector when this server sits behind a proxy with
            // different rules. Reject outright rather than guess.
            if (colon > 0 && (line[colon - 1] == ' ' || line[colon - 1] == '\t')) {
                failParse(conn, 400, bodyStart, true);
                return true;
            }
            std::string key = su::toLower(su::trim(line.substr(0, colon)));
            std::string value = su::trim(line.substr(colon + 1));

            // RFC 7230 5.4: a request with more than one Host header, or
            // with an ambiguous Host, must be rejected -- not just quietly
            // resolved by keeping the last one.
            if (key == "host" && headers.find("host") != headers.end()) {
                failParse(conn, 400, bodyStart, true);
                return true;
            }

            // buildEnv() turns every header into an HTTP_ env var for CGI,
            // mapping '-' to '_' (RFC 3875 convention) -- so distinct
            // headers like "X-Foo" and "X_Foo" would otherwise collide into
            // the same HTTP_X_FOO env var, and which one a CGI script's
            // getenv() actually sees becomes an implementation accident
            // rather than something either header sender could predict.
            std::string envName = key;
            for (size_t k = 0; k < envName.size(); ++k) {
                if (envName[k] == '-')
                    envName[k] = '_';
            }
            std::map<std::string, std::string>::const_iterator envIt = envNames.find(envName);
            if (envIt != envNames.end() && envIt->second != key) {
                failParse(conn, 400, bodyStart, true);
                return true;
            }
            envNames[envName] = key;

            std::map<std::string, std::string>::iterator existing = headers.find(key);
            if (existing != headers.end()) {
                if (key == "content-length" && existing->second != value) {
                    failParse(conn, 400, bodyStart, true);
                    return true;
                }
                existing->second = value;
            } else {
                headers[key] = value;
            }
        }

        if (version == "HTTP/1.1" && headers.find("host") == headers.end()) {
            failParse(conn, 400, bodyStart, true);
            return true;
        }

        std::map<std::string, std::string>::const_iterator teCheck = headers.find("transfer-encoding");
        bool chunkedTE = (teCheck != headers.end() &&
                           su::toLower(teCheck->second).find("chunked") != std::string::npos);
        if (chunkedTE && headers.find("content-length") != headers.end()) {
            // RFC 7230 3.3.3: a message with both headers is a smuggling
            // risk and must be rejected outright, not resolved by picking
            // one of the two framings.
            failParse(conn, 400, bodyStart, true);
            return true;
        }

        bool keepAlive = (version == "HTTP/1.1");
        std::map<std::string, std::string>::const_iterator connHeader = headers.find("connection");
        if (connHeader != headers.end()) {
            std::string v = su::toLower(connHeader->second);
            if (v.find("close") != std::string::npos)
                keepAlive = false;
            else if (v.find("keep-alive") != std::string::npos)
                keepAlive = true;
        }

        std::string decodedPath = collapseSlashes(su::urlDecode(rawPath));
        if (decodedPath.find('\0') != std::string::npos) {
            failParse(conn, 400, bodyStart, true);
            return true;
        }

        conn.method = method;
        conn.path = decodedPath;
        conn.query_string = queryString;
        conn.http_version = version;
        conn.headers = headers;
        conn.keep_alive = keepAlive;
        conn.body_start = bodyStart;
        conn.headers_ready = true;
    }

    size_t bodyStart = conn.body_start;

    size_t maxBody = conn.server_conf ? conn.server_conf->client_max_body_size
                                       : static_cast<size_t>(-1);
    if (conn.server_conf) {
        const Location* earlyLoc = conn.server_conf->matchLocation(conn.path);
        if (earlyLoc && earlyLoc->client_max_body_size != Location::NO_BODY_SIZE_OVERRIDE)
            maxBody = earlyLoc->client_max_body_size;
    }

    std::map<std::string, std::string>::const_iterator teHeader = conn.headers.find("transfer-encoding");
    bool chunked = (teHeader != conn.headers.end() &&
                    su::toLower(teHeader->second).find("chunked") != std::string::npos);

    std::string body;

    if (chunked) {
        if (maxBody <= static_cast<size_t>(-1) - 65536 && buf.size() - bodyStart > maxBody + 65536) {
            failParse(conn, 413, buf.size(), true);
            return true;
        }
        size_t totalConsumed = 0;
        bool malformed = false;
        if (!request_parser::decodeChunked(conn, bodyStart, totalConsumed, malformed)) {
            if (malformed) {
                failParse(conn, 400, buf.size(), true);
                conn.body.clear();
                conn.chunked_scan_pos = 0;
                return true;
            }
            if (conn.body.size() > maxBody) {
                failParse(conn, 413, buf.size(), true);
                conn.body.clear();
                conn.chunked_scan_pos = 0;
                return true;
            }
            return false;
        }
        if (conn.body.size() > maxBody) {
            failParse(conn, 413, bodyStart + totalConsumed, true);
            conn.body.clear();
            conn.chunked_scan_pos = 0;
            return true;
        }
        body = conn.body;
        conn.chunked_scan_pos = 0;
        buf.erase(0, bodyStart + totalConsumed);
    } else {
        std::map<std::string, std::string>::const_iterator clHeader = conn.headers.find("content-length");
        if (clHeader != conn.headers.end()) {
            bool ok = false;
            long len = su::toLong(clHeader->second, ok);
            if (!ok || len < 0) {
                failParse(conn, 400, bodyStart, true);
                return true;
            }
            size_t contentLength = static_cast<size_t>(len);
            if (contentLength > maxBody) {
                failParse(conn, 413, bodyStart, true);
                return true;
            }
            if (buf.size() < bodyStart + contentLength)
                return false;
            body = buf.substr(bodyStart, contentLength);
            buf.erase(0, bodyStart + contentLength);
        } else {
            buf.erase(0, bodyStart);
        }
    }

    conn.body = body;
    conn.status_code = 0;
    conn.headers_ready = false;
    return true;
}
