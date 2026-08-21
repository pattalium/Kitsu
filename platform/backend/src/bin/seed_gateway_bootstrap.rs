use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    time::Duration,
};

use anyhow::{bail, Context, Result};
use clap::Parser;
use kitsu_platform_backend::{
    crypto::{random_token, sha256_text},
    db::Database,
    oidc::OidcPrincipal,
};
use serde::Deserialize;
use uuid::Uuid;
use zeroize::{Zeroize, Zeroizing};

#[derive(Parser)]
#[command(about = "Create one root-authorized, one-use home-gateway bootstrap handoff")]
struct Args {
    #[arg(long)]
    database_url_file: PathBuf,
    #[arg(long)]
    owner_principal_file: PathBuf,
    #[arg(long)]
    output_dir: PathBuf,
    #[arg(long, default_value = "Kitsu Home Gateway")]
    display_name: String,
    #[arg(long, default_value_t = 900)]
    ttl_seconds: u64,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct OwnerPrincipalFile {
    schema: String,
    issuer: String,
    subject: String,
    username: String,
    #[serde(rename = "displayName")]
    display_name: String,
}

#[tokio::main]
async fn main() -> Result<()> {
    let args = Args::parse();
    if args.output_dir.exists() {
        bail!("refusing to replace an existing gateway-bootstrap handoff");
    }
    if !(60..=3_600).contains(&args.ttl_seconds) {
        bail!("gateway-bootstrap TTL must be between 60 and 3600 seconds");
    }
    let mut database_url = read_small_secret(&args.database_url_file, 8 * 1024)?;
    let principal_bytes = read_small_regular_file(&args.owner_principal_file, 8 * 1024)?;
    let principal: OwnerPrincipalFile =
        serde_json::from_slice(&principal_bytes).context("parse owner principal handoff")?;
    validate_principal(&principal)?;

    let database = Database::connect(database_url.trim_end(), 2)
        .await
        .context("connect to Kitsu PostgreSQL")?;
    database_url.zeroize();
    let owner = database
        .upsert_owner(&OidcPrincipal {
            issuer: principal.issuer,
            subject: principal.subject,
            email: None,
            display_name: Some(principal.display_name),
        })
        .await
        .context("seed the verified initial owner")?;
    let mut claim = random_token(32);
    let bootstrap = database
        .create_gateway_bootstrap(
            owner.id,
            &args.display_name,
            &sha256_text(&claim),
            Duration::from_secs(args.ttl_seconds),
        )
        .await
        .context("create one-use gateway bootstrap")?;

    publish_handoff(&args.output_dir, bootstrap.id, claim.as_str())?;
    claim.zeroize();
    println!("seeded one-use gateway bootstrap {}", bootstrap.id);
    Ok(())
}

fn validate_principal(principal: &OwnerPrincipalFile) -> Result<()> {
    let issuer = url::Url::parse(&principal.issuer).context("invalid owner issuer")?;
    if principal.schema != "kitsu.owner-principal.v1"
        || issuer.scheme() != "https"
        || issuer.as_str() != "https://auth.k32.run/realms/kitsu"
        || principal.subject.is_empty()
        || principal.subject.len() > 255
        || principal.username != "k32-owner"
        || principal.display_name != "K32 Owner"
    {
        bail!("owner principal does not satisfy the initial-owner contract");
    }
    Ok(())
}

fn read_small_secret(path: &Path, limit: u64) -> Result<Zeroizing<String>> {
    let bytes = read_small_regular_file(path, limit)?;
    let text = String::from_utf8(bytes).context("database credential is not UTF-8")?;
    if text.trim_end().is_empty() {
        bail!("database credential is empty");
    }
    Ok(Zeroizing::new(text))
}

fn read_small_regular_file(path: &Path, limit: u64) -> Result<Vec<u8>> {
    let metadata =
        fs::symlink_metadata(path).with_context(|| format!("inspect {}", path.display()))?;
    if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() > limit {
        bail!("input must be a bounded regular file");
    }
    fs::read(path).with_context(|| format!("read {}", path.display()))
}

fn publish_handoff(output_dir: &Path, bootstrap_id: Uuid, claim: &str) -> Result<()> {
    let parent = output_dir.parent().context("handoff path has no parent")?;
    let name = output_dir
        .file_name()
        .and_then(|name| name.to_str())
        .context("handoff directory name is invalid")?;
    let staging = parent.join(format!(".{name}.{}.tmp", Uuid::new_v4()));
    fs::create_dir(&staging).context("create gateway-bootstrap staging directory")?;
    set_mode(&staging, 0o700)?;
    let result = (|| -> Result<()> {
        write_new(
            &staging.join("bootstrap-id"),
            &format!("{bootstrap_id}\n"),
            0o600,
        )?;
        write_new(&staging.join("claim-token"), &format!("{claim}\n"), 0o600)?;
        OpenOptions::new()
            .read(true)
            .open(&staging)
            .context("open gateway-bootstrap staging directory")?
            .sync_all()
            .context("sync gateway-bootstrap staging directory")?;
        fs::rename(&staging, output_dir).context("publish gateway-bootstrap handoff")?;
        OpenOptions::new()
            .read(true)
            .open(parent)
            .context("open gateway-bootstrap parent directory")?
            .sync_all()
            .context("sync gateway-bootstrap parent directory")?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(&staging);
    }
    result
}

fn write_new(path: &Path, value: &str, mode: u32) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .with_context(|| format!("create {}", path.display()))?;
    set_mode(path, mode)?;
    file.write_all(value.as_bytes())
        .with_context(|| format!("write {}", path.display()))?;
    file.sync_all()
        .with_context(|| format!("sync {}", path.display()))
}

#[cfg(unix)]
fn set_mode(path: &Path, mode: u32) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(path, fs::Permissions::from_mode(mode))
        .with_context(|| format!("set permissions on {}", path.display()))
}

#[cfg(not(unix))]
fn set_mode(_path: &Path, _mode: u32) -> Result<()> {
    Ok(())
}
