#pragma once

#include <stddef.h>
#include <string.h>

#include <iosfwd>
#include <string>

namespace adt {
class StringRef {
   public:
    using CharT = char;
    using iterator = const CharT*;
    using const_iterator = const CharT*;

    StringRef() : size_(0), str_(nullptr) {}

    StringRef(const char* str) : size_(strlen(str)), str_(str) {}

    template <size_t N>
    StringRef(const char (&str)[N]) : size_(N), str_(str) {}

    StringRef(const std::string& str) : size_(str.size()), str_(str.c_str()) {}

    StringRef(const StringRef& other) = default;
    StringRef(StringRef&& other) = default;

    StringRef& operator=(const StringRef& other) = default;
    StringRef& operator=(StringRef&& other) = default;

    const CharT* c_str() const { return str_; }

    bool empty() const { return !size_ || !str_; }

    size_t size() const { return size_; }

    std::string str() const { return std::string(str_, str_ + size_); }

    const char* data() const { return str_; }

    std::string::size_type find(StringRef other) {
        auto p = strstr(str_, other.c_str());
        if (!p) {
            return std::string::npos;
        }
        return p - str_;
    }

    std::string::size_type contain(StringRef other) {
        return find(other) != std::string::npos;
    }

    friend inline bool operator==(StringRef lhs, StringRef rhs);
    friend inline bool operator!=(StringRef lhs, StringRef rhs);

   private:
    size_t size_;
    const CharT* str_;
};

inline bool operator==(StringRef lhs, StringRef rhs) {
    return lhs.size_ == rhs.size_ &&
           (lhs.str_ == rhs.str_ || !strcmp(lhs.str_, rhs.str_));
}

inline bool operator!=(StringRef lhs, StringRef rhs) { return !(lhs == rhs); }

inline std::ostream& operator<<(std::ostream& os, StringRef str) {
    os << str.str();
    return os;
}

}  // namespace adt