use std::{path::PathBuf, str::FromStr};

use anyhow::{bail, Context};
use clap::Parser;
use kitsu_platform_backend::{
    db::Database,
    persistence::{postgres::PgPoolOptions, query, query_scalar},
};
use url::Url;
use uuid::Uuid;
use zeroize::Zeroizing;

const EXPECTED_MIGRATION_COUNT: i64 = 10;

#[derive(Parser)]
#[command(about = "Apply or preflight the embedded Kitsu PostgreSQL migrations")]
struct Arguments {
    #[arg(long, value_name = "PATH")]
    database_url_file: PathBuf,
    #[arg(long, default_value_t = 4)]
    max_connections: u32,
    /// Apply every migration to a disposable schema and remove it again.
    #[arg(long)]
    preflight: bool,
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let arguments = Arguments::parse();
    if !(1..=16).contains(&arguments.max_connections) {
        bail!("migration connection limit must be between 1 and 16");
    }
    let raw = std::fs::read_to_string(&arguments.database_url_file)
        .context("read database credential")?;
    let database_url = Zeroizing::new(raw.trim_end_matches(['\r', '\n']).to_owned());
    if database_url.is_empty() || database_url.chars().any(|character| character.is_control()) {
        bail!("database credential is empty or malformed");
    }
    let parsed = Url::from_str(&database_url).context("database credential is not a URL")?;
    if !matches!(parsed.scheme(), "postgres" | "postgresql") {
        bail!("database credential is not PostgreSQL");
    }

    if arguments.preflight {
        preflight(&database_url, arguments.max_connections).await?;
        println!(
            "all {EXPECTED_MIGRATION_COUNT} embedded migrations passed disposable-schema preflight"
        );
    } else {
        let database = Database::connect(&database_url, arguments.max_connections)
            .await
            .context("apply production migrations")?;
        verify_count(&database).await?;
        database.pool().close().await;
        println!("all {EXPECTED_MIGRATION_COUNT} embedded migrations are applied");
    }
    Ok(())
}

async fn preflight(database_url: &str, max_connections: u32) -> anyhow::Result<()> {
    let admin = PgPoolOptions::new()
        .max_connections(1)
        .connect(database_url)
        .await
        .context("connect for migration preflight")?;
    let schema = format!("kitsu_migration_preflight_{}", Uuid::new_v4().simple());
    query(&format!("CREATE SCHEMA \"{schema}\""))
        .execute(&admin)
        .await
        .context("create migration preflight schema")?;

    let mut scoped = Url::parse(database_url).context("parse migration database URL")?;
    scoped
        .query_pairs_mut()
        // PostgreSQL URI's supported way to set a startup GUC. SQLx/tokio-
        // postgres reject the Rails-style `options[search_path]` spelling.
        .append_pair("options", &format!("-csearch_path={schema}"));
    let result = async {
        let database = Database::connect(scoped.as_str(), max_connections)
            .await
            .context("apply migrations in disposable schema")?;
        verify_count(&database).await?;
        database.pool().close().await;
        anyhow::Ok(())
    }
    .await;

    // The schema name is generated from a UUID and cannot contain SQL syntax.
    let cleanup = query(&format!("DROP SCHEMA \"{schema}\" CASCADE"))
        .execute(&admin)
        .await
        .context("drop migration preflight schema");
    admin.close().await;
    result?;
    cleanup?;
    Ok(())
}

async fn verify_count(database: &Database) -> anyhow::Result<()> {
    let count: i64 = query_scalar("SELECT count(*) FROM _sqlx_migrations")
        .fetch_one(database.pool())
        .await
        .context("read applied migration count")?;
    if count != EXPECTED_MIGRATION_COUNT {
        bail!(
            "embedded migration count mismatch: expected {EXPECTED_MIGRATION_COUNT}, got {count}"
        );
    }
    Ok(())
}
