#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "lan/app/receiver_config.h"
#include "lan/app/sender_config.h"
#include "lan/net/tcp.h"
#include "lan/protocol/frame.h"
#include "lan/protocol/hello.h"
#include "lan/transfer/single_file.h"

namespace {

struct TestContext {
    std::filesystem::path root;
};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "    failed: " << message << '\n';
        return false;
    }
    return true;
}

std::uint16_t test_port(int offset) {
    const auto pid_part = static_cast<std::uint16_t>(::getpid() % 10000);
    return static_cast<std::uint16_t>(40000 + pid_part + offset);
}

bool write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output << content;
    return output.good();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

lan::ReceiverConfig receiver_config(const std::filesystem::path& dir, std::uint16_t port) {
    lan::ReceiverConfig config;
    config.bind_address = "127.0.0.1";
    config.port = port;
    config.receive_dir = dir;
    return config;
}

lan::SenderConfig sender_config(const std::filesystem::path& source, std::uint16_t port) {
    lan::SenderConfig config;
    config.target.host = "127.0.0.1";
    config.target.port = port;
    config.source_path = source;
    config.chunk_size = 7;
    return config;
}

void wait_for_receiver_to_listen() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void write_u64_be(std::byte* output, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        output[7 - i] = static_cast<std::byte>((value >> (i * 8)) & 0xff);
    }
}

std::vector<std::byte> chunk_body(std::uint64_t offset, const std::string& payload) {
    std::vector<std::byte> body(8 + payload.size());
    write_u64_be(body.data(), offset);
    std::memcpy(body.data() + 8, payload.data(), payload.size());
    return body;
}

bool test_single_file_transfer(const TestContext& context) {
    const auto receive_dir = context.root / "normal-receiver";
    std::filesystem::create_directories(receive_dir);

    const auto source = context.root / "normal-source.txt";
    const std::string content = "single file transfer test\nwith multiple tiny chunks\n";
    if (!expect(write_text_file(source, content), "failed to create source file")) {
        return false;
    }

    const auto port = test_port(1);
    auto receiver = std::async(std::launch::async, [config = receiver_config(receive_dir, port)] {
        return lan::receive_single_file(config);
    });

    wait_for_receiver_to_listen();

    auto sent = lan::send_single_file(sender_config(source, port));
    if (!expect(static_cast<bool>(sent), sent ? "" : sent.error().message)) {
        return false;
    }

    auto received = receiver.get();
    if (!expect(static_cast<bool>(received), received ? "" : received.error().message)) {
        return false;
    }

    const auto target = receive_dir / source.filename();
    return expect(std::filesystem::exists(target), "target file does not exist") &&
           expect(read_text_file(target) == content, "target content does not match source") &&
           expect(!std::filesystem::exists(target.string() + ".part"), "part file was left behind");
}

bool test_partial_file_cleanup_after_disconnect(const TestContext& context) {
    const auto receive_dir = context.root / "disconnect-receiver";
    std::filesystem::create_directories(receive_dir);

    const auto port = test_port(2);
    auto receiver = std::async(std::launch::async, [config = receiver_config(receive_dir, port)] {
        return lan::receive_single_file(config);
    });

    wait_for_receiver_to_listen();

    auto socket = lan::connect_tcp("127.0.0.1", port);
    if (!expect(static_cast<bool>(socket), socket ? "" : socket.error().message)) {
        return false;
    }

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::encode_hello(lan::HelloMetadata{.mode = lan::HelloMode::file, .sender_id = {}});
    auto hello_written = lan::write_frame(socket.value(), hello);
    if (!expect(static_cast<bool>(hello_written), hello_written ? "" : hello_written.error().message)) {
        return false;
    }

    lan::Frame begin;
    begin.type = lan::MessageType::file_begin;
    begin.body = lan::bytes_from_string("name=broken.txt\nsize=100\nsha256=00\n");
    auto begin_written = lan::write_frame(socket.value(), begin);
    if (!expect(static_cast<bool>(begin_written), begin_written ? "" : begin_written.error().message)) {
        return false;
    }

    auto ack = lan::read_frame(socket.value());
    if (!expect(static_cast<bool>(ack), ack ? "" : ack.error().message)) {
        return false;
    }
    if (!expect(ack.value().type == lan::MessageType::ack, "receiver did not ack file_begin")) {
        return false;
    }

    socket.value().reset();

    auto received = receiver.get();
    if (!expect(!received, "receiver unexpectedly accepted an incomplete transfer")) {
        return false;
    }

    return expect(!std::filesystem::exists(receive_dir / "broken.txt"), "final file was created") &&
           expect(!std::filesystem::exists(receive_dir / "broken.txt.part"), "part file was left behind");
}

bool test_invalid_chunk_offset_is_rejected(const TestContext& context) {
    const auto receive_dir = context.root / "invalid-offset-receiver";
    std::filesystem::create_directories(receive_dir);

    const auto port = test_port(3);
    auto receiver = std::async(std::launch::async, [config = receiver_config(receive_dir, port)] {
        return lan::receive_single_file(config);
    });

    wait_for_receiver_to_listen();

    auto socket = lan::connect_tcp("127.0.0.1", port);
    if (!expect(static_cast<bool>(socket), socket ? "" : socket.error().message)) {
        return false;
    }

    lan::Frame hello;
    hello.type = lan::MessageType::hello;
    hello.body = lan::encode_hello(lan::HelloMetadata{.mode = lan::HelloMode::file, .sender_id = {}});
    auto hello_written = lan::write_frame(socket.value(), hello);
    if (!expect(static_cast<bool>(hello_written), hello_written ? "" : hello_written.error().message)) {
        return false;
    }

    lan::Frame begin;
    begin.type = lan::MessageType::file_begin;
    begin.body = lan::bytes_from_string("name=offset.txt\nsize=3\nsha256=00\n");
    auto begin_written = lan::write_frame(socket.value(), begin);
    if (!expect(static_cast<bool>(begin_written), begin_written ? "" : begin_written.error().message)) {
        return false;
    }

    auto ack = lan::read_frame(socket.value());
    if (!expect(static_cast<bool>(ack), ack ? "" : ack.error().message)) {
        return false;
    }
    if (!expect(ack.value().type == lan::MessageType::ack, "receiver did not ack file_begin")) {
        return false;
    }

    lan::Frame chunk;
    chunk.type = lan::MessageType::chunk;
    chunk.body = chunk_body(1, "abc");
    auto chunk_written = lan::write_frame(socket.value(), chunk);
    if (!expect(static_cast<bool>(chunk_written), chunk_written ? "" : chunk_written.error().message)) {
        return false;
    }

    auto error = lan::read_frame(socket.value());
    if (!expect(static_cast<bool>(error), error ? "" : error.error().message)) {
        return false;
    }
    if (!expect(error.value().type == lan::MessageType::error, "receiver did not reject invalid offset")) {
        return false;
    }

    auto received = receiver.get();
    if (!expect(!received, "receiver unexpectedly accepted an invalid chunk offset")) {
        return false;
    }

    return expect(!std::filesystem::exists(receive_dir / "offset.txt"), "final file was created") &&
           expect(!std::filesystem::exists(receive_dir / "offset.txt.part"), "part file was left behind");
}

bool run_test(const std::string& name, bool (*test)(const TestContext&), const TestContext& context) {
    std::cout << "running " << name << '\n';
    const bool passed = test(context);
    std::cout << (passed ? "  passed\n" : "  failed\n");
    return passed;
}

}  // namespace

int main() {
    TestContext context;
    context.root = std::filesystem::temp_directory_path() /
                   ("lan-tests-" + std::to_string(::getpid()));

    std::error_code ec;
    std::filesystem::remove_all(context.root, ec);
    std::filesystem::create_directories(context.root);

    int failures = 0;
    if (!run_test("single_file_transfer", test_single_file_transfer, context)) {
        ++failures;
    }
    if (!run_test("partial_file_cleanup_after_disconnect", test_partial_file_cleanup_after_disconnect, context)) {
        ++failures;
    }
    if (!run_test("invalid_chunk_offset_is_rejected", test_invalid_chunk_offset_is_rejected, context)) {
        ++failures;
    }

    std::filesystem::remove_all(context.root, ec);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
