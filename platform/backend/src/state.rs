use std::{collections::HashMap, sync::Arc};

use metrics_exporter_prometheus::PrometheusHandle;
use serde_json::Value;
use tokio::sync::{mpsc, RwLock};
use uuid::Uuid;

use crate::{
    config::Config, db::Database, issuer::DynCertificateIssuer, kms::DynKms, oidc::OidcClient,
    wire::RemoteAction,
};

#[derive(Clone)]
pub struct AppState {
    pub config: Arc<Config>,
    pub db: Database,
    pub oidc: Arc<OidcClient>,
    pub kms: DynKms,
    pub certificate_issuer: DynCertificateIssuer,
    pub enrollment_ca_cert_der: Arc<Vec<u8>>,
    pub instance_id: Uuid,
    pub hubs: Arc<ConnectionHubs>,
    pub metrics: PrometheusHandle,
}

pub struct ConnectionHubs {
    gateways: RwLock<HashMap<Uuid, (Uuid, mpsc::UnboundedSender<RemoteAction>)>>,
    owners: RwLock<HashMap<Uuid, HashMap<Uuid, mpsc::UnboundedSender<Value>>>>,
}

impl ConnectionHubs {
    pub fn new() -> Self {
        Self {
            gateways: RwLock::new(HashMap::new()),
            owners: RwLock::new(HashMap::new()),
        }
    }

    pub async fn register_gateway(
        &self,
        gateway_id: Uuid,
    ) -> (Uuid, mpsc::UnboundedReceiver<RemoteAction>) {
        let connection_id = Uuid::new_v4();
        let (sender, receiver) = mpsc::unbounded_channel();
        self.gateways
            .write()
            .await
            .insert(gateway_id, (connection_id, sender));
        (connection_id, receiver)
    }

    pub async fn unregister_gateway(&self, gateway_id: Uuid, connection_id: Uuid) {
        let mut gateways = self.gateways.write().await;
        if gateways
            .get(&gateway_id)
            .is_some_and(|(active, _)| *active == connection_id)
        {
            gateways.remove(&gateway_id);
        }
    }

    /// Drops the live sender so an explicitly revoked gateway session exits.
    pub async fn disconnect_gateway(&self, gateway_id: Uuid) -> bool {
        self.gateways.write().await.remove(&gateway_id).is_some()
    }

    pub async fn send_gateway(&self, gateway_id: Uuid, action: RemoteAction) -> bool {
        self.gateways
            .read()
            .await
            .get(&gateway_id)
            .is_some_and(|(_, sender)| sender.send(action).is_ok())
    }

    pub async fn register_owner(&self, owner_id: Uuid) -> (Uuid, mpsc::UnboundedReceiver<Value>) {
        let connection_id = Uuid::new_v4();
        let (sender, receiver) = mpsc::unbounded_channel();
        self.owners
            .write()
            .await
            .entry(owner_id)
            .or_default()
            .insert(connection_id, sender);
        (connection_id, receiver)
    }

    pub async fn unregister_owner(&self, owner_id: Uuid, connection_id: Uuid) {
        let mut owners = self.owners.write().await;
        if let Some(connections) = owners.get_mut(&owner_id) {
            connections.remove(&connection_id);
            if connections.is_empty() {
                owners.remove(&owner_id);
            }
        }
    }

    pub async fn broadcast_owner(&self, owner_id: Uuid, event: Value) {
        let mut failed = Vec::new();
        {
            let owners = self.owners.read().await;
            if let Some(connections) = owners.get(&owner_id) {
                for (id, sender) in connections {
                    if sender.send(event.clone()).is_err() {
                        failed.push(*id);
                    }
                }
            }
        }
        if !failed.is_empty() {
            let mut owners = self.owners.write().await;
            if let Some(connections) = owners.get_mut(&owner_id) {
                for id in failed {
                    connections.remove(&id);
                }
            }
        }
    }
}

impl Default for ConnectionHubs {
    fn default() -> Self {
        Self::new()
    }
}
