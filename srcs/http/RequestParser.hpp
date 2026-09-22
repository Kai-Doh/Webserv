#ifndef REQUEST_PARSER_HPP
#define REQUEST_PARSER_HPP

// try_parse_request(Connection&) itself is declared in connection.hpp
// (it's part of the shared contract). This header only exposes a couple
// of pieces that RequestHandler / tests find convenient to reuse.

#include <cstddef>

struct Connection;

namespace request_parser {

/**
 * @brief Incrementally decodes an RFC 7230 chunked body, resuming from
 *        conn.chunked_scan_pos so a body spread across many partial
 *        read()s is only ever scanned once in total.
 *
 * Each fully-received chunk's payload is appended straight to conn.body
 * as soon as it's confirmed; on the call that finally returns true,
 * conn.body already holds the complete decoded payload.
 *
 * @param conn          Connection being parsed.
 * @param bodyStart     Offset into conn.read_buffer where the chunked
 *                       stream begins.
 * @param totalConsumed Set, once true is returned, to how many bytes from
 *                       bodyStart made up the whole encoded stream
 *                       (including the terminating "0\r\n\r\n").
 * @param malformed     Set to true if the encoding itself is broken.
 * @return true once the terminating chunk + trailer has been seen; false
 *         if more data is needed or the encoding is invalid.
 */
bool decodeChunked(Connection& conn, size_t bodyStart, size_t& totalConsumed, bool& malformed);

}  // namespace request_parser

#endif
