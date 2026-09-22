#ifndef REQUEST_PARSER_HPP
#define REQUEST_PARSER_HPP

// try_parse_request(Connection&) itself is declared in connection.hpp
// (it's part of the shared contract). This header only exposes a couple
// of pieces that RequestHandler / tests find convenient to reuse.

#include <string>
#include <cstddef>

namespace request_parser {

// Decodes a full RFC 7230 chunked body starting at `data`. On success,
// returns true, fills `out` with the decoded bytes and `consumed` with
// how many bytes of `data` made up the encoded form (including the final
// "0\r\n\r\n"). Returns false if the buffer doesn't yet contain a full
// terminated chunked body (caller should wait for more data). Sets
// `malformed` to true if the encoding itself is invalid.
bool decodeChunked(const std::string& data, std::string& out, size_t& consumed, bool& malformed);

}  // namespace request_parser

#endif
