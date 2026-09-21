#include "connection.hpp"
#include "RequestParser.hpp"
#include "StringUtils.hpp"
#include <cstdlib>
#include <cctype>

namespace {

const size_t MAX_HEADER_SECTION = 8192;
const size_t MAX_URI_LENGTH = 8000;

}  // namespace

namespace request_parser {

bool decodeChunked(const std::string& data, std::string& out, size_t& consumed, bool& malformed) {
    malformed = false;
    size_t pos = 0;
    std::string result;

    while (true) {
        size_t lineEnd = data.find("\r\n", pos);
        if (lineEnd == std::string::npos)
            return false;  // need more data to even read the size line

        std::string sizeLine = data.substr(pos, lineEnd - pos);
        size_t semi = sizeLine.find(';');  // drop chunk-extensions, we don't use them
        if (semi != std::string::npos)
            sizeLine = sizeLine.substr(0, semi);
        sizeLine = su::trim(sizeLine);
        if (sizeLine.empty()) {
            malformed = true;
            return false;
        }

        char* endptr = 0;
        long chunkSizeLong = std::strtol(sizeLine.c_str(), &endptr, 16);
        if (endptr == sizeLine.c_str() || *endptr != '\0' || chunkSizeLong < 0) {
            malformed = true;
            return false;
        }
        size_t chunkSize = static_cast<size_t>(chunkSizeLong);
        pos = lineEnd + 2;

        if (chunkSize == 0) {
            // Trailer section: zero or more "Name: value\r\n" lines,
            // terminated by a lone blank "\r\n".
            size_t p = pos;
            while (true) {
                size_t nl = data.find("\r\n", p);
                if (nl == std::string::npos)
                    return false;  // wait for the rest of the trailer
                if (nl == p) {
                    p = nl + 2;
                    break;
                }
                p = nl + 2;
            }
            consumed = p;
            out = result;
            return true;
        }

        if (data.size() < pos + chunkSize + 2)
            return false;  // wait for the rest of this chunk
        if (data.compare(pos + chunkSize, 2, "\r\n") != 0) {
            malformed = true;
            return false;
        }
        result.append(data, pos, chunkSize);
        pos = pos + chunkSize + 2;
    }
}

}  // namespace request_parser

namespace {

bool findHeaderEnd(const std::string& buf, size_t& headerEnd, size_t& sepLen) {
    size_t crlf = buf.find("\r\n\r\n");
    size_t lf = buf.find("\n\n");  // telnet-friendly fallback
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

// Collapses runs of '/' into one, the way nginx/Apache normalize request
// paths, so "//directory" and "/directory" route to the same location
// instead of the doubled slash silently falling through to the catch-all
// "/" location and 404ing.
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

void failParse(Connection& conn, int code, size_t consumedBytes, bool closeConn) {
    conn.status_code = code;
    conn.keep_alive = !closeConn;
    if (consumedBytes >= conn.read_buffer.size())
        conn.read_buffer.clear();
    else
        conn.read_buffer.erase(0, consumedBytes);
}

}  // namespace

bool try_parse_request(Connection& conn) {
    std::string& buf = conn.read_buffer;

    size_t headerEnd = 0;
    size_t sepLen = 0;
    if (!findHeaderEnd(buf, headerEnd, sepLen)) {
        if (buf.size() > MAX_HEADER_SECTION) {
            failParse(conn, 431, buf.size(), true);
            return true;
        }
        return false;  // headers not fully arrived yet
    }
    // The check above only catches an incomplete, still-growing header
    // section. An oversized header section that completes in a single
    // read() (the common case for anything short of a slow-loris-style
    // drip feed) would otherwise sail straight past it, since
    // findHeaderEnd() already succeeded above -- so the limit must be
    // enforced again here, now that the header section's true length is
    // known.
    //
    // The request line's own length is checked first, and separately from
    // the overall section: an oversized URI necessarily also blows past
    // MAX_HEADER_SECTION (a 9000-byte target alone already exceeds it), so
    // checking section size first would always report 431 and make 414
    // unreachable. Checking the request line specifically first lets an
    // overlong URI report 414, while a short URI with merely bloated
    // headers still correctly falls through to 431 below.
    size_t firstLineEnd = buf.find('\n', 0);
    if (firstLineEnd == std::string::npos || firstLineEnd > headerEnd)
        firstLineEnd = headerEnd;
    if (firstLineEnd > MAX_URI_LENGTH + 32) {  // slack for "METHOD "+" HTTP/x.y"+CR
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

    // --- Request line ---
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

    // --- Headers ---
    std::map<std::string, std::string> headers;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = su::trim(lines[i]);
        if (line.empty())
            continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            failParse(conn, 400, bodyStart, true);
            return true;
        }
        std::string key = su::toLower(su::trim(line.substr(0, colon)));
        std::string value = su::trim(line.substr(colon + 1));
        std::map<std::string, std::string>::iterator existing = headers.find(key);
        if (existing != headers.end()) {
            if (key == "content-length" && existing->second != value) {
                failParse(conn, 400, bodyStart, true);  // conflicting lengths: possible smuggling
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

    bool keepAlive = (version == "HTTP/1.1");
    std::map<std::string, std::string>::const_iterator connHeader = headers.find("connection");
    if (connHeader != headers.end()) {
        std::string v = su::toLower(connHeader->second);
        if (v.find("close") != std::string::npos)
            keepAlive = false;
        else if (v.find("keep-alive") != std::string::npos)
            keepAlive = true;
    }

    size_t maxBody = conn.server_conf ? conn.server_conf->client_max_body_size
                                       : static_cast<size_t>(-1);

    std::map<std::string, std::string>::const_iterator teHeader = headers.find("transfer-encoding");
    bool chunked = (teHeader != headers.end() &&
                    su::toLower(teHeader->second).find("chunked") != std::string::npos);

    std::string body;

    if (chunked) {
        // Bail out before decoding a runaway chunked body into memory:
        // decoded size is always <= encoded size, so once the still-encoded
        // bytes we already hold blow past the limit (plus generous slack
        // for chunk-framing overhead) there is no need to keep buffering.
        if (maxBody <= static_cast<size_t>(-1) - 65536 && buf.size() - bodyStart > maxBody + 65536) {
            failParse(conn, 413, buf.size(), true);
            return true;
        }
        std::string encoded = buf.substr(bodyStart);
        std::string decoded;
        size_t consumed = 0;
        bool malformed = false;
        if (!request_parser::decodeChunked(encoded, decoded, consumed, malformed)) {
            if (malformed) {
                failParse(conn, 400, buf.size(), true);
                return true;
            }
            return false;  // wait for the rest of the chunked body
        }
        if (decoded.size() > maxBody) {
            failParse(conn, 413, bodyStart + consumed, true);
            return true;
        }
        body = decoded;
        buf.erase(0, bodyStart + consumed);
    } else {
        std::map<std::string, std::string>::const_iterator clHeader = headers.find("content-length");
        if (clHeader != headers.end()) {
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
                return false;  // wait for the rest of the body
            body = buf.substr(bodyStart, contentLength);
            buf.erase(0, bodyStart + contentLength);
        } else {
            // No Transfer-Encoding and no Content-Length: RFC 7230
            // 3.3.3 case 6 defines the body length as zero, for any
            // method including POST -- this is not an error (curl -X
            // POST with no -d, and nginx, both treat it this way).
            buf.erase(0, bodyStart);
        }
    }

    conn.method = method;
    conn.path = collapseSlashes(su::urlDecode(rawPath));
    conn.query_string = queryString;
    conn.http_version = version;
    conn.headers = headers;
    conn.body = body;
    conn.keep_alive = keepAlive;
    conn.status_code = 0;
    return true;
}
