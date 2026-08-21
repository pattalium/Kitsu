use axum::{
    http::StatusCode,
    response::{IntoResponse, Response},
    Json,
};
use serde::Serialize;

use crate::persistence as sqlx;

#[derive(Debug, thiserror::Error)]
pub enum ApiError {
    #[error("unauthorized")]
    Unauthorized,
    #[error("forbidden")]
    Forbidden,
    #[error("not found")]
    NotFound,
    #[error("conflict: {0}")]
    Conflict(&'static str),
    #[error("rate limited")]
    RateLimited,
    #[error("invalid request: {0}")]
    Invalid(&'static str),
    #[error("request expired")]
    Expired,
    #[error("certificate issuance is ambiguous; create a replacement claim")]
    ReplacementRequired,
    #[error("replayed or decreasing sequence")]
    Replay,
    #[error("temporarily unavailable")]
    Unavailable,
    #[error("internal service error")]
    Internal(#[source] anyhow::Error),
}

impl ApiError {
    pub fn internal(error: impl Into<anyhow::Error>) -> Self {
        Self::Internal(error.into())
    }

    pub fn status(&self) -> StatusCode {
        match self {
            Self::Unauthorized => StatusCode::UNAUTHORIZED,
            Self::Forbidden => StatusCode::FORBIDDEN,
            Self::NotFound => StatusCode::NOT_FOUND,
            Self::Conflict(_) | Self::Replay | Self::ReplacementRequired => StatusCode::CONFLICT,
            Self::RateLimited => StatusCode::TOO_MANY_REQUESTS,
            Self::Invalid(_) => StatusCode::BAD_REQUEST,
            Self::Expired => StatusCode::GONE,
            Self::Unavailable => StatusCode::SERVICE_UNAVAILABLE,
            Self::Internal(_) => StatusCode::INTERNAL_SERVER_ERROR,
        }
    }

    pub fn code(&self) -> &'static str {
        match self {
            Self::Unauthorized => "unauthorized",
            Self::Forbidden => "forbidden",
            Self::NotFound => "not_found",
            Self::Conflict(_) => "conflict",
            Self::RateLimited => "rate_limited",
            Self::Invalid(_) => "invalid_request",
            Self::Expired => "expired",
            Self::ReplacementRequired => "replacement_required",
            Self::Replay => "sequence_replay",
            Self::Unavailable => "temporarily_unavailable",
            Self::Internal(_) => "internal_error",
        }
    }

    fn public_message(&self) -> &str {
        match self {
            Self::Conflict(message) | Self::Invalid(message) => message,
            _ => self.code(),
        }
    }
}

#[derive(Serialize)]
struct ErrorBody<'a> {
    error: ErrorDescription<'a>,
}

#[derive(Serialize)]
struct ErrorDescription<'a> {
    code: &'a str,
    message: &'a str,
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        if let Self::Internal(ref source) = self {
            tracing::error!(error = %source, "request failed");
        }
        let status = self.status();
        let body = ErrorBody {
            error: ErrorDescription {
                code: self.code(),
                message: self.public_message(),
            },
        };
        (status, Json(body)).into_response()
    }
}

impl From<sqlx::Error> for ApiError {
    fn from(error: sqlx::Error) -> Self {
        Self::internal(error)
    }
}
