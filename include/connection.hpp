#ifndef CONNECTION_HPP
#define CONNECTION_HPP

#include <string>
#include <map>
#include <ctime>
#include <sys/types.h>

#include "config/Config.hpp"

enum ConnState {
    READING_REQUEST,
    PROCESSING,
    CGI_RUNNING,
    WRITING_RESPONSE,
    DONE
};

struct Connection {
    int fd;
    ConnState state;
    std::string read_buffer;
    std::string write_buffer;
    size_t bytes_written;
    bool keep_alive;

    time_t last_activity;

    const ServerConfig* server_conf;

    std::string method;
    std::string path;
    std::string http_version;
    std::map<std::string, std::string> headers;
    std::string body;
    int cgi_stdin_fd;
    int cgi_stdout_fd;
    pid_t cgi_pid;

    std::string query_string;

    std::string cgi_path_info;

    int status_code;

    std::string cgi_out;
    size_t cgi_in_offset;
    time_t cgi_deadline;

    bool headers_ready;

    size_t body_start;

    size_t chunked_scan_pos;

    Connection()
        : fd(-1), state(READING_REQUEST), bytes_written(0), keep_alive(true),
          last_activity(0), server_conf(0), cgi_stdin_fd(-1), cgi_stdout_fd(-1),
          cgi_pid(-1), status_code(0), cgi_in_offset(0), cgi_deadline(0),
          headers_ready(false), body_start(0), chunked_scan_pos(0) {}
};

bool try_parse_request(Connection& conn);
void handle_request(Connection& conn);

#endif
