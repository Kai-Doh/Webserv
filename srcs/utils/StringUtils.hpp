#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <string>
#include <vector>
#include <cstddef>

namespace su {

std::string trim(const std::string& s);
std::string toLower(const std::string& s);
std::string toUpper(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
bool startsWith(const std::string& s, const std::string& prefix);
std::string toString(long value);
std::string toString(size_t value);
long toLong(const std::string& s, bool& ok);
std::string urlDecode(const std::string& s);
std::string headerKeyToEnv(const std::string& key);

}

#endif
