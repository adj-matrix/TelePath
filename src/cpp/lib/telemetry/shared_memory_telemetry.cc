#include "telepath/telemetry/telemetry_sink.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace telepath {

namespace {

class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) : fd_(fd) {}
  FileDescriptor(const FileDescriptor &) = delete;
  auto operator=(const FileDescriptor &) -> FileDescriptor & = delete;
  FileDescriptor(FileDescriptor &&other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
  }
  auto operator=(FileDescriptor &&other) noexcept -> FileDescriptor & {
    if (this == &other) return *this;
    if (fd_ >= 0) close(fd_);
    fd_ = other.fd_;
    other.fd_ = -1;
    return *this;
  }
  ~FileDescriptor() {
    if (fd_ >= 0) close(fd_);
  }

  auto get() const -> int { return fd_; }

 private:
  int fd_{-1};
};

class MappedRegion {
 public:
  MappedRegion() = default;
  MappedRegion(void *address, std::size_t size) : address_(address), size_(size) {}
  MappedRegion(const MappedRegion &) = delete;
  auto operator=(const MappedRegion &) -> MappedRegion & = delete;
  MappedRegion(MappedRegion &&other) noexcept : address_(other.address_), size_(other.size_) {
    other.address_ = MAP_FAILED;
    other.size_ = 0;
  }
  auto operator=(MappedRegion &&other) noexcept -> MappedRegion & {
    if (this == &other) return *this;
    if (address_ != MAP_FAILED && address_ != nullptr) munmap(address_, size_);
    address_ = other.address_;
    size_ = other.size_;
    other.address_ = MAP_FAILED;
    other.size_ = 0;
    return *this;
  }
  ~MappedRegion() {
    if (address_ != MAP_FAILED && address_ != nullptr) munmap(address_, size_);
  }

  auto data() -> char * { return static_cast<char *>(address_); }
  auto data() const -> const char * { return static_cast<const char *>(address_); }

 private:
  void *address_{MAP_FAILED};
  std::size_t size_{0};
};

auto ErrnoMessage(const char *prefix, int error) -> std::string {
  return std::string(prefix) + ": " + std::strerror(error);
}

auto ValidateSharedMemoryName(const std::string &name) -> Status {
  if (name.empty()) return Status::InvalidArgument("telemetry shared-memory name must not be empty");
  if (name.size() < 2 || name.front() != '/') {
    return Status::InvalidArgument("telemetry shared-memory name must start with '/' and include a name");
  }
  if (name.find('/', 1) != std::string::npos) {
    return Status::InvalidArgument("telemetry shared-memory name must not contain nested '/' characters");
  }
  return Status::Ok();
}

auto BuildHeader(
  uint64_t sequence,
  uint64_t payload_size,
  uint64_t payload_capacity,
  uint16_t flags
) -> TelemetrySharedMemoryHeader {
  TelemetrySharedMemoryHeader header;
  header.magic = kTelemetrySharedMemoryMagic;
  header.version = kTelemetrySharedMemoryVersion;
  header.flags = flags;
  header.sequence = sequence;
  header.payload_size = payload_size;
  header.payload_capacity = payload_capacity;
  return header;
}

auto IsHeaderCompatible(const TelemetrySharedMemoryHeader &header) -> bool {
  return header.magic == kTelemetrySharedMemoryMagic && header.version == kTelemetrySharedMemoryVersion;
}

auto ReadPreviousSequence(int fd) -> uint64_t {
  TelemetrySharedMemoryHeader header;
  const ssize_t read_size = pread(fd, &header, sizeof(header), 0);
  if (read_size != static_cast<ssize_t>(sizeof(header))) return 0;
  if (!IsHeaderCompatible(header)) return 0;
  return header.sequence;
}

auto NextSequence(uint64_t previous) -> uint64_t {
  if (previous == std::numeric_limits<uint64_t>::max()) return 1;
  return previous + 1;
}

auto OpenSharedMemory(const std::string &name, int flags, mode_t mode) -> Result<FileDescriptor> {
  const int fd = shm_open(name.c_str(), flags, mode);
  if (fd < 0) {
    if (errno == ENOENT) return Status::NotFound(ErrnoMessage("telemetry shared memory not found", errno));
    return Status::IoError(ErrnoMessage("failed to open telemetry shared memory", errno));
  }
  return FileDescriptor(fd);
}

auto MapSharedMemory(int fd, std::size_t size, int protection) -> Result<MappedRegion> {
  void *address = mmap(nullptr, size, protection, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED) return Status::IoError(ErrnoMessage("failed to map telemetry shared memory", errno));
  return MappedRegion(address, size);
}

}  // namespace

auto WriteTelemetryExportSharedMemory(
  const std::string &name,
  std::size_t payload_capacity,
  const TelemetryExportSnapshot &snapshot
) -> Status {
  auto name_status = ValidateSharedMemoryName(name);
  if (!name_status.ok()) return name_status;
  if (payload_capacity == 0) return Status::InvalidArgument("telemetry shared-memory payload capacity must be greater than zero");
  if (payload_capacity > std::numeric_limits<std::size_t>::max() - sizeof(TelemetrySharedMemoryHeader)) return Status::ResourceExhausted("telemetry shared-memory payload capacity is too large");

  const std::string payload = SerializeTelemetryExportJson(snapshot);
  if (payload.size() > payload_capacity) return Status::ResourceExhausted("telemetry shared-memory payload capacity is too small");

  auto fd_result = OpenSharedMemory(name, O_CREAT | O_RDWR, 0600);
  if (!fd_result.ok()) return fd_result.status();
  FileDescriptor fd(std::move(fd_result.value()));

  const uint64_t sequence = NextSequence(ReadPreviousSequence(fd.get()));
  const std::size_t total_size = sizeof(TelemetrySharedMemoryHeader) + payload_capacity;
  if (ftruncate(fd.get(), static_cast<off_t>(total_size)) != 0) return Status::IoError(ErrnoMessage("failed to size telemetry shared memory", errno));

  auto mapping_result = MapSharedMemory(fd.get(), total_size, PROT_READ | PROT_WRITE);
  if (!mapping_result.ok()) return mapping_result.status();
  MappedRegion mapping(std::move(mapping_result.value()));

  const auto writing_header = BuildHeader(
    sequence,
    static_cast<uint64_t>(payload.size()),
    static_cast<uint64_t>(payload_capacity),
    0);
  std::memcpy(mapping.data(), &writing_header, sizeof(writing_header));

  char *payload_begin = mapping.data() + sizeof(TelemetrySharedMemoryHeader);
  std::memcpy(payload_begin, payload.data(), payload.size());
  if (payload.size() < payload_capacity) {
    std::memset(payload_begin + payload.size(), 0, payload_capacity - payload.size());
  }

  std::atomic_thread_fence(std::memory_order_release);
  const auto ready_header = BuildHeader(
    sequence,
    static_cast<uint64_t>(payload.size()),
    static_cast<uint64_t>(payload_capacity),
    kTelemetrySharedMemoryReady);
  std::memcpy(mapping.data(), &ready_header, sizeof(ready_header));
  return Status::Ok();
}

auto ReadTelemetryExportSharedMemory(const std::string &name) -> Result<std::string> {
  auto name_status = ValidateSharedMemoryName(name);
  if (!name_status.ok()) return name_status;

  auto fd_result = OpenSharedMemory(name, O_RDONLY, 0);
  if (!fd_result.ok()) return fd_result.status();
  FileDescriptor fd(std::move(fd_result.value()));

  struct stat stat_buffer {};
  if (fstat(fd.get(), &stat_buffer) != 0) return Status::IoError(ErrnoMessage("failed to stat telemetry shared memory", errno));
  if (stat_buffer.st_size < static_cast<off_t>(sizeof(TelemetrySharedMemoryHeader))) return Status::NotFound("telemetry shared memory is smaller than the header");

  const auto total_size = static_cast<std::size_t>(stat_buffer.st_size);
  auto mapping_result = MapSharedMemory(fd.get(), total_size, PROT_READ);
  if (!mapping_result.ok()) return mapping_result.status();
  MappedRegion mapping(std::move(mapping_result.value()));

  TelemetrySharedMemoryHeader header;
  std::memcpy(&header, mapping.data(), sizeof(header));
  if (!IsHeaderCompatible(header)) return Status::InvalidArgument("telemetry shared memory header is not compatible");
  if ((header.flags & kTelemetrySharedMemoryReady) == 0) return Status::Unavailable("telemetry shared memory snapshot is not ready");

  const std::size_t available_payload = total_size - sizeof(TelemetrySharedMemoryHeader);
  if (header.payload_capacity > available_payload || header.payload_size > header.payload_capacity) return Status::Internal("telemetry shared memory header has invalid payload bounds");

  std::atomic_thread_fence(std::memory_order_acquire);
  const char *payload_begin = mapping.data() + sizeof(TelemetrySharedMemoryHeader);
  return std::string(payload_begin, payload_begin + static_cast<std::size_t>(header.payload_size));
}

auto UnlinkTelemetryExportSharedMemory(const std::string &name) -> Status {
  auto name_status = ValidateSharedMemoryName(name);
  if (!name_status.ok()) return name_status;
  if (shm_unlink(name.c_str()) != 0) {
    if (errno == ENOENT) return Status::Ok();
    return Status::IoError(ErrnoMessage("failed to unlink telemetry shared memory", errno));
  }
  return Status::Ok();
}

}  // namespace telepath
