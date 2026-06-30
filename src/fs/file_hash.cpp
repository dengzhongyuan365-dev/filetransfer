#include "lan/fs/file_hash.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "lan/fs/file_descriptor.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
}

std::string openssl_error(std::string action) {
    return action + " failed";
}

std::string to_hex(const unsigned char* data, unsigned int size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');

    for (unsigned int i = 0; i < size; ++i) {
        output << std::setw(2) << static_cast<unsigned int>(data[i]);
    }

    return output.str();
}

Result<FileDescriptor> open_for_read(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<FileDescriptor>::failure(
            make_error(ErrorCode::io_error,
                       "failed to open file for hashing " + quote_path(path) + ": " +
                           std::strerror(errno)));
    }

    return Result<FileDescriptor>::success(FileDescriptor(fd));
}

}  // namespace

Result<FileHash> hash_file(const std::filesystem::path& path, std::uint64_t buffer_size) {
    return hash_file(path, buffer_size, nullptr);
}

Result<FileHash> hash_file(const std::filesystem::path& path,
                           std::uint64_t buffer_size,
                           const CancellationToken* cancellation) {
    if (buffer_size == 0) {
        return Result<FileHash>::failure(
            make_error(ErrorCode::invalid_argument, "buffer size must be greater than zero"));
    }

    auto file = open_for_read(path);
    if (!file) {
        return Result<FileHash>::failure(file.error());
    }

    using ContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    ContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) {
        return Result<FileHash>::failure(
            make_error(ErrorCode::internal_error, "failed to allocate OpenSSL digest context"));
    }

    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return Result<FileHash>::failure(
            make_error(ErrorCode::internal_error, openssl_error("EVP_DigestInit_ex")));
    }

    std::vector<unsigned char> buffer(static_cast<std::size_t>(buffer_size));
    std::uint64_t bytes_hashed = 0;

    while (true) {
        if (cancellation != nullptr && cancellation->is_cancelled()) {
            return Result<FileHash>::failure(
                make_error(ErrorCode::cancelled, "file hashing cancelled"));
        }

        const auto bytes_read = ::read(file.value().get(), buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<FileHash>::failure(
                make_error(ErrorCode::io_error,
                           "failed to read file for hashing " + quote_path(path) + ": " +
                               std::strerror(errno)));
        }

        if (bytes_read == 0) {
            break;
        }

        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(bytes_read)) != 1) {
            return Result<FileHash>::failure(
                make_error(ErrorCode::internal_error, openssl_error("EVP_DigestUpdate")));
        }

        bytes_hashed += static_cast<std::uint64_t>(bytes_read);
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        return Result<FileHash>::failure(
            make_error(ErrorCode::internal_error, openssl_error("EVP_DigestFinal_ex")));
    }

    return Result<FileHash>::success(FileHash{
        .algorithm = "sha256",
        .hex_digest = to_hex(digest.data(), digest_size),
        .bytes_hashed = bytes_hashed,
    });
}

}  // namespace lan
