/**
 * @file non_finite_scan.cpp
 * @brief Tokenizer-level scan of generated JSON for bare non-finite numbers.
 */

#include "non_finite_scan.h"

#include <cctype>
#include <string>
#include <vector>

namespace quantra {

namespace {

struct Frame {
    bool isArray = false;
    std::size_t index = 0; // arrays: index of the element being read
    std::string key;       // objects: key of the member being read
    bool awaitingKey = true;
};

std::string BuildPath(const std::vector<Frame>& stack) {
    std::string path;
    for (const auto& f : stack) {
        if (f.isArray) {
            path += "[" + std::to_string(f.index) + "]";
        } else {
            if (!path.empty()) path += ".";
            path += f.key;
        }
    }
    return path.empty() ? std::string("<root>") : path;
}

bool IsDelimiter(char c) {
    return c == ',' || c == '}' || c == ']' || c == ':' ||
           std::isspace(static_cast<unsigned char>(c));
}

// A bare token is a JSON literal or a number. Anything else — in practice
// FlatBuffers' `inf` / `-inf` / `nan` / `-nan` and the `Infinity` / `NaN`
// spellings — is a non-finite double written where JSON has no encoding.
bool IsNonFiniteToken(const std::string& tok) {
    if (tok == "true" || tok == "false" || tok == "null") return false;
    for (char c : tok) {
        if (std::isalpha(static_cast<unsigned char>(c)) && c != 'e' && c != 'E') return true;
    }
    return false;
}

} // namespace

std::optional<std::string> FindNonFiniteValue(const std::string& json) {
    std::vector<Frame> stack;
    const std::size_t n = json.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = json[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '{') {
            Frame f;
            f.isArray = false;
            stack.push_back(f);
            ++i;
        } else if (c == '[') {
            Frame f;
            f.isArray = true;
            stack.push_back(f);
            ++i;
        } else if (c == '}' || c == ']') {
            if (!stack.empty()) stack.pop_back();
            ++i;
        } else if (c == ',') {
            if (!stack.empty()) {
                if (stack.back().isArray) {
                    ++stack.back().index;
                } else {
                    stack.back().awaitingKey = true;
                }
            }
            ++i;
        } else if (c == ':') {
            ++i;
        } else if (c == '"') {
            // String: skip to the closing quote, honouring escapes.
            std::size_t j = i + 1;
            std::string content;
            while (j < n && json[j] != '"') {
                if (json[j] == '\\' && j + 1 < n) {
                    content += json[j];
                    content += json[j + 1];
                    j += 2;
                } else {
                    content += json[j];
                    ++j;
                }
            }
            if (!stack.empty() && !stack.back().isArray && stack.back().awaitingKey) {
                stack.back().key = content;
                stack.back().awaitingKey = false;
            }
            i = (j < n) ? j + 1 : n;
        } else {
            // Bare token: literal or number.
            std::size_t j = i;
            while (j < n && !IsDelimiter(json[j]))
                ++j;
            const std::string tok = json.substr(i, j - i);
            if (IsNonFiniteToken(tok)) return BuildPath(stack);
            i = j;
        }
    }
    return std::nullopt;
}

} // namespace quantra
