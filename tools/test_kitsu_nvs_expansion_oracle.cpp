#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nvs.h"
#include "nvs_storage.hpp"
#include "partition.hpp"

void _esp_error_check_failed(esp_err_t result, const char* file, int line,
                             const char* function, const char* expression) {
  std::cerr << "ESP_ERROR_CHECK result=" << result << " file=" << file
            << " line=" << line << " function=" << function
            << " expression=" << expression << '\n';
  std::abort();
}

namespace {

constexpr size_t kSectorBytes = 4096U;
constexpr size_t kExpandedNvsBytes = 0x40000U;
constexpr size_t kExpandedNvsSectors = kExpandedNvsBytes / kSectorBytes;

class MemoryPartition final : public nvs::Partition {
 public:
  explicit MemoryPartition(std::vector<uint8_t> bytes)
      : bytes_(std::move(bytes)), original_(bytes_) {}

  const char* get_partition_name() override { return "nvs"; }

  esp_err_t read_raw(size_t offset, void* output, size_t size) override {
    return read(offset, output, size);
  }

  esp_err_t read(size_t offset, void* output, size_t size) override {
    if (offset > bytes_.size() || size > bytes_.size() - offset) {
      return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(output, bytes_.data() + offset, size);
    ++read_operations_;
    return ESP_OK;
  }

  esp_err_t write_raw(size_t offset, const void* input, size_t size) override {
    return write(offset, input, size);
  }

  esp_err_t write(size_t offset, const void* input, size_t size) override {
    if (offset > bytes_.size() || size > bytes_.size() - offset) {
      return ESP_ERR_INVALID_SIZE;
    }
    const auto* source = static_cast<const uint8_t*>(input);
    for (size_t index = 0; index < size; ++index) {
      if ((static_cast<uint8_t>(~bytes_[offset + index]) & source[index]) != 0U) {
        return ESP_ERR_FLASH_OP_FAIL;
      }
      bytes_[offset + index] &= source[index];
    }
    ++write_operations_;
    return ESP_OK;
  }

  esp_err_t erase_range(size_t offset, size_t size) override {
    if (offset % kSectorBytes != 0U || size % kSectorBytes != 0U ||
        offset > bytes_.size() || size > bytes_.size() - offset) {
      return ESP_ERR_INVALID_SIZE;
    }
    std::fill(bytes_.begin() + static_cast<ptrdiff_t>(offset),
              bytes_.begin() + static_cast<ptrdiff_t>(offset + size), 0xffU);
    ++erase_operations_;
    return ESP_OK;
  }

  uint32_t get_address() override { return 0U; }
  uint32_t get_size() override { return static_cast<uint32_t>(bytes_.size()); }

  size_t reads() const { return read_operations_; }
  size_t writes() const { return write_operations_; }
  size_t erases() const { return erase_operations_; }
  bool unchanged() const { return bytes_ == original_; }

 private:
  std::vector<uint8_t> bytes_;
  const std::vector<uint8_t> original_;
  size_t read_operations_ = 0U;
  size_t write_operations_ = 0U;
  size_t erase_operations_ = 0U;
};

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::runtime_error("cannot open expanded NVS image");
  const std::streamsize size = stream.tellg();
  if (size != static_cast<std::streamsize>(kExpandedNvsBytes)) {
    throw std::runtime_error("expanded NVS image is not exactly 0x40000 bytes");
  }
  stream.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
    throw std::runtime_error("cannot read expanded NVS image");
  }
  return bytes;
}

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("usage: oracle EXPANDED_NVS.bin");
    MemoryPartition partition(readFile(argv[1]));
    nvs::Storage storage(&partition);
    const esp_err_t result = storage.init(0U, kExpandedNvsSectors);
    require(result == ESP_OK, "pinned IDF Storage::init rejected expanded NVS");
    require(storage.isValid(), "pinned IDF Storage is not valid after init");
    require(partition.writes() == 0U, "Storage::init wrote expanded NVS");
    require(partition.erases() == 0U, "Storage::init erased expanded NVS");
    require(partition.unchanged(), "Storage::init mutated expanded NVS bytes");

    nvs_stats_t stats{};
    require(storage.fillStats(stats) == ESP_OK, "cannot obtain pinned IDF NVS stats");
    require(stats.total_entries == 8064U, "expanded NVS total-entry count changed");
    require(stats.used_entries + stats.free_entries == stats.total_entries,
            "expanded NVS entry accounting is inconsistent");
    require(stats.free_entries >= 4096U,
            "expanded NVS does not have the required recovery headroom");
    require(stats.namespace_count >= 6U,
            "expanded NVS is missing required application namespaces");

    nvs_opaque_iterator_t iterator{};
    iterator.type = NVS_TYPE_ANY;
    std::set<std::pair<std::string, std::string>> entries;
    if (storage.findEntry(&iterator, nullptr)) {
      do {
        entries.emplace(iterator.entry_info.namespace_name,
                        iterator.entry_info.key);
      } while (storage.nextEntry(&iterator));
    }
    const std::pair<const char*, const char*> required[] = {
        {"wisp868", "uid"}, {"wisp868", "name"},
        {"wisp868", "progress_v1"}, {"wisp868", "fun_v1"},
        {"wisp868", "advent_v1"}, {"wisp868", "signal_v2"},
        {"wisp868", "ble_act_v3"}, {"kitsu_sec", "slot_a"},
        {"kitsu_sec", "slot_b"}, {"kitsu_jrn", "slot_a"},
        {"kitsu_jrn", "slot_b"}, {"kitsu_brain", "brain_a"},
        {"kitsu_brain", "brain_b"}, {"kitsu_msg", "state2a"},
        {"nimble_bond", "local_irk_1"}, {"nimble_bond", "our_sec_1"},
        {"nimble_bond", "peer_sec_1"},
    };
    for (const auto& item : required) {
      require(entries.count({item.first, item.second}) == 1U,
              "critical frozen NVS item is not enumerable");
    }
    require(partition.writes() == 0U && partition.erases() == 0U &&
                partition.unchanged(),
            "read-only enumeration mutated expanded NVS");
    std::cout << "KITSU_NVS_IDF447_ORACLE_OK"
              << " total=" << stats.total_entries
              << " used=" << stats.used_entries
              << " free=" << stats.free_entries
              << " namespaces=" << stats.namespace_count
              << " iterable=" << entries.size()
              << " reads=" << partition.reads()
              << " writes=" << partition.writes()
              << " erases=" << partition.erases() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "KITSU_NVS_IDF447_ORACLE_ERROR " << error.what() << '\n';
    return 1;
  }
}
