#pragma once

namespace lan {

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) noexcept;
    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    int get() const noexcept;
    int release() noexcept;
    void reset(int fd = -1) noexcept;
    explicit operator bool() const noexcept;

private:
    int fd_;
};

}  // namespace lan
