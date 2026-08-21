use std::{io, time::Duration};

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

#[derive(Debug, thiserror::Error)]
pub enum FrameError {
    #[error("peer closed the stream")]
    Closed,
    #[error("frame length {actual} exceeds limit {limit}")]
    TooLarge { actual: usize, limit: usize },
    #[error("frame assembly timed out")]
    Timeout,
    #[error(transparent)]
    Io(#[from] io::Error),
}

pub async fn read_frame<R: AsyncRead + Unpin>(
    stream: &mut R,
    max_bytes: usize,
    timeout: Duration,
) -> Result<Vec<u8>, FrameError> {
    let future = async {
        let mut size_bytes = [0_u8; 4];
        match stream.read_exact(&mut size_bytes).await {
            Ok(_) => {}
            Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => {
                return Err(FrameError::Closed)
            }
            Err(error) => return Err(FrameError::Io(error)),
        }
        let size = u32::from_be_bytes(size_bytes) as usize;
        if size == 0 || size > max_bytes {
            return Err(FrameError::TooLarge {
                actual: size,
                limit: max_bytes,
            });
        }
        let mut body = vec![0_u8; size];
        stream.read_exact(&mut body).await?;
        Ok(body)
    };

    tokio::time::timeout(timeout, future)
        .await
        .map_err(|_| FrameError::Timeout)?
}

pub async fn write_frame<W: AsyncWrite + Unpin>(
    stream: &mut W,
    body: &[u8],
) -> Result<(), FrameError> {
    let size = u32::try_from(body.len()).map_err(|_| FrameError::TooLarge {
        actual: body.len(),
        limit: u32::MAX as usize,
    })?;
    if size == 0 {
        return Err(FrameError::TooLarge {
            actual: 0,
            limit: u32::MAX as usize,
        });
    }
    stream.write_all(&size.to_be_bytes()).await?;
    stream.write_all(body).await?;
    stream.flush().await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn frame_round_trip() {
        let (mut writer, mut reader) = tokio::io::duplex(128);
        let task = tokio::spawn(async move { write_frame(&mut writer, b"hello").await.unwrap() });
        let frame = read_frame(&mut reader, 32, Duration::from_secs(1))
            .await
            .unwrap();
        task.await.unwrap();
        assert_eq!(frame, b"hello");
    }

    #[tokio::test]
    async fn rejects_oversize_before_allocation() {
        let bytes = 200_u32.to_be_bytes();
        let mut reader = &bytes[..];
        let error = read_frame(&mut reader, 64, Duration::from_secs(1))
            .await
            .unwrap_err();
        assert!(matches!(
            error,
            FrameError::TooLarge {
                actual: 200,
                limit: 64
            }
        ));
    }
}
