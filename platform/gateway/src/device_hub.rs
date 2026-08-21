use std::{
    collections::{HashMap, VecDeque},
    sync::Arc,
};

use sha2::{Digest, Sha256};
use tokio::sync::{mpsc, RwLock};
use uuid::Uuid;

const RECENT_ACTION_IDS: usize = 64;

#[derive(Debug, Clone)]
pub struct DeviceCommand {
    pub action_id: Uuid,
    pub bytes: Arc<[u8]>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RouteOutcome {
    Queued,
    DuplicateSuppressed,
    Offline,
    Backpressured,
    ConflictingDuplicate,
}

#[derive(Clone, Default)]
pub struct DeviceHub {
    inner: Arc<RwLock<HashMap<Uuid, Session>>>,
}

struct Session {
    id: Uuid,
    sender: mpsc::Sender<DeviceCommand>,
    recent_order: VecDeque<Uuid>,
    recent_digests: HashMap<Uuid, [u8; 32]>,
}

impl DeviceHub {
    pub fn new() -> Self {
        Self::default()
    }

    /// Registers an authenticated LAN session using the companion UUID bound
    /// into the verified client-certificate SAN. A newer connection replaces
    /// the old route; generation-safe unregister prevents the old task from
    /// deleting the replacement.
    pub async fn register(&self, companion_id: Uuid, sender: mpsc::Sender<DeviceCommand>) -> Uuid {
        let id = Uuid::new_v4();
        self.inner.write().await.insert(
            companion_id,
            Session {
                id,
                sender,
                recent_order: VecDeque::with_capacity(RECENT_ACTION_IDS),
                recent_digests: HashMap::with_capacity(RECENT_ACTION_IDS),
            },
        );
        id
    }

    pub async fn unregister(&self, companion_id: Uuid, session_id: Uuid) {
        let mut sessions = self.inner.write().await;
        if sessions
            .get(&companion_id)
            .is_some_and(|session| session.id == session_id)
        {
            sessions.remove(&companion_id);
        }
    }

    pub async fn route(
        &self,
        companion_id: Uuid,
        action_id: Uuid,
        bytes: Arc<[u8]>,
    ) -> RouteOutcome {
        let digest: [u8; 32] = Sha256::digest(bytes.as_ref()).into();
        let mut sessions = self.inner.write().await;
        let Some(session) = sessions.get_mut(&companion_id) else {
            return RouteOutcome::Offline;
        };
        if let Some(previous) = session.recent_digests.get(&action_id) {
            return if previous == &digest {
                RouteOutcome::DuplicateSuppressed
            } else {
                RouteOutcome::ConflictingDuplicate
            };
        }
        match session.sender.try_send(DeviceCommand { action_id, bytes }) {
            Ok(()) => {
                session.recent_order.push_back(action_id);
                session.recent_digests.insert(action_id, digest);
                if session.recent_order.len() > RECENT_ACTION_IDS {
                    if let Some(expired) = session.recent_order.pop_front() {
                        session.recent_digests.remove(&expired);
                    }
                }
                RouteOutcome::Queued
            }
            Err(mpsc::error::TrySendError::Full(_)) => RouteOutcome::Backpressured,
            Err(mpsc::error::TrySendError::Closed(_)) => {
                sessions.remove(&companion_id);
                RouteOutcome::Offline
            }
        }
    }

    pub async fn online_count(&self) -> usize {
        self.inner.read().await.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn routes_only_to_the_current_session_and_deduplicates_exact_bytes() {
        let hub = DeviceHub::new();
        let companion = Uuid::new_v4();
        let action = Uuid::new_v4();
        let (sender, mut receiver) = mpsc::channel(2);
        let session = hub.register(companion, sender).await;
        let body: Arc<[u8]> = Arc::from(b"signed-action".as_slice());

        assert_eq!(
            hub.route(companion, action, body.clone()).await,
            RouteOutcome::Queued
        );
        assert_eq!(
            receiver.recv().await.unwrap().bytes.as_ref(),
            b"signed-action"
        );
        assert_eq!(
            hub.route(companion, action, body).await,
            RouteOutcome::DuplicateSuppressed
        );
        assert_eq!(
            hub.route(companion, action, Arc::from(b"changed".as_slice()))
                .await,
            RouteOutcome::ConflictingDuplicate
        );

        hub.unregister(companion, session).await;
        assert_eq!(
            hub.route(companion, Uuid::new_v4(), Arc::from(b"later".as_slice()))
                .await,
            RouteOutcome::Offline
        );
    }

    #[tokio::test]
    async fn stale_unregister_cannot_remove_a_replacement_session() {
        let hub = DeviceHub::new();
        let companion = Uuid::new_v4();
        let (old_sender, _old_receiver) = mpsc::channel(1);
        let old = hub.register(companion, old_sender).await;
        let (new_sender, mut new_receiver) = mpsc::channel(1);
        let new = hub.register(companion, new_sender).await;

        hub.unregister(companion, old).await;
        assert_eq!(hub.online_count().await, 1);
        let action = Uuid::new_v4();
        assert_eq!(
            hub.route(companion, action, Arc::from(b"action".as_slice()))
                .await,
            RouteOutcome::Queued
        );
        assert_eq!(new_receiver.recv().await.unwrap().action_id, action);
        hub.unregister(companion, new).await;
        assert_eq!(hub.online_count().await, 0);
    }

    #[tokio::test]
    async fn closed_or_full_sessions_fail_without_recording_a_fake_delivery() {
        let hub = DeviceHub::new();
        let companion = Uuid::new_v4();
        let (sender, receiver) = mpsc::channel(1);
        let _session = hub.register(companion, sender).await;
        assert_eq!(
            hub.route(companion, Uuid::new_v4(), Arc::from(b"first".as_slice()))
                .await,
            RouteOutcome::Queued
        );
        assert_eq!(
            hub.route(companion, Uuid::new_v4(), Arc::from(b"second".as_slice()))
                .await,
            RouteOutcome::Backpressured
        );
        drop(receiver);
        assert_eq!(
            hub.route(companion, Uuid::new_v4(), Arc::from(b"third".as_slice()))
                .await,
            RouteOutcome::Offline
        );
        assert_eq!(hub.online_count().await, 0);
    }
}
