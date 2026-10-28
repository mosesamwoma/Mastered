#pragma once

#include <stdexcept>
#include <string>

namespace mastered {

class MasteredError : public std::runtime_error {
public:
    explicit MasteredError(const std::string& message) : std::runtime_error(message) {}
};

}
