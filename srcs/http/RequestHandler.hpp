#ifndef REQUEST_HANDLER_HPP
#define REQUEST_HANDLER_HPP

#include <string>

struct Connection;
struct ServerConfig;
struct Location;

namespace request_handler {

void writeErrorResponse(Connection& conn, int code, const Location* loc = 0);

void writeResponse(Connection& conn, int code, const std::string& contentType,
                    const std::string& body,
                    const std::string& extraHeaders = "");

}

#endif
