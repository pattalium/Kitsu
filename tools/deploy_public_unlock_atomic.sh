#!/usr/bin/env bash
set -Eeuo pipefail

# Production plan:
#   1. Run this script with --digest against the exact public-site directory.
#   2. Transfer that directory and this script to the host without changing bytes.
#   3. Invoke deployment with the pinned current release and digest. The script
#      validates, stages, atomically switches, probes, and rolls back on failure.
# This file never copies to or changes a host unless deployment mode is invoked.

die() {
  printf 'KITSU_PUBLIC_UNLOCK_DEPLOY_FAILED reason=%s\n' "$*" >&2
  exit 1
}

tree_digest() {
  python3 - "$1" <<'PY'
from hashlib import sha256
from pathlib import Path
import os
import re
import stat
import sys

root = Path(sys.argv[1])
if not root.is_absolute() or not root.is_dir() or root.is_symlink():
    raise SystemExit("source_must_be_an_absolute_real_directory")

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
        raise SystemExit(f"required_file_missing:{relative}")

forbidden_suffixes = {".aab", ".idsig", ".jks", ".keystore", ".p12", ".pfx"}
secret_name = re.compile(r"(^|[-_.])(private|secret|password|credential|upload[-_]?key|signing[-_]?key)($|[-_.])")
private_key_header = re.compile(br"BEGIN (?:ENCRYPTED |RSA |EC |OPENSSH )?PRIVATE KEY")
records = []
for candidate in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
    relative = candidate.relative_to(root).as_posix()
    mode = candidate.lstat().st_mode
    if stat.S_ISLNK(mode):
        raise SystemExit(f"symlink_forbidden:{relative}")
    if stat.S_ISDIR(mode):
        if candidate.name == ".git":
            raise SystemExit(f"repository_metadata_forbidden:{relative}")
        continue
    if not stat.S_ISREG(mode):
        raise SystemExit(f"special_entry_forbidden:{relative}")
    lowered = relative.lower()
    if candidate.suffix.lower() in forbidden_suffixes or secret_name.search(lowered):
        raise SystemExit(f"sensitive_artifact_forbidden:{relative}")
    content = candidate.read_bytes()
    if private_key_header.search(content):
        raise SystemExit(f"private_key_forbidden:{relative}")
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

if [[ "${1:-}" == '--digest' ]]; then
  [[ $# == 2 ]] || die 'usage: deploy_public_unlock_atomic.sh --digest /absolute/public-site'
  tree_digest "$2"
  exit 0
fi

[[ $# == 4 ]] || die 'usage: deploy_public_unlock_atomic.sh SOURCE EXPECTED_CURRENT RELEASE_ID SOURCE_SHA256'
[[ "$(id -u)" == '0' ]] || die 'root_required'

source_dir=$1
expected_current=$2
release_id=$3
expected_digest=$4
release_root='/srv/k32/public/releases'
current='/srv/k32/public/current'
lock='/srv/k32/public/.kitsu-production-promotion.lock'
origin="${KITSU_PUBLIC_ORIGIN:-http://127.0.0.1:8100}"
target="$release_root/$release_id"
staging="$release_root/.$release_id.tmp"
next_link="/srv/k32/public/.current-$release_id.next"
rollback_link="/srv/k32/public/.current-$release_id.rollback"
probe_dir=''
switched=0

[[ "$source_dir" == /* && -d "$source_dir" && ! -L "$source_dir" ]] || die 'invalid_source_directory'
[[ "$expected_current" == "$release_root/"* ]] || die 'expected_current_outside_release_root'
[[ "$release_id" =~ ^[0-9]{8}T[0-9]{6}Z-[a-z0-9][a-z0-9.-]{0,63}-public-unlock$ ]] || die 'invalid_release_id'
[[ "$expected_digest" =~ ^[0-9a-f]{64}$ ]] || die 'invalid_source_digest'

source_dir=$(readlink -f -- "$source_dir")
expected_current=$(readlink -f -- "$expected_current")
[[ "$expected_current" == "$release_root/"* && -d "$expected_current" ]] || die 'invalid_expected_current'

rollback() {
  local status=$?
  local rollback_status=0
  trap - ERR EXIT
  set +e
  if (( switched )); then
    if [[ -L "$rollback_link" ]] && mv -Tf -- "$rollback_link" "$current" \
        && [[ "$(readlink -f -- "$current")" == "$expected_current" ]]; then
      /usr/sbin/nginx -t >/dev/null 2>&1 && systemctl reload nginx
      printf 'KITSU_PUBLIC_UNLOCK_ROLLBACK_OK restored=%s\n' "$expected_current" >&2
    else
      rollback_status=98
      printf 'KITSU_PUBLIC_UNLOCK_ROLLBACK_INCOMPLETE expected=%s recovery=%s\n' \
        "$expected_current" "$rollback_link" >&2
    fi
  fi
  rm -f -- "$next_link"
  [[ -n "$probe_dir" ]] && rm -rf -- "$probe_dir"
  [[ -e "$staging" ]] && rm -rf -- "$staging"
  (( rollback_status == 0 )) && rm -f -- "$rollback_link"
  (( rollback_status == 0 )) || status=$rollback_status
  exit "$status"
}
trap rollback ERR EXIT

for command in chown chmod cmp cp curl cut find flock install ln mktemp mv openssl python3 readlink rm sha256sum sleep systemctl; do
  command -v "$command" >/dev/null || die "missing_command:$command"
done
[[ -x /usr/sbin/nginx ]] || die 'nginx_missing'

exec 9>"$lock"
flock -n 9 || die 'promotion_lock_busy'
[[ -L "$current" && "$(readlink -f -- "$current")" == "$expected_current" ]] || die 'unexpected_current_release'
[[ ! -e "$target" && ! -L "$target" && ! -e "$staging" && ! -L "$staging" ]] || die 'release_target_exists'
[[ ! -e "$next_link" && ! -L "$next_link" && ! -e "$rollback_link" && ! -L "$rollback_link" ]] || die 'stale_switch_link'

actual_digest=$(tree_digest "$source_dir")
[[ "$actual_digest" == "$expected_digest" ]] || die "source_digest_mismatch:$actual_digest"
openssl pkeyutl -verify -pubin \
  -inkey "$source_dir/downloads/update-ed25519-public.pem" \
  -rawin -in "$source_dir/downloads/latest.json" \
  -sigfile "$source_dir/downloads/latest.json.sig" >/dev/null \
  || die 'android_manifest_signature_invalid'

python3 - "$source_dir" <<'PY'
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

install -d -o root -g root -m 0755 -- "$staging"
cp -a -- "$source_dir/." "$staging/"
[[ "$(tree_digest "$staging")" == "$expected_digest" ]] || die 'staged_tree_mismatch'
chown -R root:root -- "$staging"
find "$staging" -type d -exec chmod 0555 {} +
find "$staging" -type f -exec chmod 0444 {} +
mv -- "$staging" "$target"

/usr/sbin/nginx -t
ln -s -- "$expected_current" "$rollback_link"
ln -s -- "$target" "$next_link"
mv -Tf -- "$next_link" "$current"
switched=1
[[ "$(readlink -f -- "$current")" == "$target" ]] || die 'atomic_switch_mismatch'
systemctl reload nginx
[[ "$(systemctl is-active nginx)" == 'active' ]] || die 'nginx_not_active'

probe_dir=$(mktemp -d '/tmp/kitsu-public-unlock.XXXXXX')
fetch_exact() {
  local mode=$1
  local request_path=$2
  local expected=$3
  local output
  output="$probe_dir/$mode-$(printf '%s' "$request_path" | sha256sum | cut -d' ' -f1)"
  if [[ "$mode" == 'origin' ]]; then
    curl --fail --silent --show-error --max-time 20 --header 'Host: k32.run' "$origin$request_path" -o "$output" || return 1
  else
    curl --fail --silent --show-error --max-time 30 "https://k32.run$request_path" -o "$output" || return 1
  fi
  cmp -s -- "$expected" "$output"
}

ready=0
for _ in {1..20}; do
  if fetch_exact origin '/unlock/#code=K8-ABCDE-FGHJK-MNPQR' "$target/unlock/index.html"; then
    ready=1
    break
  fi
  sleep 0.25
done
[[ "$ready" == 1 ]] || die 'origin_unlock_not_ready'
fetch_exact origin '/' "$target/index.html"
fetch_exact origin '/unlock/unlock.css' "$target/unlock/unlock.css"
fetch_exact origin '/unlock/unlock.js' "$target/unlock/unlock.js"
fetch_exact origin '/unlock/catalog.js' "$target/unlock/catalog.js"
fetch_exact origin '/downloads/latest.json' "$target/downloads/latest.json"
fetch_exact origin '/downloads/latest.json.sig' "$target/downloads/latest.json.sig"
fetch_exact public "/unlock/?release=$release_id#code=K8-ABCDE-FGHJK-MNPQR" "$target/unlock/index.html"

[[ "$(readlink -f -- "$current")" == "$target" ]] || die 'current_changed_after_checks'
rm -f -- "$rollback_link"
switched=0
trap - ERR EXIT
rm -rf -- "$probe_dir"
printf 'KITSU_PUBLIC_UNLOCK_DEPLOY_OK release=%s source_sha256=%s\n' "$target" "$expected_digest"
