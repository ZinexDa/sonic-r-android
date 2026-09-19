use serde::{Deserialize, Serialize};
use std::net::SocketAddr;

pub type ServerId = uuid::Uuid;
pub type PunchToken = uuid::Uuid;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ServerStatus {
    #[default]
    InLobby,
    InRace,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ServerInfo {
    pub id: ServerId,
    pub name: String,
    pub players: u8,
    pub max_players: u8,
    pub game_version: String,
    #[serde(default)]
    pub status: ServerStatus,
}

impl ServerInfo {
    /// Returns whether this server is currently joinable (in lobby and has available player slots).
    pub fn is_joinable(&self) -> bool {
        self.status == ServerStatus::InLobby && self.players < self.max_players
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum ClientMessage {
    RegisterHost {
        name: String,
        max_players: u8,
        game_version: String,
        /// Optional UDP port hint. The actual public endpoint is derived
        /// from the UDP punch packet at the hub.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        udp_port: Option<u16>,
        punch_token: PunchToken,
    },
    ListServers,
    JoinRequest {
        server_id: ServerId,
        punch_token: PunchToken,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        udp_port: Option<u16>,
    },
    Heartbeat {
        players: Option<u8>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        status: Option<ServerStatus>,
    },
    RelayFallback {
        punch_token: PunchToken,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type")]
pub enum HubMessage {
    Registered { server_id: ServerId },
    ServerList { servers: Vec<ServerInfo> },
    PeerCandidate {
        peer_addr: SocketAddr,
        punch_token: PunchToken,
    },
    UseRelay {
        punch_token: PunchToken,
        relay_addr: SocketAddr,
    },
    Error { message: String },
}
