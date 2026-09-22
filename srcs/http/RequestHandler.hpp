#ifndef REQUEST_HANDLER_HPP
#define REQUEST_HANDLER_HPP

#include <string>

struct Connection;
struct ServerConfig;

namespace request_handler {

void writeErrorResponse(Connection& conn, int code);

void writeResponse(Connection& conn, int code, const std::string& contentType,
                    const std::string& body,
                    const std::string& extraHeaders = "");

}

#endif
