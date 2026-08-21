//! Narrow PostgreSQL-only SQLx surface used by this service.
//!
//! Keeping this adapter local avoids the umbrella crate and keeps every other
//! database driver out of both the active graph and Cargo.lock.

pub use sqlx_core::{
    error::Error, query::query, query_scalar::query_scalar, row::Row, transaction::Transaction,
};
pub use sqlx_postgres::{PgPool, Postgres};

pub mod postgres {
    pub use sqlx_postgres::{PgListener, PgPoolOptions, PgRow};
}

pub mod migrate {
    pub use sqlx_core::migrate::{Migration, MigrationType, Migrator};
}
