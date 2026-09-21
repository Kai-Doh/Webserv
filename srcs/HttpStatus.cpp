#include "HttpStatus.hpp"
#include "StringUtils.hpp"

namespace http_status {

/** @brief Standard reason phrase for a status code, "Unknown" if we don't recognize it. */
std::string reasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 307: return "Temporary Redirect";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

/** @brief Minimal built-in HTML error page, used when no error_page is configured for `code`. */
std::string defaultErrorBody(int code) {
    std::string reason = reasonPhrase(code);
    std::string codeStr = su::toString(static_cast<long>(code));
    std::string body;
    body += "<!DOCTYPE html>\n<html><head><title>";
    body += codeStr + " " + reason;
    body += "</title></head><body>\n<h1>";
    body += codeStr + " " + reason;
    body += "</h1>\n<hr><p>webserv</p>\n</body></html>\n";
    return body;
}

/** @brief Guesses a Content-Type from a file's extension. Falls back to application/octet-stream. */
std::string mimeType(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "application/octet-stream";
    std::string ext = su::toLower(path.substr(dot + 1));
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "js") return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "txt") return "text/plain";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "pdf") return "application/pdf";
    return "application/octet-stream";
}

}  // namespace http_status
