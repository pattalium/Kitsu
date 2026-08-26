#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

# One fail-closed production transaction for the unlock release:
#   public site + backend/migration binaries + private 21-pack directory +
#   route-scoped nginx browser policy.
#
# Digest mode is read-only and may run off-host:
#   deploy_public_unlock_atomic.sh --digest PUBLIC BACKEND PACKS POLICY
#
# Deployment mode must run as root on the production host:
#   deploy_public_unlock_atomic.sh \
#     PUBLIC BACKEND PACKS POLICY \
#     EXPECTED_PUBLIC_CURRENT EXPECTED_BACKEND_CURRENT \
#     EXPECTED_BACKEND_CANDIDATE RELEASE_ID SOURCE_SHA256 \
#     EXPECTED_NGINX_SHA256
#
# BACKEND is a prepared payload containing exactly:
#   bin/kitsu-platform-backend
#   bin/kitsu-platform-migrate
#   share/0011_pet_pack_unlocks.sql
#
# PACKS may contain non-pack QA evidence, but its direct .k868 files must be
# exactly the 21 catalogue filenames. Only those files enter protected runtime
# storage. No pack byte is ever copied beneath an nginx root.

die() {
  printf 'KITSU_PUBLIC_UNLOCK_DEPLOY_FAILED reason=%s\n' "$*" >&2
  exit 1
}

public_tree_digest() {
  python3 - "$1" <<'PY'
from hashlib import sha256
from pathlib import Path
import re
import stat
import sys

root = Path(sys.argv[1])
if not root.is_absolute() or not root.is_dir() or root.is_symlink():
    raise SystemExit("public_source_must_be_an_absolute_real_directory")

required = {
    "index.html", "styles.css", "theme.js", "site.js",
    "demo/index.html", "unlock/index.html", "unlock/unlock.css",
    "unlock/unlock.js", "unlock/catalog.js", "privacy/index.html",
    "terms/index.html", "security/index.html", "contact/index.html",
    "downloads/latest.json", "downloads/latest.json.sig",
    "downloads/update-ed25519-public.pem",
}
for relative in required:
    candidate = root / relative
    if not candidate.is_file() or candidate.is_symlink():
        raise SystemExit(f"public_required_file_missing:{relative}")

forbidden_suffixes = {".aab", ".idsig", ".jks", ".keystore", ".p12", ".pfx"}
secret_name = re.compile(
    r"(^|[-_.])(private|secret|password|credential|upload[-_]?key|signing[-_]?key)($|[-_.])"
)
private_key_header = re.compile(br"BEGIN (?:ENCRYPTED |RSA |EC |OPENSSH )?PRIVATE KEY")
records = []
for candidate in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
    relative = candidate.relative_to(root).as_posix()
    mode = candidate.lstat().st_mode
    if stat.S_ISLNK(mode):
        raise SystemExit(f"public_symlink_forbidden:{relative}")
    if stat.S_ISDIR(mode):
        if candidate.name == ".git":
            raise SystemExit(f"public_repository_metadata_forbidden:{relative}")
        continue
    if not stat.S_ISREG(mode):
        raise SystemExit(f"public_special_entry_forbidden:{relative}")
    lowered = relative.lower()
    if lowered.startswith("unlock/") and candidate.suffix.lower() == ".k868":
        raise SystemExit(f"private_unlock_pack_forbidden:{relative}")
    if candidate.suffix.lower() in forbidden_suffixes or secret_name.search(lowered):
        raise SystemExit(f"public_sensitive_artifact_forbidden:{relative}")
    content = candidate.read_bytes()
    if private_key_header.search(content):
        raise SystemExit(f"public_private_key_forbidden:{relative}")
    records.append((relative, len(content), sha256(content).hexdigest()))

tree = sha256()
for relative, size, digest in records:
    tree.update(relative.encode("utf-8"))
    tree.update(b"\0")
    tree.update(str(size).encode("ascii"))
    tree.update(b"\0")
    tree.update(digest.encode("ascii"))
    tree.update(b"\n")
print(tree.hexdigest())
PY
}

backend_tree_digest() {
  python3 - "$1" <<'PY'
from hashlib import sha256
from pathlib import Path
import re
import stat
import sys

root = Path(sys.argv[1])
if not root.is_absolute() or not root.is_dir() or root.is_symlink():
    raise SystemExit("backend_source_must_be_an_absolute_real_directory")

required = {
    "bin/kitsu-platform-backend",
    "bin/kitsu-platform-migrate",
    "share/0011_pet_pack_unlocks.sql",
}
records = []
seen = set()
private_key_header = re.compile(br"BEGIN (?:ENCRYPTED |RSA |EC |OPENSSH )?PRIVATE KEY")
for candidate in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
    relative = candidate.relative_to(root).as_posix()
    mode = candidate.lstat().st_mode
    if stat.S_ISLNK(mode):
        raise SystemExit(f"backend_symlink_forbidden:{relative}")
    if stat.S_ISDIR(mode):
        if relative not in {"bin", "share"}:
            raise SystemExit(f"backend_unexpected_directory:{relative}")
        continue
    if not stat.S_ISREG(mode):
        raise SystemExit(f"backend_special_entry_forbidden:{relative}")
    if relative not in required:
        raise SystemExit(f"backend_unexpected_file:{relative}")
    content = candidate.read_bytes()
    if not content:
        raise SystemExit(f"backend_empty_file:{relative}")
    if private_key_header.search(content):
        raise SystemExit(f"backend_private_key_forbidden:{relative}")
    if relative.startswith("bin/") and not content.startswith(b"\x7fELF"):
        raise SystemExit(f"backend_binary_not_elf:{relative}")
    if relative.endswith(".sql") and b"CREATE TABLE pet_pack_unlocks" not in content:
        raise SystemExit("backend_migration_0011_contract_missing")
    seen.add(relative)
    records.append((relative, len(content), sha256(content).hexdigest()))

if seen != required:
    missing = ",".join(sorted(required - seen))
    raise SystemExit(f"backend_required_file_missing:{missing}")

tree = sha256()
for relative, size, digest in records:
    tree.update(relative.encode("utf-8"))
    tree.update(b"\0")
    tree.update(str(size).encode("ascii"))
    tree.update(b"\0")
    tree.update(digest.encode("ascii"))
    tree.update(b"\n")
print(tree.hexdigest())
PY
}

pack_tree_digest() {
  python3 - "$1" <<'PY'
from hashlib import sha256
from pathlib import Path
import stat
import struct
import sys
import zlib

root = Path(sys.argv[1])
if not root.is_absolute() or not root.is_dir() or root.is_symlink():
    raise SystemExit("pack_source_must_be_an_absolute_real_directory")

catalog = {
    "frog.k868": 0x5CAC86A3,
    "hamster.k868": 0x13793DC7,
    "turtle.k868": 0x7495DBFB,
    "rabbit.k868": 0x68D9554E,
    "hedgehog.k868": 0x5DF6BE74,
    "ferret.k868": 0xE59408E0,
    "otter.k868": 0x29B4B2F7,
    "axolotl.k868": 0x69276D0C,
    "chinchilla.k868": 0x2DFB0797,
    "raccoon.k868": 0xC163EFED,
    "capybara.k868": 0x374D2540,
    "sugar_glider.k868": 0x39FC5B1A,
    "red_panda.k868": 0x91A2DE7B,
    "pangolin.k868": 0xE04EC405,
    "tasmanian_devil.k868": 0x8E0E1B03,
    "snow_leopard.k868": 0x533B9B30,
    "okapi.k868": 0x86F3BB5D,
    "shoebill.k868": 0x2D1D89AF,
    "cat_girl.k868": 0xA52160C5,
    "rabbit_girl.k868": 0xF0F750BD,
    "deer_girl.k868": 0x52A1C03A,
}
expected_bytes = 64 + (12 * 12) + (48 * 4) + (48 * 512)
present = {
    path.relative_to(root).as_posix()
    for path in root.rglob("*.k868")
}
if present != set(catalog):
    missing = ",".join(sorted(set(catalog) - present))
    extra = ",".join(sorted(present - set(catalog)))
    raise SystemExit(f"pack_catalog_mismatch:missing={missing}:extra={extra}")

records = []
for filename, expected_id in sorted(catalog.items()):
    path = root / filename
    mode = path.lstat().st_mode
    if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
        raise SystemExit(f"pack_not_regular:{filename}")
    data = path.read_bytes()
    if len(data) != expected_bytes or data[:8] != b"K868PK1\0":
        raise SystemExit(f"pack_header_invalid:{filename}")
    version, header_bytes = struct.unpack_from("<HH", data, 8)
    total_bytes, payload_crc, header_crc, pack_id, revision = struct.unpack_from(
        "<IIIII", data, 12
    )
    width, height, frame_count, clip_count = struct.unpack_from("<HHHH", data, 32)
    step_count, flags = struct.unpack_from("<II", data, 40)
    if (
        version != 1
        or header_bytes != 64
        or total_bytes != len(data)
        or pack_id != expected_id
        or revision == 0
        or width != 64
        or height != 64
        or frame_count != 48
        or clip_count != 12
        or step_count != 48
        or flags != 0
    ):
        raise SystemExit(f"pack_structure_invalid:{filename}")
    display = data[48:64]
    terminator = display.find(b"\0")
    if (
        terminator <= 0
        or any(byte < 0x20 or byte > 0x7E for byte in display[:terminator])
        or any(display[terminator:])
    ):
        raise SystemExit(f"pack_display_name_invalid:{filename}")
    if zlib.crc32(data[64:]) & 0xFFFFFFFF != payload_crc:
        raise SystemExit(f"pack_payload_crc_invalid:{filename}")
    header = bytearray(data[8:64])
    header[12:16] = b"\0\0\0\0"
    if zlib.crc32(header) & 0xFFFFFFFF != header_crc:
        raise SystemExit(f"pack_header_crc_invalid:{filename}")
    records.append((filename, len(data), sha256(data).hexdigest()))

tree = sha256()
for relative, size, digest in records:
    tree.update(relative.encode("utf-8"))
    tree.update(b"\0")
    tree.update(str(size).encode("ascii"))
    tree.update(b"\0")
    tree.update(digest.encode("ascii"))
    tree.update(b"\n")
print(tree.hexdigest())
PY
}

policy_file_digest() {
  python3 - "$1" <<'PY'
from hashlib import sha256
from pathlib import Path
import re
import sys

path = Path(sys.argv[1])
if not path.is_absolute() or not path.is_file() or path.is_symlink():
    raise SystemExit("nginx_policy_source_must_be_an_absolute_regular_file")
data = path.read_bytes()
try:
    text = data.decode("utf-8")
except UnicodeDecodeError as error:
    raise SystemExit(f"nginx_policy_not_utf8:{error}")

for variable in (
    "$kitsu_public_site_csp",
    "$kitsu_public_site_permissions_policy",
):
    if text.count(f"map $request_uri {variable}") != 1:
        raise SystemExit(f"nginx_policy_map_invalid:{variable}")
if text.count("~^/unlock(?:/|$)") != 2:
    raise SystemExit("nginx_policy_unlock_scope_invalid")
if "connect-src 'self' https://api.k32.run" not in text:
    raise SystemExit("nginx_policy_unlock_api_missing")
if text.count("serial=()") != 1 or text.count("serial=(self)") != 1:
    raise SystemExit("nginx_policy_serial_scope_invalid")
if re.search(r"\balias\b|\broot\b|\.k868|pet-packs", text, re.IGNORECASE):
    raise SystemExit("nginx_policy_private_storage_directive_forbidden")
print(sha256(data).hexdigest())
PY
}

combine_component_digests() {
  python3 - "$1" "$2" "$3" "$4" <<'PY'
from hashlib import sha256
import sys

digest = sha256()
for label, value in zip(("public", "backend", "packs", "policy"), sys.argv[1:]):
    digest.update(label.encode("ascii"))
    digest.update(b"\0")
    digest.update(value.encode("ascii"))
    digest.update(b"\n")
print(digest.hexdigest())
PY
}

deployment_digest() {
  local public_digest backend_digest pack_digest policy_digest
  public_digest=$(public_tree_digest "$1")
  backend_digest=$(backend_tree_digest "$2")
  pack_digest=$(pack_tree_digest "$3")
  policy_digest=$(policy_file_digest "$4")
  combine_component_digests \
    "$public_digest" "$backend_digest" "$pack_digest" "$policy_digest"
}

if [[ "${1:-}" == '--digest' ]]; then
  [[ $# == 5 ]] || die \
    'usage: deploy_public_unlock_atomic.sh --digest PUBLIC BACKEND PACKS POLICY'
  deployment_digest "$2" "$3" "$4" "$5"
  exit 0
fi

[[ $# == 10 ]] || die \
  'usage: deploy_public_unlock_atomic.sh PUBLIC BACKEND PACKS POLICY EXPECTED_PUBLIC_CURRENT EXPECTED_BACKEND_CURRENT EXPECTED_BACKEND_CANDIDATE RELEASE_ID SOURCE_SHA256 EXPECTED_NGINX_SHA256'
[[ "$(id -u)" == '0' ]] || die 'root_required'

public_source=$1
backend_source=$2
pack_source=$3
policy_source=$4
expected_public_current=$5
expected_backend_current=$6
expected_backend_candidate=$7
release_id=$8
expected_digest=$9
expected_nginx_digest=${10}

public_release_root='/srv/k32/public/releases'
public_current='/srv/k32/public/current'
current=$public_current
public_lock='/srv/k32/public/.kitsu-production-promotion.lock'
backend_release_root='/opt/kitsu/releases'
backend_current='/opt/kitsu/current'
backend_candidate='/opt/kitsu/candidate'
backend_lock='/run/lock/kitsu-platform-deploy.lock'
static_lock='/run/lock/kitsu-static-deploy.lock'
nginx_lock='/run/lock/kitsu-nginx-config.lock'
pack_release_root='/var/lib/kitsu-backend/pet-packs/releases'
nginx_available='/etc/nginx/sites-available/k32-sites.conf'
nginx_enabled='/etc/nginx/sites-enabled/k32-sites.conf'
nginx_policy='/etc/nginx/conf.d/20-k32-unlock-policy-map.conf'
backend_dropin_dir='/etc/systemd/system/kitsu-backend.service.d'
backend_dropin="$backend_dropin_dir/40-pet-pack-release.conf"
nginx_available_next="/etc/nginx/sites-available/.k32-sites.conf.$release_id.next"
nginx_enabled_next="/etc/nginx/sites-enabled/.k32-sites.conf.$release_id.next"
nginx_policy_next="/etc/nginx/conf.d/.20-k32-unlock-policy-map.$release_id.next"
backend_dropin_next="$backend_dropin_dir/.40-pet-pack-release.$release_id.next"

public_target="$public_release_root/$release_id"
public_staging="$public_release_root/.$release_id.tmp"
backend_target="$backend_release_root/kitsu-platform-$release_id"
backend_staging="$backend_release_root/.kitsu-platform-$release_id.tmp"
pack_target="$pack_release_root/$release_id"
pack_staging="$pack_release_root/.$release_id.tmp"

next_link="/srv/k32/public/.current-$release_id.next"
backend_current_next="/opt/kitsu/.current-$release_id.next"
backend_candidate_next="/opt/kitsu/.candidate-$release_id.next"

public_origin="${KITSU_PUBLIC_ORIGIN:-http://127.0.0.1:8100}"
api_origin="${KITSU_API_ORIGIN:-http://127.0.0.1:8102}"

probe_dir=''
backup_dir=''
public_switched=0
backend_current_switched=0
backend_candidate_switched=0
backend_dropin_changed=0
backend_committed=0
nginx_changed=0
old_policy_exists=0
old_dropin_exists=0
public_staging_created=0
backend_staging_created=0
pack_staging_created=0
public_next_created=0
backend_current_next_created=0
backend_candidate_next_created=0
nginx_available_next_created=0
nginx_enabled_next_created=0
nginx_policy_next_created=0
backend_dropin_next_created=0

for command in chown chmod cmp cp curl cut dirname find flock getent grep install \
  ln mktemp mv openssl python3 readlink rm sha256sum sleep sync systemctl; do
  command -v "$command" >/dev/null || die "missing_command:$command"
done
[[ -x /usr/sbin/nginx ]] || die 'nginx_missing'

[[ "$public_source" == /* && -d "$public_source" && ! -L "$public_source" ]] \
  || die 'invalid_public_source_directory'
[[ "$backend_source" == /* && -d "$backend_source" && ! -L "$backend_source" ]] \
  || die 'invalid_backend_source_directory'
[[ "$pack_source" == /* && -d "$pack_source" && ! -L "$pack_source" ]] \
  || die 'invalid_pack_source_directory'
[[ "$policy_source" == /* && -f "$policy_source" && ! -L "$policy_source" ]] \
  || die 'invalid_policy_source_file'
[[ "$expected_public_current" == "$public_release_root/"* ]] \
  || die 'expected_public_current_outside_release_root'
[[ "$expected_backend_current" == "$backend_release_root/"* ]] \
  || die 'expected_backend_current_outside_release_root'
[[ "$expected_backend_candidate" == "$backend_release_root/"* ]] \
  || die 'expected_backend_candidate_outside_release_root'
[[ "$release_id" =~ ^[0-9]{8}T[0-9]{6}Z-[a-z0-9][a-z0-9.-]{0,63}-public-unlock$ ]] \
  || die 'invalid_release_id'
[[ "$expected_digest" =~ ^[0-9a-f]{64}$ ]] || die 'invalid_source_digest'
[[ "$expected_nginx_digest" =~ ^[0-9a-f]{64}$ ]] || die 'invalid_nginx_digest'

public_source=$(readlink -f -- "$public_source")
backend_source=$(readlink -f -- "$backend_source")
pack_source=$(readlink -f -- "$pack_source")
policy_source=$(readlink -f -- "$policy_source")
expected_public_current=$(readlink -f -- "$expected_public_current")
expected_backend_current=$(readlink -f -- "$expected_backend_current")
expected_backend_candidate=$(readlink -f -- "$expected_backend_candidate")

[[ "$expected_public_current" == "$public_release_root/"* && -d "$expected_public_current" ]] \
  || die 'invalid_expected_public_current'
[[ "$expected_backend_current" == "$backend_release_root/"* && -d "$expected_backend_current" ]] \
  || die 'invalid_expected_backend_current'
[[ "$expected_backend_candidate" == "$backend_release_root/"* && -d "$expected_backend_candidate" ]] \
  || die 'invalid_expected_backend_candidate'

restore_link() {
  local link=$1
  local target=$2
  local temporary=$3
  rm -f -- "$temporary"
  ln -s -- "$target" "$temporary" || return 1
  mv -Tf -- "$temporary" "$link" || return 1
  [[ "$(readlink -f -- "$link")" == "$target" ]] || return 1
  sync -f -- "$(dirname "$link")"
}

restore_file_atomic() {
  local source=$1
  local target=$2
  local temporary=$3
  rm -f -- "$temporary"
  install -o root -g root -m 0644 -- "$source" "$temporary" || return 1
  mv -Tf -- "$temporary" "$target" || return 1
  sync -f -- "$(dirname "$target")"
}

backend_ready() {
  curl --fail --silent --show-error --max-time 10 \
    --header 'Host: api.k32.run' "$api_origin/health/live" >/dev/null
}

rollback() {
  local status=$?
  local rollback_status=0
  local reload_nginx=0
  local restart_backend=0
  trap - ERR EXIT
  set +e

  if (( public_switched )); then
    restore_link "$public_current" "$expected_public_current" "$next_link" \
      || rollback_status=98
    reload_nginx=1
  fi

  if (( nginx_changed )); then
    restore_file_atomic "$backup_dir/k32-sites.available.conf" \
      "$nginx_available" "$nginx_available_next" \
      || rollback_status=98
    restore_file_atomic "$backup_dir/k32-sites.enabled.conf" \
      "$nginx_enabled" "$nginx_enabled_next" \
      || rollback_status=98
    if (( old_policy_exists )); then
      restore_file_atomic "$backup_dir/k32-unlock-policy-map.conf" \
        "$nginx_policy" "$nginx_policy_next" \
        || rollback_status=98
    else
      rm -f -- "$nginx_policy" || rollback_status=98
    fi
    reload_nginx=1
  fi

  if (( backend_current_switched && ! backend_committed )); then
    restore_link "$backend_current" "$expected_backend_current" \
      "$backend_current_next" || rollback_status=98
    restart_backend=1
  fi
  if (( backend_candidate_switched && ! backend_committed )); then
    restore_link "$backend_candidate" "$expected_backend_candidate" \
      "$backend_candidate_next" || rollback_status=98
  fi
  if (( backend_dropin_changed && ! backend_committed )); then
    if (( old_dropin_exists )); then
      restore_file_atomic "$backup_dir/40-pet-pack-release.conf" \
        "$backend_dropin" "$backend_dropin_next" \
        || rollback_status=98
    else
      rm -f -- "$backend_dropin" || rollback_status=98
    fi
    systemctl daemon-reload || rollback_status=98
    restart_backend=1
  fi

  if (( restart_backend )); then
    systemctl restart kitsu-backend.service || rollback_status=98
    local old_ready=0
    for _ in {1..30}; do
      if backend_ready; then
        old_ready=1
        break
      fi
      sleep 0.5
    done
    (( old_ready == 1 )) || rollback_status=98
  fi

  if (( reload_nginx )); then
    /usr/sbin/nginx -t >/dev/null 2>&1 \
      && systemctl reload nginx \
      && [[ "$(systemctl is-active nginx)" == 'active' ]] \
      || rollback_status=98
  fi

  [[ -n "$probe_dir" ]] && rm -rf -- "$probe_dir"
  (( public_staging_created )) && [[ -e "$public_staging" ]] \
    && rm -rf -- "$public_staging"
  (( backend_staging_created )) && [[ -e "$backend_staging" ]] \
    && rm -rf -- "$backend_staging"
  (( pack_staging_created )) && [[ -e "$pack_staging" ]] \
    && rm -rf -- "$pack_staging"

  if (( rollback_status == 0 )); then
    (( public_next_created )) && rm -f -- "$next_link"
    (( backend_current_next_created )) && rm -f -- "$backend_current_next"
    (( backend_candidate_next_created )) && rm -f -- "$backend_candidate_next"
    (( nginx_available_next_created )) && rm -f -- "$nginx_available_next"
    (( nginx_enabled_next_created )) && rm -f -- "$nginx_enabled_next"
    (( nginx_policy_next_created )) && rm -f -- "$nginx_policy_next"
    (( backend_dropin_next_created )) && rm -f -- "$backend_dropin_next"
    [[ -n "$backup_dir" ]] && rm -rf -- "$backup_dir"
    printf 'KITSU_PUBLIC_UNLOCK_ROLLBACK_OK public=%s backend=%s\n' \
      "$expected_public_current" "$expected_backend_current" >&2
  else
    status=$rollback_status
    printf 'KITSU_PUBLIC_UNLOCK_ROLLBACK_INCOMPLETE public=%s backend=%s recovery_dir=%s\n' \
      "$expected_public_current" "$expected_backend_current" \
      "${backup_dir:-not-created}" >&2
  fi
  exit "$status"
}
trap rollback ERR EXIT

exec 9>"$public_lock"
flock -n 9 || die 'public_promotion_lock_busy'
exec 8>"$backend_lock"
flock -n 8 || die 'backend_promotion_lock_busy'
exec 7>"$static_lock"
flock -n 7 || die 'static_promotion_lock_busy'
exec 6>"$nginx_lock"
flock -n 6 || die 'nginx_promotion_lock_busy'

[[ -d "$public_release_root" && ! -L "$public_release_root" ]] \
  || die 'public_release_root_invalid'
[[ -d "$backend_release_root" && ! -L "$backend_release_root" ]] \
  || die 'backend_release_root_invalid'
[[ -d /var/lib/kitsu-backend && ! -L /var/lib/kitsu-backend ]] \
  || die 'backend_state_root_invalid'
getent group kitsu-backend >/dev/null || die 'backend_service_group_missing'
install -d -o root -g kitsu-backend -m 0750 -- \
  /var/lib/kitsu-backend/pet-packs "$pack_release_root"

[[ -L "$public_current" \
  && "$(readlink -f -- "$public_current")" == "$expected_public_current" ]] \
  || die 'unexpected_public_current_release'
[[ -L "$backend_current" \
  && "$(readlink -f -- "$backend_current")" == "$expected_backend_current" ]] \
  || die 'unexpected_backend_current_release'
[[ -L "$backend_candidate" \
  && "$(readlink -f -- "$backend_candidate")" == "$expected_backend_candidate" ]] \
  || die 'unexpected_backend_candidate_release'
systemctl show kitsu-backend.service --property=ExecStart --value \
  | grep -Fq '/opt/kitsu/current/bin/kitsu-platform-backend' \
  || die 'backend_unit_exec_path_changed'
systemctl show kitsu-backend-migrate.service --property=ExecStart --value \
  | grep -Fq '/opt/kitsu/current/bin/kitsu-platform-migrate' \
  || die 'migration_unit_exec_path_changed'
[[ "$(systemctl is-active kitsu-backend.service)" == 'active' ]] \
  || die 'backend_not_active_before_deploy'
[[ "$(systemctl is-active nginx)" == 'active' ]] \
  || die 'nginx_not_active_before_deploy'

for path in "$public_target" "$public_staging" "$backend_target" \
  "$backend_staging" "$pack_target" "$pack_staging" "$next_link" \
  "$backend_current_next" "$backend_candidate_next" \
  "$nginx_available_next" "$nginx_enabled_next" "$nginx_policy_next" \
  "$backend_dropin_next"; do
  [[ ! -e "$path" && ! -L "$path" ]] || die "release_path_exists:$path"
done

[[ -f "$nginx_available" && ! -L "$nginx_available" ]] \
  || die 'nginx_available_config_invalid'
[[ -f "$nginx_enabled" && ! -L "$nginx_enabled" ]] \
  || die 'nginx_enabled_config_invalid'
cmp -s -- "$nginx_available" "$nginx_enabled" \
  || die 'nginx_site_config_copies_differ'
[[ "$(sha256sum "$nginx_available" | cut -d' ' -f1)" == "$expected_nginx_digest" ]] \
  || die 'nginx_baseline_digest_mismatch'
[[ "$(sha256sum "$nginx_enabled" | cut -d' ' -f1)" == "$expected_nginx_digest" ]] \
  || die 'nginx_enabled_digest_mismatch'
[[ ! -e "$nginx_policy" || ( -f "$nginx_policy" && ! -L "$nginx_policy" ) ]] \
  || die 'existing_nginx_policy_invalid'
[[ ! -e "$backend_dropin" || ( -f "$backend_dropin" && ! -L "$backend_dropin" ) ]] \
  || die 'existing_backend_dropin_invalid'

public_source_digest=$(public_tree_digest "$public_source")
backend_source_digest=$(backend_tree_digest "$backend_source")
pack_source_digest=$(pack_tree_digest "$pack_source")
policy_source_digest=$(policy_file_digest "$policy_source")
actual_digest=$(combine_component_digests \
  "$public_source_digest" "$backend_source_digest" \
  "$pack_source_digest" "$policy_source_digest")
[[ "$actual_digest" == "$expected_digest" ]] \
  || die "source_digest_mismatch:$actual_digest"

openssl pkeyutl -verify -pubin \
  -inkey "$public_source/downloads/update-ed25519-public.pem" \
  -rawin -in "$public_source/downloads/latest.json" \
  -sigfile "$public_source/downloads/latest.json.sig" >/dev/null \
  || die 'android_manifest_signature_invalid'

python3 - "$public_source" <<'PY'
from hashlib import sha256
from pathlib import Path
import json
import sys

root = Path(sys.argv[1])
release = json.loads((root / "downloads/latest.json").read_text(encoding="utf-8"))
url = release.get("url")
if not isinstance(url, str) or not url.startswith("/downloads/") or not url.endswith(".apk"):
    raise SystemExit("manifest_apk_path_invalid")
apk = root / url.removeprefix("/")
if not apk.is_file() or apk.is_symlink():
    raise SystemExit("manifest_apk_missing")
content = apk.read_bytes()
if len(content) != release.get("bytes") or sha256(content).hexdigest() != release.get("sha256"):
    raise SystemExit("manifest_apk_integrity_mismatch")
PY

# Stage and re-hash every immutable release before changing any live pointer.
install -d -o root -g root -m 0755 -- "$public_staging"
public_staging_created=1
cp -a -- "$public_source/." "$public_staging/"
[[ "$(public_tree_digest "$public_staging")" == "$public_source_digest" ]] \
  || die 'staged_public_tree_mismatch'
chown -R root:root -- "$public_staging"
find "$public_staging" -type d -exec chmod 0555 {} +
find "$public_staging" -type f -exec chmod 0444 {} +
mv -- "$public_staging" "$public_target"
public_staging_created=0

install -d -o root -g root -m 0755 -- "$backend_staging"
backend_staging_created=1
cp -a -- "$backend_source/." "$backend_staging/"
[[ "$(backend_tree_digest "$backend_staging")" == "$backend_source_digest" ]] \
  || die 'staged_backend_tree_mismatch'
chown -R root:root -- "$backend_staging"
find "$backend_staging" -type d -exec chmod 0555 {} +
find "$backend_staging" -type f -exec chmod 0444 {} +
chmod 0555 -- \
  "$backend_staging/bin/kitsu-platform-backend" \
  "$backend_staging/bin/kitsu-platform-migrate"
mv -- "$backend_staging" "$backend_target"
backend_staging_created=0

install -d -o root -g kitsu-backend -m 0550 -- "$pack_staging"
pack_staging_created=1
for filename in frog hamster turtle rabbit hedgehog ferret otter axolotl \
  chinchilla raccoon capybara sugar_glider red_panda pangolin \
  tasmanian_devil snow_leopard okapi shoebill cat_girl rabbit_girl deer_girl; do
  install -o root -g kitsu-backend -m 0440 -- \
    "$pack_source/$filename.k868" "$pack_staging/$filename.k868"
done
[[ "$(pack_tree_digest "$pack_staging")" == "$pack_source_digest" ]] \
  || die 'staged_pack_tree_mismatch'
mv -- "$pack_staging" "$pack_target"
pack_staging_created=0
sync -f -- "$public_target" "$backend_target" "$pack_target"

install -d -o root -g root -m 0700 -- /var/lib/kitsu-deploy
backup_dir=$(mktemp -d '/var/lib/kitsu-deploy/unlock-release.XXXXXX')
cp -a -- "$nginx_available" "$backup_dir/k32-sites.available.conf"
cp -a -- "$nginx_enabled" "$backup_dir/k32-sites.enabled.conf"
if [[ -f "$nginx_policy" ]]; then
  cp -a -- "$nginx_policy" "$backup_dir/k32-unlock-policy-map.conf"
  old_policy_exists=1
fi
if [[ -f "$backend_dropin" ]]; then
  cp -a -- "$backend_dropin" "$backup_dir/40-pet-pack-release.conf"
  old_dropin_exists=1
fi

# Stage the candidate pointer first, then switch current immediately before the
# migration unit runs from /opt/kitsu/current. The running old backend keeps its
# open executable, and migration 11 is additive. Rollback restores each link to
# its independently captured pre-deployment target.
ln -s -- "$backend_target" "$backend_candidate_next"
backend_candidate_next_created=1
mv -Tf -- "$backend_candidate_next" "$backend_candidate"
backend_candidate_next_created=0
backend_candidate_switched=1
[[ "$(readlink -f -- "$backend_candidate")" == "$backend_target" ]] \
  || die 'backend_candidate_switch_mismatch'

install -d -o root -g root -m 0755 -- "$backend_dropin_dir"
dropin_staging="$backup_dir/40-pet-pack-release.next"
printf '[Service]\nEnvironment=KITSU_PET_PACK_DIR=%s\n' "$pack_target" \
  > "$dropin_staging"
install -o root -g root -m 0644 -- "$dropin_staging" "$backend_dropin_next"
backend_dropin_next_created=1
backend_dropin_changed=1
mv -Tf -- "$backend_dropin_next" "$backend_dropin"
backend_dropin_next_created=0
sync -f -- "$backend_dropin_dir"
systemctl daemon-reload
effective_backend_environment=$(
  systemctl show kitsu-backend.service --property=Environment --value
)
python3 - "$effective_backend_environment" "$pack_target" <<'PY'
import shlex
import sys

assignments = [
    item.removeprefix("KITSU_PET_PACK_DIR=")
    for item in shlex.split(sys.argv[1])
    if item.startswith("KITSU_PET_PACK_DIR=")
]
if assignments != [sys.argv[2]]:
    raise SystemExit(
        f"effective_pack_environment_mismatch:{assignments!r}:{sys.argv[2]}"
    )
PY

ln -s -- "$backend_target" "$backend_current_next"
backend_current_next_created=1
mv -Tf -- "$backend_current_next" "$backend_current"
backend_current_next_created=0
backend_current_switched=1
[[ "$(readlink -f -- "$backend_current")" == "$backend_target" ]] \
  || die 'backend_current_switch_mismatch'
sync -f -- /opt/kitsu

systemctl start kitsu-backend-migrate.service
[[ "$(systemctl show kitsu-backend-migrate.service --property=Result --value)" == 'success' ]] \
  || die 'backend_migration_result_failed'
[[ "$(systemctl show kitsu-backend-migrate.service --property=ExecMainStatus --value)" == '0' ]] \
  || die 'backend_migration_exit_failed'

systemctl restart kitsu-backend.service

backend_is_ready=0
for _ in {1..40}; do
  if backend_ready; then
    backend_is_ready=1
    break
  fi
  sleep 0.5
done
[[ "$backend_is_ready" == 1 ]] || die 'backend_health_not_ready'
[[ "$(systemctl is-active kitsu-backend.service)" == 'active' ]] \
  || die 'backend_not_active_after_restart'
backend_committed=1

probe_dir=$(mktemp -d '/tmp/kitsu-public-unlock.XXXXXX')
verify_rejection_has_no_pack() {
  local label=$1
  shift
  local headers="$probe_dir/$label.headers"
  local body="$probe_dir/$label.body"
  local status
  status=$(curl --silent --show-error --max-time 20 \
    --header 'Host: api.k32.run' \
    --dump-header "$headers" --output "$body" --write-out '%{http_code}' \
    "$@" "$api_origin/v1/pet-packs/redeem")
  [[ "$status" =~ ^[45][0-9][0-9]$ ]] || return 1
  ! grep -Eiq '^content-type:[[:space:]]*application/octet-stream' "$headers" \
    || return 1
  python3 - "$body" <<'PY'
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
if b"K868PK1\0" in data:
    raise SystemExit("rejected_redemption_returned_pack_bytes")
PY
}

verify_rejection_has_no_pack invalid-get
verify_rejection_has_no_pack invalid-post \
  --request POST \
  --header 'Origin: https://k32.run' \
  --header 'Content-Type: application/json' \
  --data '{}'

preflight_headers="$probe_dir/redemption-preflight.headers"
preflight_body="$probe_dir/redemption-preflight.body"
preflight_status=$(curl --silent --show-error --max-time 20 \
  --request OPTIONS \
  --header 'Host: api.k32.run' \
  --header 'Origin: https://k32.run' \
  --header 'Access-Control-Request-Method: POST' \
  --header 'Access-Control-Request-Headers: content-type' \
  --dump-header "$preflight_headers" --output "$preflight_body" \
  --write-out '%{http_code}' \
  "$api_origin/v1/pet-packs/redeem")
[[ "$preflight_status" =~ ^2[0-9][0-9]$ ]] \
  || die "unlock_preflight_status:$preflight_status"
python3 - "$preflight_headers" "$preflight_body" <<'PY'
from pathlib import Path
import sys

headers = Path(sys.argv[1]).read_text(encoding="latin-1").splitlines()
body = Path(sys.argv[2]).read_bytes()

def values(name):
    found = []
    for line in headers:
        key, separator, value = line.partition(":")
        if separator and key.strip().lower() == name:
            found.append(value.strip())
    return found

if values("access-control-allow-origin") != ["https://k32.run"]:
    raise SystemExit("unlock_preflight_origin_missing_or_duplicated")
methods = ",".join(values("access-control-allow-methods")).upper()
if "POST" not in {method.strip() for method in methods.split(",")}:
    raise SystemExit("unlock_preflight_post_missing")
if b"K868PK1\0" in body:
    raise SystemExit("unlock_preflight_returned_pack_bytes")
PY

verify_browser_policies() {
  local unlock_headers="$probe_dir/unlock.headers"
  local home_headers="$probe_dir/home.headers"
  curl --fail --silent --show-error --max-time 20 \
    --header 'Host: k32.run' --dump-header "$unlock_headers" \
    --output /dev/null "$public_origin/unlock/"
  curl --fail --silent --show-error --max-time 20 \
    --header 'Host: k32.run' --dump-header "$home_headers" \
    --output /dev/null "$public_origin/"
  python3 - "$unlock_headers" "$home_headers" <<'PY'
from pathlib import Path
import sys

def one_header(path, name):
    values = []
    for line in Path(path).read_text(encoding="latin-1").splitlines():
        key, separator, value = line.partition(":")
        if separator and key.strip().lower() == name:
            values.append(value.strip())
    if len(values) != 1:
        raise SystemExit(f"expected_one_{name}_header:{path}:{len(values)}")
    return values[0]

unlock_csp = one_header(sys.argv[1], "content-security-policy")
unlock_permissions = one_header(sys.argv[1], "permissions-policy")
home_csp = one_header(sys.argv[2], "content-security-policy")
home_permissions = one_header(sys.argv[2], "permissions-policy")

unlock_directives = {
    part.strip().split(" ", 1)[0]: part.strip()
    for part in unlock_csp.split(";")
    if part.strip()
}
home_directives = {
    part.strip().split(" ", 1)[0]: part.strip()
    for part in home_csp.split(";")
    if part.strip()
}
if unlock_directives.get("connect-src") != "connect-src 'self' https://api.k32.run":
    raise SystemExit("unlock_connect_src_policy_missing")
if "serial=(self)" not in unlock_permissions:
    raise SystemExit("unlock_web_serial_policy_missing")
if "connect-src" in home_directives:
    raise SystemExit("home_connect_src_policy_widened")
if "serial=()" not in home_permissions or "serial=(self)" in home_permissions:
    raise SystemExit("home_web_serial_policy_widened")
PY
}

transform_nginx_config() {
  python3 - "$1" "$2" "$3" "$4" <<'PY'
from pathlib import Path
import json
import re
import sys

source = Path(sys.argv[1])
policy = Path(sys.argv[2])
output = Path(sys.argv[3])
public_source = Path(sys.argv[4])
text = source.read_text(encoding="utf-8")
policy_text = policy.read_text(encoding="utf-8")

def map_default(variable):
    match = re.search(
        rf"map\s+\$request_uri\s+{re.escape(variable)}\s*\{{(.*?)\n\}}",
        policy_text,
        re.DOTALL,
    )
    if not match:
        raise SystemExit(f"policy_map_missing:{variable}")
    default = re.search(r'^\s*default\s+"([^"]+)";\s*$', match.group(1), re.MULTILINE)
    if not default:
        raise SystemExit(f"policy_default_missing:{variable}")
    return default.group(1)

defaults = {
    "Content-Security-Policy": (
        "$kitsu_public_site_csp",
        map_default("$kitsu_public_site_csp"),
    ),
    "Permissions-Policy": (
        "$kitsu_public_site_permissions_policy",
        map_default("$kitsu_public_site_permissions_policy"),
    ),
}

lines = text.splitlines(keepends=True)
blocks = []
depth = 0
start = None
start_depth = None
for index, line in enumerate(lines):
    structural = line.split("#", 1)[0]
    if start is None and re.match(r"^\s*server\s*\{", structural):
        start = index
        start_depth = depth
    depth += structural.count("{") - structural.count("}")
    if start is not None and depth == start_depth:
        blocks.append((start, index))
        start = None
        start_depth = None
if start is not None or depth != 0:
    raise SystemExit("nginx_server_block_parse_failed")

matches = []
for first, last in blocks:
    block = "".join(lines[first:last + 1])
    server_names = re.findall(
        r"^\s*server_name\s+([^;]+);",
        block,
        re.MULTILINE,
    )
    names = set(server_names[0].split()) if len(server_names) == 1 else set()
    if (
        {"k32.run", "www.k32.run"}.issubset(names)
        and re.search(r"^\s*root\s+/srv/k32/public/current\s*;",
                      block, re.MULTILINE)
    ):
        matches.append((first, last))
if len(matches) != 1:
    raise SystemExit(f"expected_one_public_server_block:{len(matches)}")

first, last = matches[0]
states = []
for header, (variable, default_value) in defaults.items():
    directive = re.compile(
        rf"^(\s*)add_header\s+{re.escape(header)}\s+(.+?)\s+always;\s*(?:\r?\n)?$"
    )
    found = []
    for index in range(first, last + 1):
        match = directive.match(lines[index])
        if match:
            found.append((index, match))
    if len(found) != 1:
        raise SystemExit(f"expected_one_{header}_directive:{len(found)}")
    index, match = found[0]
    value = match.group(2)
    if value == variable:
        states.append("mapped")
        continue
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    else:
        raise SystemExit(f"unexpected_{header}_directive")
    if value != default_value:
        raise SystemExit(f"{header}_baseline_differs_from_policy_default")
    newline = "\r\n" if lines[index].endswith("\r\n") else "\n"
    lines[index] = f"{match.group(1)}add_header {header} {variable} always;{newline}"
    states.append("fixed")

if states not in (["fixed", "fixed"], ["mapped", "mapped"]):
    raise SystemExit("nginx_policy_directives_in_mixed_state")

transformed = "".join(lines)
release = json.loads(
    (public_source / "downloads/latest.json").read_text(encoding="utf-8")
)
apk_uri = release.get("url")
if not isinstance(apk_uri, str) or not re.fullmatch(
    r"/downloads/[A-Za-z0-9][A-Za-z0-9._-]{0,160}\.apk", apk_uri
):
    raise SystemExit("nginx_release_apk_uri_invalid")
apk_path = public_source / apk_uri.removeprefix("/")
if not apk_path.is_file() or apk_path.is_symlink():
    raise SystemExit("nginx_release_apk_missing")
route = f"    location = {apk_uri} {{"
if route not in transformed:
    marker = "    location ^~ /downloads/ {"
    if transformed.count(marker) != 1:
        raise SystemExit("nginx_downloads_deny_marker_invalid")
    block = (
        f"    location = {apk_uri} {{\n"
        "        try_files $uri =404;\n"
        "        default_type application/vnd.android.package-archive;\n"
        "        expires -1;\n"
        "    }\n\n"
    )
    transformed = transformed.replace(marker, block + marker)
if transformed.count(route) != 1:
    raise SystemExit("nginx_release_apk_route_invalid")

output.write_text(transformed, encoding="utf-8", newline="")
PY
}

nginx_candidate="$backup_dir/k32-sites.next.conf"
transform_nginx_config \
  "$nginx_available" "$policy_source" "$nginx_candidate" "$public_source"
install -o root -g root -m 0644 -- "$policy_source" "$nginx_policy_next"
nginx_policy_next_created=1
install -o root -g root -m 0644 -- "$nginx_candidate" "$nginx_available_next"
nginx_available_next_created=1
install -o root -g root -m 0644 -- "$nginx_candidate" "$nginx_enabled_next"
nginx_enabled_next_created=1
nginx_changed=1
mv -Tf -- "$nginx_policy_next" "$nginx_policy"
nginx_policy_next_created=0
[[ "$(policy_file_digest "$nginx_policy")" == "$policy_source_digest" ]] \
  || die 'installed_nginx_policy_mismatch'
mv -Tf -- "$nginx_available_next" "$nginx_available"
nginx_available_next_created=0
mv -Tf -- "$nginx_enabled_next" "$nginx_enabled"
nginx_enabled_next_created=0
sync -f -- /etc/nginx
/usr/sbin/nginx -t
nginx_effective="$probe_dir/nginx-effective.conf"
/usr/sbin/nginx -T > "$nginx_effective" 2>&1
if grep -Eiq '/var/lib/kitsu-backend/pet-packs' "$nginx_effective"; then
  die 'nginx_private_pack_path_exposed'
fi
systemctl reload nginx
[[ "$(systemctl is-active nginx)" == 'active' ]] || die 'nginx_not_active'

# systemctl reload returns before all retiring workers have necessarily left.
# Require three consecutive mapped-policy responses before the public commit.
policy_consecutive=0
for _ in {1..20}; do
  if verify_browser_policies; then
    policy_consecutive=$((policy_consecutive + 1))
    if (( policy_consecutive >= 3 )); then
      break
    fi
  else
    policy_consecutive=0
  fi
  sleep 0.5
done
(( policy_consecutive >= 3 )) || die 'nginx_unlock_policy_not_serving'

# The public pointer is the final commit point.
/usr/sbin/nginx -t
ln -s -- "$public_target" "$next_link"
public_next_created=1
mv -Tf -- "$next_link" "$current"
public_next_created=0
public_switched=1
[[ "$(readlink -f -- "$public_current")" == "$public_target" ]] \
  || die 'atomic_public_switch_mismatch'
sync -f -- /srv/k32/public
systemctl reload nginx
[[ "$(systemctl is-active nginx)" == 'active' ]] || die 'nginx_not_active'

fetch_exact() {
  local mode=$1
  local request_path=$2
  local expected=$3
  local output
  output="$probe_dir/$mode-$(printf '%s' "$request_path" | sha256sum | cut -d' ' -f1)"
  if [[ "$mode" == 'origin' ]]; then
    curl --fail --silent --show-error --max-time 20 \
      --header 'Host: k32.run' "$public_origin$request_path" -o "$output" \
      || return 1
  else
    curl --fail --silent --show-error --max-time 30 \
      "https://k32.run$request_path" -o "$output" || return 1
  fi
  cmp -s -- "$expected" "$output"
}

public_is_ready=0
for _ in {1..20}; do
  if fetch_exact origin '/unlock/#code=K8-ABCDE-FGHJK-MNPQR' \
    "$public_target/unlock/index.html"; then
    public_is_ready=1
    break
  fi
  sleep 0.25
done
[[ "$public_is_ready" == 1 ]] || die 'origin_unlock_not_ready'
fetch_exact origin '/' "$public_target/index.html"
fetch_exact origin '/unlock/unlock.css' "$public_target/unlock/unlock.css"
fetch_exact origin '/unlock/unlock.js' "$public_target/unlock/unlock.js"
fetch_exact origin '/unlock/catalog.js' "$public_target/unlock/catalog.js"
fetch_exact origin '/downloads/latest.json' "$public_target/downloads/latest.json"
fetch_exact origin '/downloads/latest.json.sig' \
  "$public_target/downloads/latest.json.sig"
release_apk_path=$(python3 - "$public_target/downloads/latest.json" <<'PY'
import json
from pathlib import Path
import re
import sys

value = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")).get("url")
if not isinstance(value, str) or not re.fullmatch(
    r"/downloads/[A-Za-z0-9][A-Za-z0-9._-]{0,160}\.apk", value
):
    raise SystemExit("release_apk_probe_path_invalid")
print(value)
PY
)
fetch_exact origin "$release_apk_path" "$public_target$release_apk_path"
verify_browser_policies
fetch_exact public "/unlock/?release=$release_id#code=K8-ABCDE-FGHJK-MNPQR" \
  "$public_target/unlock/index.html"
fetch_exact public "$release_apk_path?release=$release_id" \
  "$public_target$release_apk_path"

[[ "$(readlink -f -- "$public_current")" == "$public_target" ]] \
  || die 'public_current_changed_after_checks'
[[ "$(readlink -f -- "$backend_current")" == "$backend_target" ]] \
  || die 'backend_current_changed_after_checks'
[[ "$(readlink -f -- "$backend_candidate")" == "$backend_target" ]] \
  || die 'backend_candidate_changed_after_checks'
[[ "$(public_tree_digest "$public_target")" == "$public_source_digest" ]] \
  || die 'public_target_changed_after_checks'
[[ "$(backend_tree_digest "$backend_target")" == "$backend_source_digest" ]] \
  || die 'backend_target_changed_after_checks'
[[ "$(pack_tree_digest "$pack_target")" == "$pack_source_digest" ]] \
  || die 'pack_target_changed_after_checks'
[[ "$(policy_file_digest "$nginx_policy")" == "$policy_source_digest" ]] \
  || die 'nginx_policy_changed_after_checks'

public_switched=0
backend_current_switched=0
backend_candidate_switched=0
backend_dropin_changed=0
nginx_changed=0
trap - ERR EXIT
rm -rf -- "$probe_dir" "$backup_dir"
printf 'KITSU_PUBLIC_UNLOCK_DEPLOY_OK public=%s backend=%s packs=%s source_sha256=%s\n' \
  "$public_target" "$backend_target" "$pack_target" "$expected_digest"
