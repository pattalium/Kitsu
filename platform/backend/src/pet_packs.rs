use std::{collections::HashMap, fs, path::Path, sync::Arc};

use anyhow::{bail, Context};

use crate::crypto::sha256;

const PACK_MAGIC: &[u8; 8] = b"K868PK1\0";
const PACK_HEADER_BYTES: usize = 64;
const MAXIMUM_PACK_BYTES: u64 = 2 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PetPackCatalogEntry {
    pub pack_id: u32,
    pub slug: &'static str,
    pub display_name: &'static str,
    pub rarity: &'static str,
    pub download_available: bool,
}

pub const PET_PACK_CATALOG: &[PetPackCatalogEntry] = &[
    entry(0x5CAC86A3, "frog", "Frog", "common", true),
    entry(0x13793DC7, "hamster", "Hamster", "common", true),
    entry(0x7495DBFB, "turtle", "Turtle", "common", true),
    entry(0x68D9554E, "rabbit", "Rabbit", "uncommon", true),
    entry(0x5DF6BE74, "hedgehog", "Hedgehog", "uncommon", true),
    entry(0xE59408E0, "ferret", "Ferret", "uncommon", true),
    entry(0x29B4B2F7, "otter", "Otter", "rare", true),
    entry(0x69276D0C, "axolotl", "Axolotl", "rare", true),
    entry(0x2DFB0797, "chinchilla", "Chinchilla", "rare", true),
    entry(0xC163EFED, "raccoon", "Raccoon", "very_rare", true),
    entry(0x374D2540, "capybara", "Capybara", "very_rare", true),
    entry(0x39FC5B1A, "sugar_glider", "Sugar Glider", "very_rare", true),
    entry(0x91A2DE7B, "red_panda", "Red Panda", "epic", true),
    entry(0xE04EC405, "pangolin", "Pangolin", "epic", true),
    entry(0x8E0E1B03, "tasmanian_devil", "Tasmanian Devil", "epic", true),
    entry(0x533B9B30, "snow_leopard", "Snow Leopard", "legendary", true),
    entry(0x86F3BB5D, "okapi", "Okapi", "legendary", true),
    entry(0x2D1D89AF, "shoebill", "Shoebill", "legendary", true),
    entry(0xA52160C5, "cat_girl", "Cat Girl", "mythical", true),
    entry(0xF0F750BD, "rabbit_girl", "Rabbit Girl", "mythical", true),
    entry(0x52A1C03A, "deer_girl", "Deer Girl", "mythical", true),
];

const fn entry(
    pack_id: u32,
    slug: &'static str,
    display_name: &'static str,
    rarity: &'static str,
    download_available: bool,
) -> PetPackCatalogEntry {
    PetPackCatalogEntry {
        pack_id,
        slug,
        display_name,
        rarity,
        download_available,
    }
}

pub struct PetPack {
    pub catalog: PetPackCatalogEntry,
    pub bytes: Arc<[u8]>,
    pub sha256: [u8; 32],
}

pub struct PetPackStore {
    packs: HashMap<u32, PetPack>,
}

impl PetPackStore {
    /// Loads the complete unlock catalogue into memory from a directory that
    /// is readable by the backend service but is outside every nginx root.
    /// A missing or malformed pack prevents startup instead of silently
    /// exposing an unlock that cannot be delivered.
    pub fn load(directory: &Path) -> anyhow::Result<Self> {
        let metadata = fs::symlink_metadata(directory).with_context(|| {
            format!("inspect private pet-pack directory {}", directory.display())
        })?;
        if !directory.is_absolute() || metadata.file_type().is_symlink() || !metadata.is_dir() {
            bail!("private pet-pack path must be an absolute, non-symlink directory");
        }
        ensure_private_permissions(&metadata, true)
            .context("private pet-pack directory permissions")?;

        let mut packs = HashMap::with_capacity(PET_PACK_CATALOG.len());
        for catalog in PET_PACK_CATALOG {
            let path = directory.join(format!("{}.k868", catalog.slug));
            let metadata = fs::symlink_metadata(&path)
                .with_context(|| format!("missing private pet pack {}", catalog.slug))?;
            if metadata.file_type().is_symlink()
                || !metadata.is_file()
                || metadata.len() < PACK_HEADER_BYTES as u64
                || metadata.len() > MAXIMUM_PACK_BYTES
            {
                bail!(
                    "private pet pack {} is not a bounded regular file",
                    catalog.slug
                );
            }
            ensure_private_permissions(&metadata, false)
                .with_context(|| format!("private pet-pack permissions for {}", catalog.slug))?;
            let bytes = fs::read(&path)
                .with_context(|| format!("read private pet pack {}", catalog.slug))?;
            validate_pack(&bytes, catalog.pack_id)
                .with_context(|| format!("validate private pet pack {}", catalog.slug))?;
            packs.insert(
                catalog.pack_id,
                PetPack {
                    catalog: *catalog,
                    sha256: sha256(&bytes),
                    bytes: Arc::from(bytes),
                },
            );
        }
        Ok(Self { packs })
    }

    pub fn get(&self, pack_id: u32) -> Option<&PetPack> {
        self.packs.get(&pack_id)
    }

    pub fn len(&self) -> usize {
        self.packs.len()
    }
}

#[cfg(unix)]
fn ensure_private_permissions(metadata: &fs::Metadata, directory: bool) -> anyhow::Result<()> {
    use std::os::unix::fs::PermissionsExt;

    let mode = metadata.permissions().mode();
    if directory {
        // Owner and read-only service-group access are permitted. The pack
        // tree must not be group-writable or accessible by other users.
        if mode & 0o027 != 0 {
            bail!("directory must not be group-writable or world-accessible");
        }
    } else if mode & 0o137 != 0 {
        // Regular pack data may be owner-writable and group-readable, but is
        // never executable, group-writable, or world-accessible.
        bail!("file must be non-executable, non-group-writable, and private");
    }
    Ok(())
}

#[cfg(not(unix))]
fn ensure_private_permissions(_metadata: &fs::Metadata, _directory: bool) -> anyhow::Result<()> {
    Ok(())
}

pub fn catalog_entry(pack_id: u32) -> Option<PetPackCatalogEntry> {
    PET_PACK_CATALOG
        .iter()
        .copied()
        .find(|entry| entry.pack_id == pack_id)
}

pub fn downloadable_catalog_entry(pack_id: u32) -> Option<PetPackCatalogEntry> {
    catalog_entry(pack_id).filter(|entry| entry.download_available)
}

fn little_u16(input: &[u8], offset: usize) -> anyhow::Result<u16> {
    Ok(u16::from_le_bytes(
        input
            .get(offset..offset + 2)
            .context("truncated u16")?
            .try_into()
            .expect("two-byte slice"),
    ))
}

fn little_u32(input: &[u8], offset: usize) -> anyhow::Result<u32> {
    Ok(u32::from_le_bytes(
        input
            .get(offset..offset + 4)
            .context("truncated u32")?
            .try_into()
            .expect("four-byte slice"),
    ))
}

fn validate_pack(input: &[u8], expected_pack_id: u32) -> anyhow::Result<()> {
    if input.len() < PACK_HEADER_BYTES || input.get(..8) != Some(PACK_MAGIC.as_slice()) {
        bail!("invalid K868PK1 header");
    }
    const EXPECTED_PACK_BYTES: usize = PACK_HEADER_BYTES + (12 * 12) + (48 * 4) + (48 * 512);
    if little_u16(input, 8)? != 1
        || usize::from(little_u16(input, 10)?) != PACK_HEADER_BYTES
        || usize::try_from(little_u32(input, 12)?)? != input.len()
        || input.len() != EXPECTED_PACK_BYTES
        || little_u32(input, 24)? != expected_pack_id
        || little_u32(input, 28)? == 0
        || little_u16(input, 32)? != 64
        || little_u16(input, 34)? != 64
        || little_u16(input, 36)? != 48
        || little_u16(input, 38)? != 12
        || little_u32(input, 40)? != 48
        || little_u32(input, 44)? != 0
    {
        bail!("unsupported K868PK1 structure");
    }
    let display_name = input.get(48..64).context("truncated display name")?;
    let terminator = display_name
        .iter()
        .position(|byte| *byte == 0)
        .context("unterminated display name")?;
    if terminator == 0
        || !display_name[..terminator]
            .iter()
            .all(|byte| matches!(byte, 0x20..=0x7e))
        || display_name[terminator..].iter().any(|byte| *byte != 0)
    {
        bail!("invalid K868PK1 display name");
    }

    let payload_crc = little_u32(input, 16)?;
    if crc32(&input[PACK_HEADER_BYTES..]) != payload_crc {
        bail!("K868PK1 payload CRC mismatch");
    }
    let header_crc = little_u32(input, 20)?;
    let mut header = input[8..PACK_HEADER_BYTES].to_vec();
    header[12..16].fill(0);
    if crc32(&header) != header_crc {
        bail!("K868PK1 header CRC mismatch");
    }
    Ok(())
}

pub fn crc32(input: &[u8]) -> u32 {
    let mut value = 0xffff_ffff_u32;
    for byte in input {
        value ^= u32::from(*byte);
        for _ in 0..8 {
            value = (value >> 1) ^ (0xedb8_8320_u32 & (0_u32.wrapping_sub(value & 1)));
        }
    }
    !value
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_pack(pack_id: u32) -> Vec<u8> {
        let mut bytes = vec![0_u8; PACK_HEADER_BYTES + (12 * 12) + (48 * 4) + (48 * 512)];
        bytes[..8].copy_from_slice(PACK_MAGIC);
        bytes[8..10].copy_from_slice(&1_u16.to_le_bytes());
        bytes[10..12].copy_from_slice(&(PACK_HEADER_BYTES as u16).to_le_bytes());
        let total = u32::try_from(bytes.len()).unwrap();
        bytes[12..16].copy_from_slice(&total.to_le_bytes());
        bytes[24..28].copy_from_slice(&pack_id.to_le_bytes());
        bytes[28..32].copy_from_slice(&2_u32.to_le_bytes());
        bytes[32..34].copy_from_slice(&64_u16.to_le_bytes());
        bytes[34..36].copy_from_slice(&64_u16.to_le_bytes());
        bytes[36..38].copy_from_slice(&48_u16.to_le_bytes());
        bytes[38..40].copy_from_slice(&12_u16.to_le_bytes());
        bytes[40..44].copy_from_slice(&48_u32.to_le_bytes());
        bytes[48..53].copy_from_slice(b"Frog\0");
        let payload_crc = crc32(&bytes[PACK_HEADER_BYTES..]);
        bytes[16..20].copy_from_slice(&payload_crc.to_le_bytes());
        let mut header = bytes[8..PACK_HEADER_BYTES].to_vec();
        header[12..16].fill(0);
        let header_crc = crc32(&header);
        bytes[20..24].copy_from_slice(&header_crc.to_le_bytes());
        bytes
    }

    #[test]
    fn validates_exact_pack_identity_and_both_crcs() {
        let pack = test_pack(0x5CAC86A3);
        assert!(validate_pack(&pack, 0x5CAC86A3).is_ok());
        assert!(validate_pack(&pack, 0x13793DC7).is_err());

        let mut corrupt = pack;
        *corrupt.last_mut().unwrap() ^= 1;
        assert!(validate_pack(&corrupt, 0x5CAC86A3).is_err());
    }

    #[test]
    fn public_catalog_excludes_starter_packs_and_owner_private_pack() {
        let owner_private_slug = ["fox", "girl"].join("_");
        assert_eq!(PET_PACK_CATALOG.len(), 21);
        assert!(catalog_entry(0x5CAC86A3).is_some());
        assert!(PET_PACK_CATALOG.iter().all(|pack| {
            !matches!(pack.pack_id, 0xFDC79D6F | 0x6C393E21 | 0xE2B5E7BA)
                && pack.slug != owner_private_slug.as_str()
        }));
    }

    #[test]
    fn visually_accepted_catalog_is_fully_downloadable() {
        assert_eq!(PET_PACK_CATALOG.len(), 21);
        for entry in PET_PACK_CATALOG {
            assert!(entry.download_available);
            assert_eq!(downloadable_catalog_entry(entry.pack_id), Some(*entry));
        }
    }
}
