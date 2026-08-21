#[cfg(feature = "aws-kms")]
use std::collections::HashMap;
use std::sync::Arc;
#[cfg(feature = "local-kms")]
use std::{collections::BTreeMap, fs, path::Path};

#[cfg(feature = "local-kms")]
use aes_gcm::{
    aead::{Aead, KeyInit, Payload},
    Aes256Gcm, Nonce,
};
use async_trait::async_trait;
use uuid::Uuid;
#[cfg(any(feature = "aws-kms", feature = "local-kms", test, feature = "test-kms"))]
use zeroize::Zeroize;
use zeroize::Zeroizing;

use crate::error::ApiError;

pub struct DataKey {
    pub plaintext: Zeroizing<[u8; 32]>,
    pub wrapped: Vec<u8>,
    pub kms_key_id: String,
}

#[async_trait]
pub trait KmsProvider: Send + Sync {
    async fn health(&self) -> Result<(), ApiError> {
        Ok(())
    }

    async fn generate_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
    ) -> Result<DataKey, ApiError>;

    async fn decrypt_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
        kms_key_id: &str,
        wrapped: &[u8],
    ) -> Result<Zeroizing<[u8; 32]>, ApiError>;
}

pub type DynKms = Arc<dyn KmsProvider>;

#[cfg(feature = "local-kms")]
const LOCAL_WRAP_VERSION: u8 = 1;

#[cfg(feature = "local-kms")]
pub struct LocalKmsProvider {
    current_id: String,
    keys: BTreeMap<String, Zeroizing<[u8; 32]>>,
}

#[cfg(feature = "local-kms")]
impl LocalKmsProvider {
    pub fn load(
        current: &crate::config::LocalKmsKeyConfig,
        previous: &[crate::config::LocalKmsKeyConfig],
    ) -> anyhow::Result<Self> {
        let mut keys = BTreeMap::new();
        keys.insert(current.id.clone(), read_raw_key(&current.path)?);
        for item in previous {
            if keys
                .insert(item.id.clone(), read_raw_key(&item.path)?)
                .is_some()
            {
                anyhow::bail!("duplicate local KMS key ID");
            }
        }
        Ok(Self {
            current_id: current.id.clone(),
            keys,
        })
    }

    #[cfg(test)]
    fn from_keys(current_id: &str, keys: impl IntoIterator<Item = (String, [u8; 32])>) -> Self {
        Self {
            current_id: current_id.to_owned(),
            keys: keys
                .into_iter()
                .map(|(id, key)| (id, Zeroizing::new(key)))
                .collect(),
        }
    }
}

#[cfg(feature = "local-kms")]
fn read_raw_key(path: &Path) -> anyhow::Result<Zeroizing<[u8; 32]>> {
    let mut bytes = Zeroizing::new(
        fs::read(path).map_err(|error| anyhow::anyhow!("read local KMS credential: {error}"))?,
    );
    let key: [u8; 32] = bytes
        .as_slice()
        .try_into()
        .map_err(|_| anyhow::anyhow!("local KMS credential must contain exactly 32 raw bytes"))?;
    bytes.zeroize();
    Ok(Zeroizing::new(key))
}

#[cfg(feature = "local-kms")]
fn local_key_id(id: &str) -> String {
    format!("local-aes256-gcm:{id}")
}

#[cfg(feature = "local-kms")]
fn local_aad(companion_id: Uuid, key_version: u32, key_id: &str) -> Vec<u8> {
    let mut aad = Vec::with_capacity(64 + key_id.len());
    aad.extend_from_slice(b"KITSU-LOCAL-KMS-WRAP-1\0");
    aad.extend_from_slice(companion_id.as_bytes());
    aad.extend_from_slice(&key_version.to_be_bytes());
    aad.extend_from_slice(&(key_id.len() as u32).to_be_bytes());
    aad.extend_from_slice(key_id.as_bytes());
    aad
}

#[cfg(feature = "local-kms")]
#[async_trait]
impl KmsProvider for LocalKmsProvider {
    async fn health(&self) -> Result<(), ApiError> {
        self.keys
            .get(&self.current_id)
            .filter(|key| key.len() == 32)
            .map(|_| ())
            .ok_or(ApiError::Unavailable)
    }

    async fn generate_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
    ) -> Result<DataKey, ApiError> {
        let plaintext = crate::crypto::random_array::<32>();
        let key = self.keys.get(&self.current_id).ok_or_else(|| {
            tracing::error!("local KMS current key is unavailable");
            ApiError::Unavailable
        })?;
        let nonce = crate::crypto::random_array::<12>();
        let kms_key_id = local_key_id(&self.current_id);
        let ciphertext = Aes256Gcm::new_from_slice(key.as_slice())
            .map_err(|_| ApiError::Unavailable)?
            .encrypt(
                Nonce::from_slice(&nonce),
                Payload {
                    msg: &plaintext,
                    aad: &local_aad(companion_id, key_version, &kms_key_id),
                },
            )
            .map_err(|_| ApiError::Unavailable)?;
        let mut wrapped = Vec::with_capacity(1 + nonce.len() + ciphertext.len());
        wrapped.push(LOCAL_WRAP_VERSION);
        wrapped.extend_from_slice(&nonce);
        wrapped.extend_from_slice(&ciphertext);
        Ok(DataKey {
            plaintext: Zeroizing::new(plaintext),
            wrapped,
            kms_key_id,
        })
    }

    async fn decrypt_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
        kms_key_id: &str,
        wrapped: &[u8],
    ) -> Result<Zeroizing<[u8; 32]>, ApiError> {
        let id = kms_key_id
            .strip_prefix("local-aes256-gcm:")
            .ok_or(ApiError::Unavailable)?;
        let key = self.keys.get(id).ok_or_else(|| {
            tracing::error!(key_id = %id, "local KMS key version is unavailable");
            ApiError::Unavailable
        })?;
        if wrapped.len() != 1 + 12 + 32 + 16 || wrapped[0] != LOCAL_WRAP_VERSION {
            return Err(ApiError::Unavailable);
        }
        let plaintext = Aes256Gcm::new_from_slice(key.as_slice())
            .map_err(|_| ApiError::Unavailable)?
            .decrypt(
                Nonce::from_slice(&wrapped[1..13]),
                Payload {
                    msg: &wrapped[13..],
                    aad: &local_aad(companion_id, key_version, kms_key_id),
                },
            )
            .map_err(|_| ApiError::Unavailable)?;
        let key: [u8; 32] = plaintext
            .as_slice()
            .try_into()
            .map_err(|_| ApiError::Unavailable)?;
        Ok(Zeroizing::new(key))
    }
}

#[cfg(feature = "aws-kms")]
fn encryption_context(companion_id: Uuid, key_version: u32) -> HashMap<String, String> {
    HashMap::from([
        ("purpose".into(), "kitsu-companion-secret".into()),
        ("companion_id".into(), companion_id.to_string()),
        ("key_version".into(), key_version.to_string()),
        ("crypto_version".into(), "1".into()),
    ])
}

#[cfg(feature = "aws-kms")]
pub struct AwsKmsProvider {
    client: aws_sdk_kms::Client,
    key_id: String,
}

#[cfg(feature = "aws-kms")]
impl AwsKmsProvider {
    pub async fn new(key_id: String) -> Self {
        let config = aws_config::load_defaults(aws_config::BehaviorVersion::latest()).await;
        Self {
            client: aws_sdk_kms::Client::new(&config),
            key_id,
        }
    }
}

#[cfg(feature = "aws-kms")]
#[async_trait]
impl KmsProvider for AwsKmsProvider {
    async fn health(&self) -> Result<(), ApiError> {
        self.client
            .describe_key()
            .key_id(&self.key_id)
            .send()
            .await
            .map(|_| ())
            .map_err(|error| {
                tracing::error!(error = %error, "KMS readiness check failed");
                ApiError::Unavailable
            })
    }

    async fn generate_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
    ) -> Result<DataKey, ApiError> {
        use aws_sdk_kms::types::DataKeySpec;

        let response = self
            .client
            .generate_data_key()
            .key_id(&self.key_id)
            .key_spec(DataKeySpec::Aes256)
            .set_encryption_context(Some(encryption_context(companion_id, key_version)))
            .send()
            .await
            .map_err(|error| {
                tracing::error!(error = %error, "KMS GenerateDataKey failed");
                ApiError::Unavailable
            })?;
        let plaintext_blob = response.plaintext().ok_or(ApiError::Unavailable)?;
        let ciphertext_blob = response.ciphertext_blob().ok_or(ApiError::Unavailable)?;
        let mut temporary = Zeroizing::new(plaintext_blob.as_ref().to_vec());
        let plaintext: [u8; 32] = temporary
            .as_slice()
            .try_into()
            .map_err(|_| ApiError::Unavailable)?;
        temporary.zeroize();
        Ok(DataKey {
            plaintext: Zeroizing::new(plaintext),
            wrapped: ciphertext_blob.as_ref().to_vec(),
            kms_key_id: response
                .key_id()
                .map(ToOwned::to_owned)
                .unwrap_or_else(|| self.key_id.clone()),
        })
    }

    async fn decrypt_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
        kms_key_id: &str,
        wrapped: &[u8],
    ) -> Result<Zeroizing<[u8; 32]>, ApiError> {
        use aws_sdk_kms::primitives::Blob;

        let response = self
            .client
            .decrypt()
            .key_id(kms_key_id)
            .ciphertext_blob(Blob::new(wrapped))
            .set_encryption_context(Some(encryption_context(companion_id, key_version)))
            .send()
            .await
            .map_err(|error| {
                tracing::error!(error = %error, "KMS Decrypt failed");
                ApiError::Unavailable
            })?;
        let blob = response.plaintext().ok_or(ApiError::Unavailable)?;
        let mut temporary = Zeroizing::new(blob.as_ref().to_vec());
        let plaintext: [u8; 32] = temporary
            .as_slice()
            .try_into()
            .map_err(|_| ApiError::Unavailable)?;
        temporary.zeroize();
        Ok(Zeroizing::new(plaintext))
    }
}

#[cfg(any(test, feature = "test-kms"))]
pub struct TestKmsProvider {
    key: Zeroizing<[u8; 32]>,
}

#[cfg(any(test, feature = "test-kms"))]
impl TestKmsProvider {
    pub fn new(key: [u8; 32]) -> Self {
        Self {
            key: Zeroizing::new(key),
        }
    }
}

#[cfg(any(test, feature = "test-kms"))]
#[async_trait]
impl KmsProvider for TestKmsProvider {
    async fn generate_data_key(
        &self,
        companion_id: Uuid,
        key_version: u32,
    ) -> Result<DataKey, ApiError> {
        use crate::crypto::{encrypt_browser_state, random_array};

        let plaintext = random_array::<32>();
        let encrypted = encrypt_browser_state(&self.key, companion_id, &plaintext)?;
        let mut wrapped = encrypted.nonce.to_vec();
        wrapped.extend_from_slice(&encrypted.ciphertext);
        Ok(DataKey {
            plaintext: Zeroizing::new(plaintext),
            wrapped,
            kms_key_id: format!("test-kms-v1:{key_version}"),
        })
    }

    async fn decrypt_data_key(
        &self,
        companion_id: Uuid,
        _key_version: u32,
        _kms_key_id: &str,
        wrapped: &[u8],
    ) -> Result<Zeroizing<[u8; 32]>, ApiError> {
        use crate::crypto::{decrypt_browser_state, EncryptedBytes};

        if wrapped.len() < 12 {
            return Err(ApiError::Unavailable);
        }
        let encrypted = EncryptedBytes {
            nonce: wrapped[..12]
                .try_into()
                .map_err(|_| ApiError::Unavailable)?,
            ciphertext: wrapped[12..].to_vec(),
        };
        let mut decoded = decrypt_browser_state(&self.key, companion_id, &encrypted)?;
        let key: [u8; 32] = decoded
            .as_slice()
            .try_into()
            .map_err(|_| ApiError::Unavailable)?;
        decoded.zeroize();
        Ok(Zeroizing::new(key))
    }
}

#[cfg(all(test, feature = "local-kms"))]
mod local_tests {
    use super::*;

    #[tokio::test]
    async fn local_wrapping_is_context_bound_and_supports_key_rotation() {
        let companion = Uuid::new_v4();
        let first = LocalKmsProvider::from_keys("v1", [("v1".to_owned(), [7_u8; 32])]);
        let data = first.generate_data_key(companion, 3).await.unwrap();
        assert_eq!(data.kms_key_id, "local-aes256-gcm:v1");
        assert_eq!(
            first
                .decrypt_data_key(companion, 3, &data.kms_key_id, &data.wrapped)
                .await
                .unwrap()
                .as_slice(),
            data.plaintext.as_slice()
        );
        assert!(first
            .decrypt_data_key(Uuid::new_v4(), 3, &data.kms_key_id, &data.wrapped)
            .await
            .is_err());
        assert!(first
            .decrypt_data_key(companion, 4, &data.kms_key_id, &data.wrapped)
            .await
            .is_err());

        let rotated = LocalKmsProvider::from_keys(
            "v2",
            [("v1".to_owned(), [7_u8; 32]), ("v2".to_owned(), [8_u8; 32])],
        );
        assert_eq!(
            rotated
                .decrypt_data_key(companion, 3, &data.kms_key_id, &data.wrapped)
                .await
                .unwrap()
                .as_slice(),
            data.plaintext.as_slice()
        );
        assert_eq!(
            rotated
                .generate_data_key(companion, 4)
                .await
                .unwrap()
                .kms_key_id,
            "local-aes256-gcm:v2"
        );
    }
}
