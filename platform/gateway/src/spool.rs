use std::{
    fs::{self, File, OpenOptions},
    io::{self, Read, Seek, Write},
    path::{Path, PathBuf},
};

use fs2::FileExt;
use serde::{Deserialize, Serialize};
use uuid::Uuid;

use crate::config::DeploymentScope;

const RECORD_MAGIC: [u8; 4] = *b"KSP1";
const STATE_MAGIC: [u8; 4] = *b"KST1";
const RECORD_HEADER_BYTES: u64 = 24;
const STATE_BYTES: usize = 40;
const STATE_VERSION: u32 = 1;
const SPOOL_IDENTITY_SCHEMA: &str = "kitsu.gateway-spool-identity.v1";
const SPOOL_IDENTITY_FILE: &str = "spool-identity.json";
const SPOOL_IDENTITY_MAX_BYTES: u64 = 1_024;
const SPOOL_LOCK_FILE: &str = ".kitsu-spool.lock";

#[derive(Debug, Clone, Copy, Deserialize, Eq, PartialEq, Serialize)]
pub struct SpoolIdentity {
    pub deployment_scope: DeploymentScope,
    pub gateway_id: Uuid,
}

#[derive(Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct StoredSpoolIdentity {
    schema: String,
    deployment_scope: DeploymentScope,
    gateway_id: Uuid,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpoolRecord {
    pub id: u64,
    pub payload: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpoolStats {
    pub next_id: u64,
    pub acked_id: u64,
    pub total_bytes: u64,
    pub pending_records: u64,
    pub segment_count: usize,
}

#[derive(Debug, thiserror::Error)]
pub enum SpoolError {
    #[error("spool is full ({used} of {limit} bytes)")]
    Full { used: u64, limit: u64 },
    #[error("spool record is empty or too large")]
    InvalidRecord,
    #[error("spool corruption in {path}: {reason}")]
    Corrupt { path: PathBuf, reason: &'static str },
    #[error("invalid acknowledgement {requested}; maximum committed record is {maximum}")]
    InvalidAck { requested: u64, maximum: u64 },
    #[error("cannot exclusively lock spool at {path}: {source}")]
    Lock {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("invalid spool identity metadata in {path}: {reason}")]
    Identity { path: PathBuf, reason: String },
    #[error(transparent)]
    Io(#[from] io::Error),
}

#[derive(Debug, Clone, Copy, Default)]
struct DurableState {
    generation: u64,
    acked_id: u64,
    next_id: u64,
}

#[derive(Debug)]
struct SegmentInfo {
    path: PathBuf,
    start_id: u64,
    end_id: u64,
    bytes: u64,
    records: u64,
}

pub struct Spool {
    _lock_file: File,
    dir: PathBuf,
    segment_max_bytes: u64,
    max_total_bytes: u64,
    max_record_bytes: usize,
    state: DurableState,
    total_bytes: u64,
    committed_records: u64,
    segments: Vec<SegmentInfo>,
}

impl Spool {
    pub fn open(
        dir: impl Into<PathBuf>,
        segment_max_bytes: u64,
        max_total_bytes: u64,
        max_record_bytes: usize,
        expected_identity: SpoolIdentity,
    ) -> Result<Self, SpoolError> {
        let dir = dir.into();
        fs::create_dir_all(&dir)?;
        let lock_path = dir.join(SPOOL_LOCK_FILE);
        let lock_file = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(false)
            .open(&lock_path)?;
        lock_file
            .try_lock_exclusive()
            .map_err(|source| SpoolError::Lock {
                path: lock_path,
                source,
            })?;
        let create_spool_identity = validate_spool_identity_or_migration(&dir, expected_identity)?;
        let mut state = read_best_state(&dir)?.unwrap_or(DurableState {
            next_id: 1,
            ..DurableState::default()
        });
        let mut paths = segment_paths(&dir)?;
        paths.sort();

        let mut segments = Vec::with_capacity(paths.len());
        let mut total_bytes = 0_u64;
        let mut committed_records = 0_u64;
        let mut last_id = 0_u64;
        for (index, path) in paths.iter().enumerate() {
            let repair_torn_tail = index + 1 == paths.len();
            let segment = scan_segment(path, max_record_bytes, repair_torn_tail)?;
            if segment.records > 0 {
                if segment.start_id <= last_id || (last_id != 0 && segment.start_id != last_id + 1)
                {
                    return Err(SpoolError::Corrupt {
                        path: path.clone(),
                        reason: "record IDs are not contiguous",
                    });
                }
                last_id = segment.end_id;
                committed_records = committed_records.saturating_add(segment.records);
            }
            total_bytes = total_bytes.saturating_add(segment.bytes);
            segments.push(segment);
        }

        state.next_id = state.next_id.max(last_id.saturating_add(1)).max(1);
        if state.acked_id > last_id && last_id != 0 {
            return Err(SpoolError::Corrupt {
                path: dir.clone(),
                reason: "acknowledgement is ahead of the WAL",
            });
        }

        let spool = Self {
            _lock_file: lock_file,
            dir,
            segment_max_bytes,
            max_total_bytes,
            max_record_bytes,
            state,
            total_bytes,
            committed_records,
            segments,
        };
        if create_spool_identity {
            write_new_spool_identity(
                &spool.dir,
                &spool.dir.join(SPOOL_IDENTITY_FILE),
                expected_identity,
            )?;
        }
        if read_best_state(&spool.dir)?.is_none() {
            spool.persist_state()?;
        }
        Ok(spool)
    }

    pub fn append(&mut self, payload: &[u8]) -> Result<u64, SpoolError> {
        if payload.is_empty()
            || payload.len() > self.max_record_bytes
            || payload.len() > u32::MAX as usize
        {
            return Err(SpoolError::InvalidRecord);
        }
        let record_bytes = RECORD_HEADER_BYTES + payload.len() as u64;
        if self.total_bytes.saturating_add(record_bytes) > self.max_total_bytes {
            return Err(SpoolError::Full {
                used: self.total_bytes,
                limit: self.max_total_bytes,
            });
        }

        let id = self.state.next_id;
        if id == 0 || id == u64::MAX {
            return Err(SpoolError::InvalidRecord);
        }
        let rotate = self.segments.last().is_none_or(|segment| {
            segment.bytes > 0 && segment.bytes.saturating_add(record_bytes) > self.segment_max_bytes
        });
        if rotate {
            let path = self.dir.join(format!("{id:020}.wal"));
            let file = OpenOptions::new()
                .create_new(true)
                .write(true)
                .open(&path)?;
            file.sync_all()?;
            sync_directory(&self.dir)?;
            self.segments.push(SegmentInfo {
                path,
                start_id: id,
                end_id: 0,
                bytes: 0,
                records: 0,
            });
        }

        let segment = self
            .segments
            .last_mut()
            .expect("segment exists after rotation");
        let mut file = OpenOptions::new().append(true).open(&segment.path)?;
        write_record(&mut file, id, payload)?;
        file.sync_data()?;

        segment.end_id = id;
        segment.bytes = segment.bytes.saturating_add(record_bytes);
        segment.records = segment.records.saturating_add(1);
        self.total_bytes = self.total_bytes.saturating_add(record_bytes);
        self.committed_records = self.committed_records.saturating_add(1);
        self.state.next_id = id + 1;
        Ok(id)
    }

    pub fn read_pending(
        &self,
        max_records: usize,
        max_bytes: usize,
    ) -> Result<Vec<SpoolRecord>, SpoolError> {
        if max_records == 0 || max_bytes == 0 {
            return Ok(Vec::new());
        }
        let mut output = Vec::new();
        let mut output_bytes = 0_usize;
        for segment in &self.segments {
            if segment.records == 0 || segment.end_id <= self.state.acked_id {
                continue;
            }
            let mut file = File::open(&segment.path)?;
            loop {
                let Some(record) = read_record(&mut file, &segment.path, self.max_record_bytes)?
                else {
                    break;
                };
                if record.id <= self.state.acked_id {
                    continue;
                }
                if !output.is_empty()
                    && output_bytes.saturating_add(record.payload.len()) > max_bytes
                {
                    return Ok(output);
                }
                output_bytes = output_bytes.saturating_add(record.payload.len());
                output.push(record);
                if output.len() >= max_records || output_bytes >= max_bytes {
                    return Ok(output);
                }
            }
        }
        Ok(output)
    }

    pub fn acknowledge_through(&mut self, record_id: u64) -> Result<(), SpoolError> {
        if record_id <= self.state.acked_id {
            return Ok(());
        }
        let maximum = self.state.next_id.saturating_sub(1);
        if record_id > maximum {
            return Err(SpoolError::InvalidAck {
                requested: record_id,
                maximum,
            });
        }

        let old_state = self.state;
        self.state.acked_id = record_id;
        self.state.generation = self.state.generation.saturating_add(1);
        if let Err(error) = self.persist_state() {
            self.state = old_state;
            return Err(error);
        }
        self.compact_acknowledged_segments()?;
        Ok(())
    }

    pub fn stats(&self) -> SpoolStats {
        let committed_max = self.state.next_id.saturating_sub(1);
        SpoolStats {
            next_id: self.state.next_id,
            acked_id: self.state.acked_id,
            total_bytes: self.total_bytes,
            pending_records: committed_max.saturating_sub(self.state.acked_id),
            segment_count: self.segments.len(),
        }
    }

    fn persist_state(&self) -> Result<(), SpoolError> {
        let slot = if self.state.generation % 2 == 0 {
            "state-a.bin"
        } else {
            "state-b.bin"
        };
        let path = self.dir.join(slot);
        let bytes = encode_state(self.state);
        let mut file = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(path)?;
        file.write_all(&bytes)?;
        file.sync_all()?;
        sync_directory(&self.dir)?;
        Ok(())
    }

    fn compact_acknowledged_segments(&mut self) -> Result<(), SpoolError> {
        let keep_last = self.segments.last().map(|segment| segment.path.clone());
        let mut retained = Vec::with_capacity(self.segments.len());
        for segment in self.segments.drain(..) {
            let removable = segment.records > 0
                && segment.end_id <= self.state.acked_id
                && Some(&segment.path) != keep_last.as_ref();
            if removable {
                fs::remove_file(&segment.path)?;
                self.total_bytes = self.total_bytes.saturating_sub(segment.bytes);
                self.committed_records = self.committed_records.saturating_sub(segment.records);
            } else {
                retained.push(segment);
            }
        }
        self.segments = retained;
        sync_directory(&self.dir)?;
        Ok(())
    }
}

fn validate_spool_identity_or_migration(
    dir: &Path,
    expected: SpoolIdentity,
) -> Result<bool, SpoolError> {
    let path = dir.join(SPOOL_IDENTITY_FILE);
    let metadata = match fs::symlink_metadata(&path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            validate_missing_identity_contents(dir, &path, expected.deployment_scope)?;
            return Ok(true);
        }
        Err(error) => return Err(error.into()),
    };
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(identity_error(
            &path,
            "identity metadata must be a regular non-symlink file",
        ));
    }
    if metadata.len() > SPOOL_IDENTITY_MAX_BYTES {
        return Err(identity_error(&path, "identity metadata is oversized"));
    }

    let file = File::open(&path)?;
    if !file.metadata()?.is_file() {
        return Err(identity_error(
            &path,
            "identity metadata changed to a non-regular file while opening",
        ));
    }
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.take(SPOOL_IDENTITY_MAX_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > SPOOL_IDENTITY_MAX_BYTES {
        return Err(identity_error(&path, "identity metadata is oversized"));
    }
    let stored: StoredSpoolIdentity = serde_json::from_slice(&bytes).map_err(|error| {
        identity_error(&path, format!("strict JSON validation failed: {error}"))
    })?;
    if stored.schema != SPOOL_IDENTITY_SCHEMA {
        return Err(identity_error(&path, "identity schema does not match"));
    }
    if stored.deployment_scope != expected.deployment_scope {
        return Err(identity_error(
            &path,
            "deployment scope does not match the persisted spool identity",
        ));
    }
    if stored.gateway_id != expected.gateway_id {
        return Err(identity_error(
            &path,
            "gateway ID does not match the persisted spool identity",
        ));
    }
    Ok(false)
}

fn validate_missing_identity_contents(
    dir: &Path,
    identity_path: &Path,
    scope: DeploymentScope,
) -> Result<(), SpoolError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if entry.file_name() == SPOOL_LOCK_FILE {
            continue;
        }

        if scope == DeploymentScope::Public {
            return Err(identity_error(
                identity_path,
                format!(
                    "a public spool without identity metadata must be otherwise empty; found {}",
                    path.display()
                ),
            ));
        }

        let name = entry.file_name();
        let legacy_name =
            name == "state-a.bin" || name == "state-b.bin" || parse_segment_start(&path).is_some();
        let file_type = entry.file_type()?;
        if !legacy_name || file_type.is_symlink() || !file_type.is_file() {
            return Err(identity_error(
                identity_path,
                format!(
                    "an unidentified private spool may contain only regular legacy state and WAL files; found {}",
                    path.display()
                ),
            ));
        }
    }
    Ok(())
}

fn write_new_spool_identity(
    dir: &Path,
    path: &Path,
    expected: SpoolIdentity,
) -> Result<(), SpoolError> {
    let stored = StoredSpoolIdentity {
        schema: SPOOL_IDENTITY_SCHEMA.to_owned(),
        deployment_scope: expected.deployment_scope,
        gateway_id: expected.gateway_id,
    };
    let mut bytes = serde_json::to_vec_pretty(&stored)
        .map_err(|error| identity_error(path, format!("serialize identity metadata: {error}")))?;
    bytes.push(b'\n');
    if bytes.len() as u64 > SPOOL_IDENTITY_MAX_BYTES {
        return Err(identity_error(
            path,
            "serialized identity metadata is oversized",
        ));
    }

    let temporary = dir.join(format!(".spool-identity.{}.tmp", Uuid::new_v4()));
    let write_result = (|| -> Result<(), SpoolError> {
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&temporary)?;
        file.write_all(&bytes)?;
        file.sync_all()?;
        fs::hard_link(&temporary, path)?;
        fs::remove_file(&temporary)?;
        sync_directory(dir)?;
        Ok(())
    })();
    if write_result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    write_result
}

fn identity_error(path: &Path, reason: impl Into<String>) -> SpoolError {
    SpoolError::Identity {
        path: path.to_owned(),
        reason: reason.into(),
    }
}

fn write_record(file: &mut File, id: u64, payload: &[u8]) -> io::Result<()> {
    let crc = crc32c::crc32c(payload);
    file.write_all(&RECORD_MAGIC)?;
    file.write_all(&id.to_be_bytes())?;
    file.write_all(&(payload.len() as u32).to_be_bytes())?;
    file.write_all(&crc.to_be_bytes())?;
    file.write_all(&0_u32.to_be_bytes())?;
    file.write_all(payload)
}

fn read_record(
    file: &mut File,
    path: &Path,
    max_record_bytes: usize,
) -> Result<Option<SpoolRecord>, SpoolError> {
    let offset = file.stream_position()?;
    let len = file.metadata()?.len();
    if offset == len {
        return Ok(None);
    }
    if len.saturating_sub(offset) < RECORD_HEADER_BYTES {
        return Err(SpoolError::Corrupt {
            path: path.to_owned(),
            reason: "partial record header",
        });
    }
    let mut header = [0_u8; RECORD_HEADER_BYTES as usize];
    file.read_exact(&mut header)?;
    if header[0..4] != RECORD_MAGIC || header[20..24] != [0, 0, 0, 0] {
        return Err(SpoolError::Corrupt {
            path: path.to_owned(),
            reason: "invalid record header",
        });
    }
    let id = u64::from_be_bytes(header[4..12].try_into().expect("fixed slice"));
    let payload_len = u32::from_be_bytes(header[12..16].try_into().expect("fixed slice")) as usize;
    let expected_crc = u32::from_be_bytes(header[16..20].try_into().expect("fixed slice"));
    if id == 0 || payload_len == 0 || payload_len > max_record_bytes {
        return Err(SpoolError::Corrupt {
            path: path.to_owned(),
            reason: "invalid record bounds",
        });
    }
    if len.saturating_sub(file.stream_position()?) < payload_len as u64 {
        return Err(SpoolError::Corrupt {
            path: path.to_owned(),
            reason: "partial record payload",
        });
    }
    let mut payload = vec![0_u8; payload_len];
    file.read_exact(&mut payload)?;
    if crc32c::crc32c(&payload) != expected_crc {
        return Err(SpoolError::Corrupt {
            path: path.to_owned(),
            reason: "record checksum mismatch",
        });
    }
    Ok(Some(SpoolRecord { id, payload }))
}

fn scan_segment(
    path: &Path,
    max_record_bytes: usize,
    repair_torn_tail: bool,
) -> Result<SegmentInfo, SpoolError> {
    let start_id = parse_segment_start(path).ok_or_else(|| SpoolError::Corrupt {
        path: path.to_owned(),
        reason: "invalid segment filename",
    })?;
    let mut file = OpenOptions::new()
        .read(true)
        .write(repair_torn_tail)
        .open(path)?;
    let mut records = 0_u64;
    let mut end_id = 0_u64;
    let mut last_good_offset = 0_u64;
    loop {
        match read_record(&mut file, path, max_record_bytes) {
            Ok(Some(record)) => {
                let expected = if end_id == 0 { start_id } else { end_id + 1 };
                if record.id != expected {
                    return Err(SpoolError::Corrupt {
                        path: path.to_owned(),
                        reason: "record ID mismatch",
                    });
                }
                end_id = record.id;
                records += 1;
                last_good_offset = file.stream_position()?;
            }
            Ok(None) => break,
            Err(
                SpoolError::Corrupt {
                    reason: "partial record header",
                    ..
                }
                | SpoolError::Corrupt {
                    reason: "partial record payload",
                    ..
                },
            ) if repair_torn_tail => {
                file.set_len(last_good_offset)?;
                file.sync_all()?;
                break;
            }
            Err(error) => return Err(error),
        }
    }
    let bytes = file.metadata()?.len();
    Ok(SegmentInfo {
        path: path.to_owned(),
        start_id,
        end_id,
        bytes,
        records,
    })
}

fn segment_paths(dir: &Path) -> io::Result<Vec<PathBuf>> {
    let mut output = Vec::new();
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.extension().and_then(|value| value.to_str()) == Some("wal") {
            output.push(path);
        }
    }
    Ok(output)
}

fn parse_segment_start(path: &Path) -> Option<u64> {
    let stem = path.file_stem()?.to_str()?;
    if stem.len() != 20 || !stem.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    stem.parse().ok()
}

fn encode_state(state: DurableState) -> [u8; STATE_BYTES] {
    let mut bytes = [0_u8; STATE_BYTES];
    bytes[0..4].copy_from_slice(&STATE_MAGIC);
    bytes[4..8].copy_from_slice(&STATE_VERSION.to_be_bytes());
    bytes[8..16].copy_from_slice(&state.generation.to_be_bytes());
    bytes[16..24].copy_from_slice(&state.acked_id.to_be_bytes());
    bytes[24..32].copy_from_slice(&state.next_id.to_be_bytes());
    let crc = crc32c::crc32c(&bytes[..32]);
    bytes[32..36].copy_from_slice(&crc.to_be_bytes());
    bytes
}

fn decode_state(bytes: &[u8]) -> Option<DurableState> {
    if bytes.len() != STATE_BYTES
        || bytes[0..4] != STATE_MAGIC
        || bytes[4..8] != STATE_VERSION.to_be_bytes()
        || bytes[36..40] != [0; 4]
    {
        return None;
    }
    let expected = u32::from_be_bytes(bytes[32..36].try_into().ok()?);
    if crc32c::crc32c(&bytes[..32]) != expected {
        return None;
    }
    let state = DurableState {
        generation: u64::from_be_bytes(bytes[8..16].try_into().ok()?),
        acked_id: u64::from_be_bytes(bytes[16..24].try_into().ok()?),
        next_id: u64::from_be_bytes(bytes[24..32].try_into().ok()?),
    };
    (state.next_id > 0 && state.acked_id < state.next_id).then_some(state)
}

fn read_best_state(dir: &Path) -> io::Result<Option<DurableState>> {
    let mut best: Option<DurableState> = None;
    for name in ["state-a.bin", "state-b.bin"] {
        match fs::read(dir.join(name)) {
            Ok(bytes) => {
                if let Some(state) = decode_state(&bytes) {
                    if best.is_none_or(|current| state.generation > current.generation) {
                        best = Some(state);
                    }
                }
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
    }
    Ok(best)
}

#[cfg(not(windows))]
fn sync_directory(dir: &Path) -> io::Result<()> {
    File::open(dir)?.sync_all()
}

// Rust's standard Windows File API cannot issue FlushFileBuffers on a
// directory handle. Every WAL/state file itself is flushed before this point;
// NTFS journals the create/delete metadata. Windows installers should place
// the spool on local NTFS/ReFS, never a network share.
#[cfg(windows)]
fn sync_directory(_dir: &Path) -> io::Result<()> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity() -> SpoolIdentity {
        SpoolIdentity {
            deployment_scope: DeploymentScope::Private,
            gateway_id: Uuid::parse_str("018f47ef-48cc-7c70-84d9-166c787a3e21").unwrap(),
        }
    }

    fn public_identity() -> SpoolIdentity {
        SpoolIdentity {
            deployment_scope: DeploymentScope::Public,
            ..identity()
        }
    }

    #[test]
    fn persists_records_and_acknowledgements_without_database() {
        let temp = tempfile::tempdir().unwrap();
        let mut spool = Spool::open(temp.path(), 80, 8192, 1024, identity()).unwrap();
        let one = spool.append(b"first record").unwrap();
        let two = spool.append(b"second record that rotates").unwrap();
        assert_eq!((one, two), (1, 2));
        assert_eq!(spool.read_pending(10, 1024).unwrap().len(), 2);
        spool.acknowledge_through(1).unwrap();
        drop(spool);

        let mut reopened = Spool::open(temp.path(), 80, 8192, 1024, identity()).unwrap();
        let pending = reopened.read_pending(10, 1024).unwrap();
        assert_eq!(
            pending,
            vec![SpoolRecord {
                id: 2,
                payload: b"second record that rotates".to_vec()
            }]
        );
        assert_eq!(reopened.append(b"third").unwrap(), 3);
    }

    #[test]
    fn repairs_only_a_torn_tail_of_latest_segment() {
        let temp = tempfile::tempdir().unwrap();
        let mut spool = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        spool.append(b"durable").unwrap();
        let path = spool.segments.last().unwrap().path.clone();
        drop(spool);
        OpenOptions::new()
            .append(true)
            .open(path)
            .unwrap()
            .write_all(b"KSP1partial")
            .unwrap();

        let reopened = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        assert_eq!(
            reopened.read_pending(10, 1024).unwrap()[0].payload,
            b"durable"
        );
    }

    #[test]
    fn checksum_corruption_fails_closed() {
        let temp = tempfile::tempdir().unwrap();
        let mut spool = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        spool.append(b"durable").unwrap();
        let path = spool.segments.last().unwrap().path.clone();
        drop(spool);
        let mut file = OpenOptions::new()
            .read(true)
            .write(true)
            .open(path)
            .unwrap();
        file.seek(std::io::SeekFrom::Start(RECORD_HEADER_BYTES))
            .unwrap();
        file.write_all(b"X").unwrap();
        file.sync_all().unwrap();

        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, identity()),
            Err(SpoolError::Corrupt { .. })
        ));
    }

    #[test]
    fn exclusive_process_lock_is_held_for_the_spool_lifetime() {
        let temp = tempfile::tempdir().unwrap();
        let first = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, identity()),
            Err(SpoolError::Lock { .. })
        ));

        drop(first);
        Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
    }

    #[test]
    fn persisted_identity_rejects_scope_and_gateway_reuse() {
        let temp = tempfile::tempdir().unwrap();
        let first = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        drop(first);

        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, public_identity()),
            Err(SpoolError::Identity { .. })
        ));

        let different_gateway = SpoolIdentity {
            gateway_id: Uuid::new_v4(),
            ..identity()
        };
        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, different_gateway),
            Err(SpoolError::Identity { .. })
        ));
    }

    #[test]
    fn spool_identity_metadata_is_strict_and_bounded() {
        let temp = tempfile::tempdir().unwrap();
        let first = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        drop(first);
        let path = temp.path().join(SPOOL_IDENTITY_FILE);

        let mut value: serde_json::Value =
            serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        value
            .as_object_mut()
            .unwrap()
            .insert("unknown".to_owned(), serde_json::json!(true));
        fs::write(&path, serde_json::to_vec(&value).unwrap()).unwrap();
        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, identity()),
            Err(SpoolError::Identity { .. })
        ));

        fs::write(&path, vec![b' '; SPOOL_IDENTITY_MAX_BYTES as usize + 1]).unwrap();
        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, identity()),
            Err(SpoolError::Identity { .. })
        ));
    }

    #[test]
    fn public_identity_can_initialize_only_a_truly_empty_spool() {
        let fresh = tempfile::tempdir().unwrap();
        let public = Spool::open(fresh.path(), 4096, 8192, 1024, public_identity()).unwrap();
        drop(public);
        assert!(fresh.path().join(SPOOL_IDENTITY_FILE).is_file());

        let state_only = tempfile::tempdir().unwrap();
        let private = Spool::open(state_only.path(), 4096, 8192, 1024, identity()).unwrap();
        drop(private);
        fs::remove_file(state_only.path().join(SPOOL_IDENTITY_FILE)).unwrap();
        assert!(state_only.path().join("state-a.bin").is_file());
        assert!(matches!(
            Spool::open(state_only.path(), 4096, 8192, 1024, public_identity()),
            Err(SpoolError::Identity { .. })
        ));
        assert!(!state_only.path().join(SPOOL_IDENTITY_FILE).exists());

        let with_wal = tempfile::tempdir().unwrap();
        let mut private = Spool::open(with_wal.path(), 4096, 8192, 1024, identity()).unwrap();
        private.append(b"legacy private record").unwrap();
        drop(private);
        fs::remove_file(with_wal.path().join(SPOOL_IDENTITY_FILE)).unwrap();
        assert_eq!(segment_paths(with_wal.path()).unwrap().len(), 1);
        assert!(matches!(
            Spool::open(with_wal.path(), 4096, 8192, 1024, public_identity()),
            Err(SpoolError::Identity { .. })
        ));
        assert!(!with_wal.path().join(SPOOL_IDENTITY_FILE).exists());
    }

    #[test]
    fn private_scope_migrates_only_valid_legacy_spool_content() {
        let legacy = tempfile::tempdir().unwrap();
        let mut original = Spool::open(legacy.path(), 4096, 8192, 1024, identity()).unwrap();
        original.append(b"legacy private record").unwrap();
        drop(original);
        fs::remove_file(legacy.path().join(SPOOL_IDENTITY_FILE)).unwrap();

        let migrated = Spool::open(legacy.path(), 4096, 8192, 1024, identity()).unwrap();
        assert_eq!(
            migrated.read_pending(10, 1024).unwrap()[0].payload,
            b"legacy private record"
        );
        assert!(legacy.path().join(SPOOL_IDENTITY_FILE).is_file());
        drop(migrated);

        let unrelated = tempfile::tempdir().unwrap();
        fs::write(unrelated.path().join("unrelated.txt"), b"not a gateway WAL").unwrap();
        assert!(matches!(
            Spool::open(unrelated.path(), 4096, 8192, 1024, identity()),
            Err(SpoolError::Identity { .. })
        ));
        assert!(!unrelated.path().join(SPOOL_IDENTITY_FILE).exists());
    }

    #[cfg(unix)]
    #[test]
    fn spool_identity_metadata_rejects_symlinks() {
        use std::os::unix::fs::symlink;

        let temp = tempfile::tempdir().unwrap();
        let first = Spool::open(temp.path(), 4096, 8192, 1024, identity()).unwrap();
        drop(first);
        let path = temp.path().join(SPOOL_IDENTITY_FILE);
        let target = temp.path().join("identity-target.json");
        fs::rename(&path, &target).unwrap();
        symlink(target, path).unwrap();
        assert!(matches!(
            Spool::open(temp.path(), 4096, 8192, 1024, identity()),
            Err(SpoolError::Identity { .. })
        ));
    }
}
