#ifndef CGI_HANDLER_HPP
#define CGI_HANDLER_HPP

#include <string>
#include <sys/types.h>

struct Connection;
struct Location;

namespace cgi_handler {

bool start(Connection& conn, const std::string& scriptPath, const std::string& interpreter,
           const Location& loc);

void onStdinWritable(Connection& conn);

void onStdoutReadable(Connection& conn);

bool isDone(const Connection& conn);

bool finish(Connection& conn, pid_t& pendingPid);

pid_t abortTimeout(Connection& conn);

bool reapIfExited(pid_t pid);

}

#endif
