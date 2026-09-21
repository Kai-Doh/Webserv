#ifndef HTTP_STATUS_HPP
#define HTTP_STATUS_HPP

#include <string>

namespace http_status {

std::string reasonPhrase(int code);
std::string defaultErrorBody(int code);
std::string mimeType(const std::string& path);

}  // namespace http_status

#endif
