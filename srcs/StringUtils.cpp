#include "StringUtils.hpp"
#include <sstream>
#include <cctype>
#include <cstdlib>

namespace su {

std::string trim(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

std::string toLower(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    return out;
}

std::string toUpper(const std::string& s) {
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}

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

bool startsWith(const std::string& s, const std::string& prefix) {
    if (prefix.size() > s.size())
        return false;
    return s.compare(0, prefix.size(), prefix) == 0;
}

std::string toString(long value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

std::string toString(size_t value) {
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

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

}  // namespace su
