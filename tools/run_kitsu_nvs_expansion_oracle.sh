#!/usr/bin/env bash
set -euo pipefail

readonly EXPECTED_ARCHIVE_SHA256="ecb1124730742772364c2b7417dbfa7f55652407b0ab0c0899a1380b0598252b"
readonly EXPECTED_GCC_VERSION="15.2.0"
readonly EXPECTED_GCC_SHA256="b5f1b773a7c733738352000c92a077dc5852a1a2fc6d836b1e411be1e9ec5f88"
readonly EXPECTED_GXX_SHA256="e6718f7e0c7d057c3ff77b550c603da9bc4030e3ede3c053705acce1293dbe4d"
readonly EXPANDED_NVS_BYTES=262144

if [[ $# -ne 4 ]]; then
  echo "usage: $0 IDF447_SOURCE.tar.gz ORACLE.cpp EXPANDED_NVS.bin RESULT_DIR" >&2
  exit 2
fi

archive=$(realpath -- "$1")
harness=$(realpath -- "$2")
expanded=$(realpath -- "$3")
result_dir=$(realpath -m -- "$4")

for input in "$archive" "$harness" "$expanded"; do
  if [[ ! -f "$input" || -L "$input" ]]; then
    echo "oracle input is missing, non-regular, or a symlink: $input" >&2
    exit 1
  fi
done
if [[ -e "$result_dir" ]]; then
  echo "oracle result directory already exists: $result_dir" >&2
  exit 1
fi
mkdir -m 700 -- "$result_dir"

archive_sha=$(sha256sum -- "$archive" | cut -d' ' -f1)
if [[ "$archive_sha" != "$EXPECTED_ARCHIVE_SHA256" ]]; then
  echo "pinned IDF 4.4.7 source archive SHA-256 mismatch" >&2
  exit 1
fi
if [[ $(stat -c '%s' -- "$expanded") -ne $EXPANDED_NVS_BYTES ]]; then
  echo "expanded NVS input is not exactly 0x40000 bytes" >&2
  exit 1
fi
gcc_version=$(gcc -dumpfullversion -dumpversion)
gxx_version=$(g++ -dumpfullversion -dumpversion)
if [[ "$gcc_version" != "$EXPECTED_GCC_VERSION" ||
      "$gxx_version" != "$EXPECTED_GCC_VERSION" ]]; then
  echo "oracle compiler is not the reviewed GCC/G++ 15.2.0 toolchain" >&2
  exit 1
fi
gcc_path=$(readlink -f -- "$(command -v gcc)")
gxx_path=$(readlink -f -- "$(command -v g++)")
gcc_sha=$(sha256sum -- "$gcc_path" | cut -d' ' -f1)
gxx_sha=$(sha256sum -- "$gxx_path" | cut -d' ' -f1)
if [[ "$gcc_sha" != "$EXPECTED_GCC_SHA256" ||
      "$gxx_sha" != "$EXPECTED_GXX_SHA256" ]]; then
  echo "oracle compiler executable SHA-256 is not the reviewed toolchain" >&2
  exit 1
fi

readonly include_flags=(
  -Icomponents/nvs_flash/src
  -Icomponents/nvs_flash/include
  -Icomponents/nvs_flash/private_include
  -Icomponents/nvs_flash/test_nvs_host
  -Icomponents/esp_common/include
  -Icomponents/log/include
  -Icomponents/esp_rom/include
  -Icomponents/esp32/include
  -Icomponents/spi_flash/include
)
readonly nvs_sources=(
  components/nvs_flash/src/nvs_page.cpp
  components/nvs_flash/src/nvs_pagemanager.cpp
  components/nvs_flash/src/nvs_storage.cpp
  components/nvs_flash/src/nvs_item_hash_list.cpp
  components/nvs_flash/src/nvs_types.cpp
)

build_oracle() {
  local name=$1
  local build="$result_dir/$name"
  mkdir -m 700 -- "$build"
  tar -xzf "$archive" -C "$build"
  cp -- "$harness" "$build/oracle.cpp"
  (
    cd "$build"
    export LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=0
    gcc -std=c11 -fno-record-gcc-switches \
      "${include_flags[@]}" \
      -c components/esp_rom/linux/esp_rom_crc.c -o esp_rom_crc.o
    g++ -std=c++17 -Wall -Wextra -Wno-deprecated-declarations \
      -fpermissive -fno-record-gcc-switches \
      -ffile-prefix-map="$build"=/kitsu-idf447-oracle \
      -DNO_DEBUG_STORAGE "${include_flags[@]}" \
      oracle.cpp "${nvs_sources[@]}" esp_rom_crc.o \
      -Wl,--build-id=none -o oracle
    ./oracle "$expanded" > oracle-result.txt
  ) >"$build/build.log" 2>&1 || {
    cat -- "$build/build.log" >&2
    exit 1
  }
}

build_oracle build-a
build_oracle build-b
cmp -- "$result_dir/build-a/oracle" "$result_dir/build-b/oracle"
cmp -- "$result_dir/build-a/oracle-result.txt" \
  "$result_dir/build-b/oracle-result.txt"
cmp -- "$result_dir/build-a/build.log" "$result_dir/build-b/build.log"

oracle_line=$(cat -- "$result_dir/build-a/oracle-result.txt")
if [[ "$oracle_line" != KITSU_NVS_IDF447_ORACLE_OK\ * ]]; then
  echo "oracle did not emit its exact success record" >&2
  exit 1
fi
runner_sha=$(sha256sum -- "$(realpath -- "$0")" | cut -d' ' -f1)
harness_sha=$(sha256sum -- "$harness" | cut -d' ' -f1)
expanded_sha=$(sha256sum -- "$expanded" | cut -d' ' -f1)
binary_sha=$(sha256sum -- "$result_dir/build-a/oracle" | cut -d' ' -f1)
build_log_sha=$(sha256sum -- "$result_dir/build-a/build.log" | cut -d' ' -f1)

cp -- "$result_dir/build-a/oracle" "$result_dir/kitsu_nvs_idf447_oracle"
{
  printf '%s\n' \
    "KITSU_NVS_IDF447_RUNNER_OK runner_sha256=$runner_sha harness_sha256=$harness_sha archive_sha256=$archive_sha expanded_sha256=$expanded_sha binary_sha256=$binary_sha build_log_sha256=$build_log_sha gcc=$gcc_version gcc_sha256=$gcc_sha gxx=$gxx_version gxx_sha256=$gxx_sha builds=2 deterministic=true"
  printf '%s\n' "$oracle_line"
} > "$result_dir/oracle-evidence.txt"
chmod 600 "$result_dir/kitsu_nvs_idf447_oracle" \
  "$result_dir/oracle-evidence.txt"
cat -- "$result_dir/oracle-evidence.txt"
