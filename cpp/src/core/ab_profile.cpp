#include "core/ab_profile.hpp"

namespace gao {

static size_t FindKeyLineStart(const std::string& text, std::string_view key, size_t& valueStart, size_t& lineEnd) {
    std::string needle = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t ls = text.find(needle, pos);
        if (ls == std::string::npos) return std::string::npos;
        bool atLineStart = (ls == 0) || text[ls-1] == '\n' || text[ls-1] == '\r';
        if (atLineStart) {
            valueStart = ls + needle.size();
            size_t e = text.find('\n', valueStart);
            lineEnd = (e == std::string::npos) ? text.size() : e;
            if (lineEnd > valueStart && text[lineEnd-1] == '\r') lineEnd -= 1;
            return ls;
        }
        pos = ls + needle.size();
    }
    return std::string::npos;
}

std::optional<std::string> ReadProfileKey(const std::string& text, std::string_view key) {
    size_t vs = 0, le = 0;
    if (FindKeyLineStart(text, key, vs, le) == std::string::npos) return std::nullopt;
    return text.substr(vs, le - vs);
}

std::string SetProfileKey(const std::string& text, std::string_view key, std::string_view value) {
    size_t vs = 0, le = 0;
    if (FindKeyLineStart(text, key, vs, le) == std::string::npos) return text;
    std::string out = text;
    out.replace(vs, le - vs, value);
    return out;
}

}
