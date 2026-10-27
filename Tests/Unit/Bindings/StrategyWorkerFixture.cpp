#include "StrategyProtocol.h"

#include <Bte/Core/Digest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#if defined(__unix__) || defined(__APPLE__)
[[nodiscard]] bte::core::Result<std::size_t>
readSome(const std::span<std::byte> bytes) {
  const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
  if (count < 0) {
    return bte::core::makeError(bte::core::ErrorCode::internal,
                                "fixture read failed");
  }
  return static_cast<std::size_t>(count);
}

[[nodiscard]] bte::core::Result<std::size_t>
writeSome(const std::span<const std::byte> bytes) {
  const auto count = ::write(3, bytes.data(), bytes.size());
  if (count < 0) {
    return bte::core::makeError(bte::core::ErrorCode::internal,
                                "fixture write failed");
  }
  return static_cast<std::size_t>(count);
}

void writeAll(const int descriptor, const std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) {
      return;
    }
    offset += static_cast<std::size_t>(count);
  }
}

[[nodiscard]] std::string argumentValue(const int argc, char **argv,
                                        const std::string_view prefix) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument.starts_with(prefix)) {
      return std::string{argument.substr(prefix.size())};
    }
  }
  return {};
}

[[nodiscard]] int runFixture(const int argc, char **argv) {
  const std::string_view mode = argc > 1 ? argv[1] : "";
  if (mode == "hang") {
    while (true) {
      ::pause();
    }
  }
  if (mode == "descendant") {
    const auto child = ::fork();
    if (child < 0) {
      return 10;
    }
    if (child == 0) {
      while (true) {
        ::pause();
      }
    }
    while (true) {
      ::pause();
    }
  }
  if (mode == "invalid-frame") {
    writeAll(3, "bad!");
    return 0;
  }

  std::string source;
  std::string history;
  bool sharedSourceIsReadOnly = false;
  bool sharedHistoryIsReadOnly = false;
  if (mode == "read-source" || mode == "read-history") {
    const auto lengthText = argumentValue(argc, argv, "--shared-data-length=");
    if (lengthText.empty()) {
      writeAll(STDERR_FILENO, "missing source length argument\n");
      return 11;
    }
    const auto length = static_cast<std::size_t>(std::stoull(lengthText));
    const auto flags = ::fcntl(4, F_GETFL);
    sharedSourceIsReadOnly = flags >= 0 && (flags & O_ACCMODE) == O_RDONLY;
    source.resize(length);
    const auto *mapping = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, 4, 0);
    if (mapping == MAP_FAILED) {
      writeAll(STDERR_FILENO, "failed mapping shared source descriptor\n");
      return 12;
    }
    std::memcpy(source.data(), mapping, length);
    static_cast<void>(::munmap(const_cast<void *>(mapping), length));
    if (mode == "read-history") {
      const auto historyLengthText =
          argumentValue(argc, argv, "--history-data-length=");
      if (historyLengthText.empty()) {
        writeAll(STDERR_FILENO, "missing history length argument\n");
        return 16;
      }
      const auto historyLength =
          static_cast<std::size_t>(std::stoull(historyLengthText));
      const auto historyFlags = ::fcntl(5, F_GETFL);
      sharedHistoryIsReadOnly =
          historyFlags >= 0 && (historyFlags & O_ACCMODE) == O_RDONLY;
      history.resize(historyLength);
      const auto *historyMapping =
          ::mmap(nullptr, historyLength, PROT_READ, MAP_SHARED, 5, 0);
      if (historyMapping == MAP_FAILED) {
        writeAll(STDERR_FILENO, "failed mapping shared history descriptor\n");
        return 17;
      }
      std::memcpy(history.data(), historyMapping, historyLength);
      static_cast<void>(
          ::munmap(const_cast<void *>(historyMapping), historyLength));
    }
  } else if (mode == "echo") {
    const auto byteCount =
        argc > 2 ? static_cast<std::size_t>(std::stoull(argv[2])) : 0;
    writeAll(STDOUT_FILENO, "stdout-first\n");
    const std::string stdoutBody(byteCount, 'o');
    writeAll(STDOUT_FILENO, stdoutBody);
    writeAll(STDOUT_FILENO, "\nstdout-last\n");
    writeAll(STDERR_FILENO, "stderr-first\n");
    const std::string stderrBody(byteCount, 'e');
    writeAll(STDERR_FILENO, stderrBody);
    writeAll(STDERR_FILENO, "\nstderr-last\n");
  }

  const auto incoming =
      bte::bindings::python::readMessage(readSome, "fixture-run", 0);
  if (!incoming.ok()) {
    writeAll(STDERR_FILENO, incoming.error().message);
    return 13;
  }
  const bool secretAbsent = std::getenv("BTE_TEST_PRIVATE_SECRET") == nullptr;
  const auto response = bte::bindings::python::ProtocolMessage{
      .protocolVersion = 1,
      .runId = "fixture-run",
      .sequence = 0,
      .messageType = "ready",
      .payload =
          nlohmann::json{
              {"secretAbsent", secretAbsent},
              {"sharedSourceIsReadOnly", sharedSourceIsReadOnly},
              {"sharedHistoryIsReadOnly", sharedHistoryIsReadOnly},
              {"history", history},
              {"historySha256Matches",
               mode == "read-history" &&
                   bte::core::sha256(history) ==
                       argumentValue(argc, argv, "--history-data-sha256=")},
              {"source", source},
              {"sourceSha256Matches",
               (mode == "read-source" || mode == "read-history") &&
                   bte::core::sha256(source) ==
                       argumentValue(argc, argv, "--shared-data-sha256=")},
          },
  };
  const auto sent = bte::bindings::python::writeMessage(writeSome, response);
  if (mode == "exit-output" && sent.ok()) {
    const auto shutdown =
        bte::bindings::python::readMessage(readSome, "fixture-run", 1);
    if (!shutdown.ok()) {
      return 15;
    }
    writeAll(STDOUT_FILENO, "exit-output\n");
  }
  return sent.ok() ? 0 : 14;
}
#endif

} // namespace

int main(const int argc, char **argv) {
#if defined(__unix__) || defined(__APPLE__)
  return runFixture(argc, argv);
#else
  static_cast<void>(argc);
  static_cast<void>(argv);
  return 0;
#endif
}
