#pragma once

#include <chrono>

namespace lan {

class Stopwatch {
public:
    using clock = std::chrono::steady_clock;

    Stopwatch();

    void reset();
    std::chrono::nanoseconds elapsed() const;
    double elapsed_seconds() const;

private:
    clock::time_point start_;
};

}  // namespace lan
