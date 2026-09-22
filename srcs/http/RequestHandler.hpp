#ifndef REQUEST_HANDLER_HPP
#define REQUEST_HANDLER_HPP

// handle_request(Connection&) is declared in connection.hpp (shared
// contract). This header exposes the pieces other translation units
// (namely CgiHandler.cpp) need to reuse so error responses look uniform
// everywhere.

#include <string>

struct Connection;
struct ServerConfig;

namespace request_handler {

// Writes a complete "status line + headers + body" HTTP response for
// `code` into conn.write_buffer, using the server's configured error page
// for that code if one exists and is readable, falling back to a small
// built-in page otherwise.
void writeErrorResponse(Connection& conn, int code);

// Builds a full response (status line, standard headers, body) into
// conn.write_buffer. `contentType` may be empty only if `body` is empty.
void writeResponse(Connection& conn, int code, const std::string& contentType,
                    const std::string& body,
                    const std::string& extraHeaders = "");

}  // namespace request_handler

#endif
