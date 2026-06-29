#include "lan/common/stopwatch.h"

namespace lan {

Stopwatch::Stopwatch() : start_(clock::now()) {}

void Stopwatch::reset() {
    start_ = clock::now();
}

std::chrono::nanoseconds Stopwatch::elapsed() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start_);
}

double Stopwatch::elapsed_seconds() const {
    return std::chrono::duration<double>(elapsed()).count();
}

}  // namespace lan
