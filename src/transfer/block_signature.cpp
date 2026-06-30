#include "lan/transfer/block_signature.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>
#include <sstream>
#include <unistd.h>
#include <utility>

#include "lan/fs/file_descriptor.h"

namespace lan {

namespace {

Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

std::string quote_path(const std::filesystem::path& path) {
    return "'" + path.string() + "'";
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
                       "failed to open file for block signatures " + quote_path(path) + ": " +
                           std::strerror(errno)));
    }

    return Result<FileDescriptor>::success(FileDescriptor(fd));
}

}  // namespace

std::uint32_t weak_checksum(std::span<const std::byte> data) {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    const auto size = static_cast<std::uint32_t>(data.size());

    for (std::uint32_t i = 0; i < size; ++i) {
        const auto value = std::to_integer<std::uint32_t>(data[i]);
        a = (a + value) & 0xffffu;
        b = (b + (size - i) * value) & 0xffffu;
    }

    return a | (b << 16);
}

Result<std::string> sha256_bytes(std::span<const std::byte> data) {
    using ContextPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    ContextPtr context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) {
        return Result<std::string>::failure(
            make_error(ErrorCode::internal_error, "failed to allocate OpenSSL digest context"));
    }

    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return Result<std::string>::failure(
            make_error(ErrorCode::internal_error, "EVP_DigestInit_ex failed"));
    }

    if (!data.empty() &&
        EVP_DigestUpdate(context.get(), data.data(), data.size()) != 1) {
        return Result<std::string>::failure(
            make_error(ErrorCode::internal_error, "EVP_DigestUpdate failed"));
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) != 1) {
        return Result<std::string>::failure(
            make_error(ErrorCode::internal_error, "EVP_DigestFinal_ex failed"));
    }

    return Result<std::string>::success(to_hex(digest.data(), digest_size));
}

Result<std::vector<BlockSignature>> build_block_signatures(const std::filesystem::path& path,
                                                           std::uint32_t block_size) {
    if (block_size == 0) {
        return Result<std::vector<BlockSignature>>::failure(
            make_error(ErrorCode::invalid_argument, "block size must be greater than zero"));
    }

    auto file = open_for_read(path);
    if (!file) {
        return Result<std::vector<BlockSignature>>::failure(file.error());
    }

    std::vector<BlockSignature> signatures;
    std::vector<std::byte> buffer(block_size);
    std::uint64_t offset = 0;
    std::uint64_t index = 0;

    while (true) {
        const auto bytes_read = ::read(file.value().get(), buffer.data(), buffer.size());
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }

            return Result<std::vector<BlockSignature>>::failure(
                make_error(ErrorCode::io_error,
                           "failed to read file for block signatures " + quote_path(path) + ": " +
                               std::strerror(errno)));
        }

        if (bytes_read == 0) {
            break;
        }

        const auto block = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(bytes_read));
        auto strong = sha256_bytes(block);
        if (!strong) {
            return Result<std::vector<BlockSignature>>::failure(strong.error());
        }

        signatures.push_back(BlockSignature{
            .index = index,
            .offset = offset,
            .size = static_cast<std::uint32_t>(bytes_read),
            .weak_checksum = weak_checksum(block),
            .strong_checksum = strong.value(),
        });

        offset += static_cast<std::uint64_t>(bytes_read);
        ++index;
    }

    return Result<std::vector<BlockSignature>>::success(std::move(signatures));
}

}  // namespace lan
