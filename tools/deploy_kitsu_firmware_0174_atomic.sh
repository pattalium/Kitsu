#!/usr/bin/env bash
set -Eeuo pipefail

expected_version='0.17.4'
expected_release_id='kitsu-0.17.4-reflashable-1'
expected_plan_schema='kitsu.firmware-publication-plan.v1'
expected_public_key_sha256='711ad6b564e129cbd31b8edca52f4977c03daf0410490f62c6fba4484f65366c'
expected_validator_sha256='6de82e00e5e011e1b2579aceae48302393c9915dcd0599af172a5853ecac272a'

archive=''
archive_sha256=''
upload_root=''
flash_base=''
update_base=''
evidence_parent=''
probe_origin=''
flash_host=''
update_host=''
while (($#)); do
  case "$1" in
    --archive)
      (($# >= 2)) || { echo 'missing --archive value' >&2; exit 2; }
      archive=$2
      shift 2
      ;;
    --archive-sha256)
      (($# >= 2)) || { echo 'missing --archive-sha256 value' >&2; exit 2; }
      archive_sha256=$2
      shift 2
      ;;
    --upload-root)
      (($# >= 2)) || { echo 'missing --upload-root value' >&2; exit 2; }
      upload_root=$2
      shift 2
      ;;
    --flash-base)
      (($# >= 2)) || { echo 'missing --flash-base value' >&2; exit 2; }
      flash_base=$2
      shift 2
      ;;
    --update-base)
      (($# >= 2)) || { echo 'missing --update-base value' >&2; exit 2; }
      update_base=$2
      shift 2
      ;;
    --evidence-parent)
      (($# >= 2)) || { echo 'missing --evidence-parent value' >&2; exit 2; }
      evidence_parent=$2
      shift 2
      ;;
    --probe-origin)
      (($# >= 2)) || { echo 'missing --probe-origin value' >&2; exit 2; }
      probe_origin=$2
      shift 2
      ;;
    --flash-host)
      (($# >= 2)) || { echo 'missing --flash-host value' >&2; exit 2; }
      flash_host=$2
      shift 2
      ;;
    --update-host)
      (($# >= 2)) || { echo 'missing --update-host value' >&2; exit 2; }
      update_host=$2
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

[[ $EUID -eq 0 ]] || { echo 'run as root' >&2; exit 1; }
for bounded_path in "$upload_root" "$flash_base" "$update_base" "$evidence_parent"; do
  [[ $bounded_path =~ ^/[0-9A-Za-z._-]+(/[0-9A-Za-z._-]+)*$ ]] || {
    echo 'deployment paths must be bounded absolute POSIX paths without whitespace' >&2
    exit 1
  }
done
[[ $flash_base != "$update_base" && $flash_base != "$evidence_parent" && $update_base != "$evidence_parent" ]] || {
  echo 'flash, update, and evidence roots must be distinct' >&2
  exit 1
}
[[ $flash_base != "$update_base/"* && $update_base != "$flash_base/"* &&
   $flash_base != "$evidence_parent/"* && $evidence_parent != "$flash_base/"* &&
   $update_base != "$evidence_parent/"* && $evidence_parent != "$update_base/"* ]] || {
  echo 'flash, update, and evidence roots must not contain one another' >&2
  exit 1
}
[[ $archive == "$upload_root/"* && ${archive#"$upload_root/"} =~ ^kitsu-firmware-0174-[0-9a-f]{64}\.tar\.gz$ ]] || {
  echo 'archive path is outside the bounded firmware publication path' >&2
  exit 1
}
[[ $archive_sha256 =~ ^[0-9a-f]{64}$ ]] || { echo 'invalid archive SHA-256' >&2; exit 1; }
[[ $probe_origin =~ ^https?://[0-9A-Za-z][0-9A-Za-z.-]{0,252}(:[1-9][0-9]{0,4})?$ ]] || {
  echo 'probe origin must be an HTTP(S) origin without a path, query, or credentials' >&2
  exit 1
}
for host_header in "$flash_host" "$update_host"; do
  [[ $host_header =~ ^[0-9A-Za-z][0-9A-Za-z.-]{0,252}(:[1-9][0-9]{0,4})?$ ]] || {
    echo 'probe Host header contains unsafe characters' >&2
    exit 1
  }
done

for command_name in chmod chown cmp cp curl cut diff find flock install ln mktemp mv nginx openssl \
  python3 readlink rm sha256sum sort stat tar touch; do
  command -v "$command_name" >/dev/null || {
    echo "missing dependency: $command_name" >&2
    exit 1
  }
done

[[ -d $upload_root && ! -L $upload_root ]] || {
  echo 'upload root is missing or is a symlink' >&2
  exit 1
}
[[ $(readlink -e "$upload_root") == "$upload_root" ]] || {
  echo 'upload root must use its canonical absolute path' >&2
  exit 1
}
for deployment_root in "$flash_base" "$update_base" "$evidence_parent"; do
  [[ -d $deployment_root && ! -L $deployment_root ]] || {
    echo 'deployment root is missing or is a symlink' >&2
    exit 1
  }
  [[ $(readlink -e "$deployment_root") == "$deployment_root" ]] || {
    echo 'deployment root must use its canonical absolute path' >&2
    exit 1
  }
done

[[ -f $archive && ! -L $archive ]] || { echo 'unsafe or missing archive' >&2; exit 1; }
[[ $(sha256sum "$archive" | cut -d' ' -f1) == "$archive_sha256" ]] || {
  echo 'archive digest mismatch' >&2
  exit 1
}

python3 - "$archive" <<'PY'
import pathlib
import sys
import tarfile

archive = pathlib.Path(sys.argv[1])
with tarfile.open(archive, "r:gz") as bundle:
    members = bundle.getmembers()
    if not members:
        raise SystemExit("publication archive is empty")
    if len(members) > 256 or sum(member.size for member in members) > 32 * 1024 * 1024:
        raise SystemExit("publication archive exceeds its bounded inventory")
    for member in members:
        name = member.name
        path = pathlib.PurePosixPath(name)
        if (
            not name
            or "\\" in name
            or any(ord(character) < 32 for character in name)
            or path.is_absolute()
            or ".." in path.parts
            or not (member.isfile() or member.isdir())
        ):
            raise SystemExit(f"unsafe publication archive member: {name!r}")
PY

exec 9>/run/lock/kitsu-firmware-publish.lock
flock -n 9 || { echo 'another firmware publication is active' >&2; exit 1; }

work=$(mktemp -d '/var/tmp/kitsu-firmware-0174.XXXXXX')
[[ $work == /var/tmp/kitsu-firmware-0174.* ]] || { echo 'unsafe temporary path' >&2; exit 1; }
cleanup_work() {
  if [[ -n ${work:-} && $work == /var/tmp/kitsu-firmware-0174.* ]]; then
    rm -rf -- "$work"
  fi
}
trap cleanup_work EXIT

stage="$work/stage"
install -d -o root -g root -m 0700 -- "$stage"
tar -xzf "$archive" --no-same-owner --no-same-permissions -C "$stage"
[[ -z $(find "$stage" -type l -print -quit) ]] || { echo 'stage contains a symlink' >&2; exit 1; }
[[ -z $(find "$stage" -not -type d -not -type f -print -quit) ]] || {
  echo 'stage contains a special file' >&2
  exit 1
}

validator="$stage/tools/validate-stage.py"
[[ -f $validator && ! -L $validator ]] || { echo 'stage validator is missing' >&2; exit 1; }
[[ $(sha256sum "$validator" | cut -d' ' -f1) == "$expected_validator_sha256" ]] || {
  echo 'stage validator identity changed' >&2
  exit 1
}
python3 "$validator" validate-stage --stage "$stage" --require-signature --openssl "$(command -v openssl)"

metadata=$(
  python3 - "$stage/publication-plan.json" "$expected_plan_schema" "$expected_version" \
    "$expected_release_id" "$expected_public_key_sha256" <<'PY'
import json
import pathlib
import re
import sys

plan = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
if plan.get("schema") != sys.argv[2]:
    raise SystemExit("publication plan schema changed")
if plan.get("firmware_version") != sys.argv[3] or plan.get("release_id") != sys.argv[4]:
    raise SystemExit("publication release identity changed")
if plan.get("public_key_sha256") != sys.argv[5]:
    raise SystemExit("publication update authority changed")
flash_id = plan.get("flash_release_id")
manifest_sha = plan.get("manifest", {}).get("sha256")
if not isinstance(flash_id, str) or not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z._-]{0,127}", flash_id):
    raise SystemExit("unsafe flash release identity")
if not isinstance(manifest_sha, str) or not re.fullmatch(r"[0-9a-f]{64}", manifest_sha):
    raise SystemExit("unsafe manifest identity")
print(plan["release_id"])
print(flash_id)
print(manifest_sha)
PY
)
mapfile -t release_metadata <<<"$metadata"
[[ ${#release_metadata[@]} -eq 3 ]] || { echo 'incomplete release metadata' >&2; exit 1; }
release_id=${release_metadata[0]}
flash_id=${release_metadata[1]}
manifest_sha256=${release_metadata[2]}
signature_sha256=$(sha256sum "$stage/update/latest.json.sig" | cut -d' ' -f1)

flash_root="$flash_base/releases"
flash_release="$flash_root/$flash_id"
flash_current="$flash_base/current"
flash_next="$flash_base/.current.next-$flash_id"
update_root="$update_base/releases"
update_release="$update_root/$release_id"
update_current="$update_base/current"
update_next="$update_base/.current.next-$flash_id"
evidence_root="$evidence_parent/$release_id"

[[ $flash_release == "$flash_root/"* && $update_release == "$update_root/"* ]] || {
  echo 'computed release path escaped its immutable root' >&2
  exit 1
}
nginx -t
[[ ! -e $flash_release && ! -L $flash_release ]] || { echo 'flash release already exists' >&2; exit 1; }
[[ ! -e $update_release && ! -L $update_release ]] || { echo 'update release already exists' >&2; exit 1; }
[[ ! -e $evidence_root && ! -L $evidence_root ]] || { echo 'deployment record already exists' >&2; exit 1; }
[[ ! -e $flash_next && ! -L $flash_next && ! -e $update_next && ! -L $update_next ]] || {
  echo 'candidate current symlink already exists' >&2
  exit 1
}

old_flash=$(readlink -e "$flash_current")
old_update=$(readlink -e "$update_current")
[[ $old_flash == "$flash_root/"* && $old_update == "$update_root/"* ]] || {
  echo 'current symlink escapes its immutable release root' >&2
  exit 1
}

prepared=0
flash_switched=0
update_switched=0
rollback() {
  status=${1:-$?}
  trap - ERR INT TERM
  set +e
  rollback_safe=1
  rm -f -- "$flash_next" "$update_next"
  if ((flash_switched)); then
    if ! ln -s -- "$old_flash" "$flash_next" || ! mv -Tf -- "$flash_next" "$flash_current"; then
      rollback_safe=0
    fi
  fi
  if ((update_switched)); then
    if ! ln -s -- "$old_update" "$update_next" || ! mv -Tf -- "$update_next" "$update_current"; then
      rollback_safe=0
    fi
  fi
  rm -f -- "$flash_next" "$update_next"
  if ((prepared && rollback_safe)); then
    rm -rf -- "$flash_release" "$update_release" "$evidence_root"
  elif ((prepared)); then
    echo 'rollback could not restore every current symlink; candidate releases were preserved' >&2
  fi
  echo "KITSU_FIRMWARE_0174_DEPLOY_FAILED status=$status" >&2
  exit "$status"
}
trap 'rollback $?' ERR
trap 'rollback 130' INT
trap 'rollback 143' TERM

prepared=1
install -d -o root -g root -m 0755 -- "$flash_root" "$update_root"
install -d -o root -g root -m 0755 -- "$flash_release" "$update_release"
install -d -o root -g root -m 0700 -- "$evidence_parent" "$evidence_root"
cp --archive --no-dereference "$stage/flash/." "$flash_release/"
cp --archive --no-dereference "$stage/update/." "$update_release/"
diff -qr "$stage/flash" "$flash_release" >/dev/null
diff -qr "$stage/update" "$update_release" >/dev/null
chown -R root:root "$flash_release" "$update_release" "$evidence_root"
find "$flash_release" "$update_release" -type d -exec chmod 0555 {} +
find "$flash_release" "$update_release" -type f -exec chmod 0444 {} +

cp -- "$stage/publication-plan.json" "$evidence_root/publication-plan.json"
printf '%s\n' "$old_flash" >"$evidence_root/previous-flash-release"
printf '%s\n' "$old_update" >"$evidence_root/previous-update-release"
printf '%s\n' "$flash_release" >"$evidence_root/candidate-flash-release"
printf '%s\n' "$update_release" >"$evidence_root/candidate-update-release"
printf '%s\n' "$manifest_sha256" >"$evidence_root/manifest-sha256"
printf '%s\n' "$signature_sha256" >"$evidence_root/signature-sha256"

ln -s -- "$update_release" "$update_next"
ln -s -- "$flash_release" "$flash_next"
mv -Tf -- "$update_next" "$update_current"
update_switched=1
mv -Tf -- "$flash_next" "$flash_current"
flash_switched=1

probe="$work/served-file"
while IFS= read -r relative; do
  curl --fail --silent --show-error --max-time 30 -H "Host: $update_host" \
    "$probe_origin/$relative" --output "$probe"
  cmp --silent "$update_release/$relative" "$probe"
done < <(cd "$update_release" && find . -type f -printf '%P\n' | sort)

while IFS= read -r relative; do
  curl --fail --silent --show-error --max-time 30 -H "Host: $flash_host" \
    "$probe_origin/$relative" --output "$probe"
  cmp --silent "$flash_release/$relative" "$probe"
done < <(cd "$flash_release" && find . -type f -printf '%P\n' | sort)

curl --fail --silent --show-error --max-time 10 -H "Host: $update_host" \
  "$probe_origin/latest.json" --output "$work/live-latest.json"
curl --fail --silent --show-error --max-time 10 -H "Host: $update_host" \
  "$probe_origin/latest.json.sig" --output "$work/live-latest.json.sig"
curl --fail --silent --show-error --max-time 10 -H "Host: $update_host" \
  "$probe_origin/update-ed25519-public.pem" --output "$work/live-update-public.pem"
openssl pkeyutl -verify -pubin -inkey "$work/live-update-public.pem" -rawin \
  -in "$work/live-latest.json" -sigfile "$work/live-latest.json.sig" >/dev/null

printf '%s\n' "$release_id" >"$evidence_root/accepted-release"
touch "$evidence_parent/updates-required"
prepared=0
trap - ERR INT TERM
printf 'KITSU_FIRMWARE_0174_DEPLOY_OK release=%s manifest_sha256=%s signature_sha256=%s flash=%s update=%s\n' \
  "$release_id" "$manifest_sha256" "$signature_sha256" "$flash_release" "$update_release"
