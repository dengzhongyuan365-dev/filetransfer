#include "lan/fs/file_descriptor.h"

#include <unistd.h>

#include <utility>

namespace lan {

FileDescriptor::FileDescriptor(int fd) noexcept : fd_(fd) {}

FileDescriptor::~FileDescriptor() {
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int FileDescriptor::get() const noexcept {
    return fd_;
}

int FileDescriptor::release() noexcept {
    return std::exchange(fd_, -1);
}

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

FileDescriptor::operator bool() const noexcept {
    return fd_ >= 0;
}

}  // namespace lan
