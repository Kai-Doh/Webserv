#include "StringUtils.hpp"
#include <sstream>
#include <cctype>
#include <cstdlib>

namespace su {

/** @brief Strips leading/trailing whitespace. */
std::string trim(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

/** @brief Lowercases a copy of `s`. Used for case-insensitive header names/values. */
std::string toLower(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    return out;
}

/** @brief Uppercases a copy of `s`. Used for building CGI HTTP_* env var names. */
std::string toUpper(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}

/**
 * @brief Splits on `delim`, dropping empty fields.
 * @return e.g. split("a//b", '/') -> {"a", "b"}, not {"a", "", "b"}.
 */
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += s[i];
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

/** @brief True if `s` begins with `prefix`. */
bool startsWith(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size())
        return false;
    return s.compare(0, prefix.size(), prefix) == 0;
}

/** @brief long -> std::string (no std::to_string in C++98). */
std::string toString(long value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

/** @brief size_t -> std::string (no std::to_string in C++98). */
std::string toString(size_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

/**
 * @brief Strict string-to-long: digits (and a leading '-') only, no
 *        trailing garbage, unlike a bare strtol() call.
 * @param ok Set to false on anything that isn't a clean integer.
 */
long toLong(const std::string& s, bool& ok) {
    if (s.empty()) {
        ok = false;
        return 0;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i])) && !(i == 0 && s[i] == '-')) {
            ok = false;
            return 0;
        }
    }
    char* endptr = 0;
    long value = std::strtol(s.c_str(), &endptr, 10);
    ok = (endptr != 0 && *endptr == '\0');
    return value;
}

/** @brief Decodes %XX escapes and '+' (as a space), application/x-www-form-urlencoded style. */
std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            std::string hex = s.substr(i + 1, 2);
            int value = static_cast<int>(std::strtol(hex.c_str(), 0, 16));
            out += static_cast<char>(value);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

/** @brief "content-type" -> "HTTP_CONTENT_TYPE", CGI/1.1's header-to-env-var convention. */
std::string headerKeyToEnv(const std::string& key) {
    std::string out = "HTTP_";
    for (size_t i = 0; i < key.size(); ++i) {
        char c = key[i];
        if (c == '-')
            out += '_';
        else
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

}
