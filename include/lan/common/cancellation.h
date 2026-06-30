#pragma once

#include <atomic>

namespace lan {

class CancellationToken {
public:
    void cancel() {
        cancelled_.store(true);
    }

    void reset() {
        cancelled_.store(false);
    }

    bool is_cancelled() const {
        return cancelled_.load();
    }

private:
    std::atomic_bool cancelled_ = false;
};

}  // namespace lan
