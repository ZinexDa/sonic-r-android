use futures_util::{SinkExt, StreamExt};
use proto::{ClientMessage, HubMessage, PunchToken, ServerId, ServerInfo};
use std::{
    net::SocketAddr,
    sync::{
        atomic::{AtomicU32, Ordering},
        Arc,
    },
    time::Duration,
};
use tokio::net::UdpSocket;
use tokio_tungstenite::{connect_async, tungstenite::Message};

#[derive(Debug, Clone)]
pub enum RunnerEvent {
    Status(String),
    Registered {
        server_id: ServerId,
    },
    PeerCandidateReceived {
        peer_addr: SocketAddr,
        punch_token: PunchToken,
    },
    TunnelEstablished {
        peer_addr: SocketAddr,
        is_relay: bool,
    },
    ProxyBound {
        port: u16,
    },
    Error(String),
    Stopped,
}

#[derive(Debug, Clone)]
pub struct HostConfig {
    pub hub_ws_url: String,
    pub hub_udp_addr: SocketAddr,
    pub name: String,
    pub bind_port: u16,
    pub target_game_addr: Option<SocketAddr>,
}

#[derive(Debug, Clone)]
pub struct JoinConfig {
    pub hub_ws_url: String,
    pub hub_udp_addr: SocketAddr,
    pub server_id: Option<ServerId>,
    pub bind_port: u16,
    pub target_game_addr: Option<SocketAddr>,
}

/// Sends a single 16-byte UDP packet containing the PunchToken to the hub's UDP port.
/// Returns the bound socket so it can be reused for subsequent P2P hole punching and relay fallback.
pub async fn punch_udp(hub_udp_addr: SocketAddr, token: PunchToken) -> Result<UdpSocket, std::io::Error> {
    let sock = UdpSocket::bind("0.0.0.0:0").await?;
    let bytes = token.as_bytes();
    sock.send_to(bytes, hub_udp_addr).await?;
    tracing::info!(%hub_udp_addr, %token, "Sent 16-byte UDP punch packet to hub");
    Ok(sock)
}

/// Sonic R UDP discovery protocol constants.
pub const NET_DISCOVER_MAGIC: u32 = 0x534F4E52; // "SONR"
pub const NET_DISCOVER_REPLY: u32 = 0x534F4E48; // "SONH"

/// Probes the local Sonic R game process to verify it has bound its UDP socket in the lobby.
pub async fn probe_game_lobby(probe_sock: &UdpSocket, target_addr: SocketAddr) -> bool {
    let msg = NET_DISCOVER_MAGIC.to_le_bytes();
    if probe_sock.send_to(&msg, target_addr).await.is_err() {
        return false;
    }

    let mut buf = [0u8; 16];
    let sleep_fut = tokio::time::sleep(Duration::from_millis(150));
    tokio::pin!(sleep_fut);

    tokio::select! {
        _ = &mut sleep_fut => false,
        recv_res = probe_sock.recv_from(&mut buf) => {
            match recv_res {
                Ok((n, from)) if from.port() == target_addr.port() && n >= 4 => {
                    let reply = u32::from_le_bytes(buf[..4].try_into().unwrap());
                    reply == NET_DISCOVER_REPLY
                }
                _ => false,
            }
        }
    }
}

/// Standalone query to fetch the active server list from the hub.
pub async fn fetch_server_list(hub_ws_url: &str) -> Result<Vec<ServerInfo>, String> {
    crate::init_crypto_provider();
    tracing::info!(%hub_ws_url, "Querying server list from hub");

    let (ws_stream, _) = connect_async(hub_ws_url)
        .await
        .map_err(|e| format!("Failed to connect to hub WebSocket: {e}"))?;

    let (mut ws_tx, mut ws_rx) = ws_stream.split();

    let list_msg = ClientMessage::ListServers;
    let text = serde_json::to_string(&list_msg)
        .map_err(|e| format!("Failed to serialize ListServers: {e}"))?;

    ws_tx
        .send(Message::Text(text))
        .await
        .map_err(|e| format!("Failed to send ListServers message: {e}"))?;

    let timeout_fut = tokio::time::sleep(Duration::from_secs(5));
    tokio::pin!(timeout_fut);

    loop {
        tokio::select! {
            _ = &mut timeout_fut => {
                return Err("Timed out waiting for server list from hub".to_string());
            }
            msg_res = ws_rx.next() => {
                match msg_res {
                    Some(Ok(Message::Text(t))) => {
                        if let Ok(HubMessage::ServerList { servers }) = serde_json::from_str(&t) {
                            return Ok(servers);
                        }
                    }
                    Some(Ok(Message::Close(_))) => {
                        return Err("Hub closed connection prematurely".to_string());
                    }
                    Some(Err(e)) => {
                        return Err(format!("WebSocket error while receiving server list: {e}"));
                    }
                    None => {
                        return Err("WebSocket connection closed without response".to_string());
                    }
                    _ => {}
                }
            }
        }
    }
}

/// Host mode session: registers a game server, sends heartbeats, and manages incoming peer connections.
pub async fn run_host_session(
    config: HostConfig,
    event_tx: Option<tokio::sync::mpsc::Sender<RunnerEvent>>,
) -> Result<(), String> {
    crate::init_crypto_provider();
    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Status(format!("Connecting to hub at {}...", config.hub_ws_url))).await;
    }

    let (ws_stream, _) = connect_async(&config.hub_ws_url)
        .await
        .map_err(|e| format!("Failed to connect to hub WebSocket: {e}"))?;

    let (mut ws_tx, mut ws_rx) = ws_stream.split();
    let (ws_cmd_tx, mut ws_cmd_rx) = tokio::sync::mpsc::unbounded_channel::<ClientMessage>();
    let (relay_tx, _) = tokio::sync::broadcast::channel::<SocketAddr>(16);

    // 1. Generate punch token and send UDP punch packet to hub
    let punch_token = uuid::Uuid::new_v4();
    let udp_socket = punch_udp(config.hub_udp_addr, punch_token)
        .await
        .map_err(|e| format!("Failed to send UDP punch packet: {e}"))?;
    let udp_socket = Arc::new(udp_socket);

    // 2. If target_game_addr is specified, wait for the local game to open its lobby
    let probe_sock = UdpSocket::bind("127.0.0.1:0")
        .await
        .map_err(|e| format!("Failed to bind UDP probe socket: {e}"))?;

    if let Some(target_game_addr) = config.target_game_addr {
        if let Some(ref tx) = event_tx {
            let _ = tx
                .send(RunnerEvent::Status("Waiting for Sonic R host lobby to open...".to_string()))
                .await;
        }

        let wait_start = std::time::Instant::now();
        let timeout = Duration::from_secs(60);
        let mut ready = false;

        while wait_start.elapsed() < timeout {
            if probe_game_lobby(&probe_sock, target_game_addr).await {
                ready = true;
                break;
            }
            tokio::time::sleep(Duration::from_millis(250)).await;
        }

        if !ready {
            return Err("Timed out waiting for Sonic R to enter host lobby (port not active)".to_string());
        }

        if let Some(ref tx) = event_tx {
            let _ = tx
                .send(RunnerEvent::Status("Host lobby active! Registering room with hub...".to_string()))
                .await;
        }
    }

    // 3. Send RegisterHost message
    let register_msg = ClientMessage::RegisterHost {
        name: config.name.clone(),
        max_players: 4,
        game_version: "0.1.0".to_string(),
        udp_port: None,
        punch_token,
    };

    let serialized = serde_json::to_string(&register_msg)
        .map_err(|e| format!("Failed to serialize RegisterHost: {e}"))?;

    ws_tx
        .send(Message::Text(serialized))
        .await
        .map_err(|e| format!("Failed to send RegisterHost: {e}"))?;

    // 4. Heartbeat task & active peer tracking
    let active_peers = Arc::new(AtomicU32::new(0));
    let (hb_tx, mut hb_rx) = tokio::sync::mpsc::channel::<()>(1);
    let hb_task = tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(10));
        interval.tick().await;

        loop {
            tokio::select! {
                _ = interval.tick() => {
                    let _ = hb_tx.send(()).await;
                }
            }
        }
    });

    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Status("Registered host, awaiting players...".to_string())).await;
    }

    // 5. Message loop with game lobby liveness monitor
    let mut liveness_interval = tokio::time::interval(Duration::from_secs(2));
    liveness_interval.tick().await; // skip immediate tick
    let mut failed_probes = 0;

    loop {
        tokio::select! {
            _ = liveness_interval.tick(), if config.target_game_addr.is_some() => {
                if let Some(target_game_addr) = config.target_game_addr {
                    if probe_game_lobby(&probe_sock, target_game_addr).await {
                        failed_probes = 0;
                    } else {
                        failed_probes += 1;
                        if failed_probes >= 3 {
                            tracing::info!(%target_game_addr, "Local game process left lobby or closed socket; terminating host session");
                            break;
                        }
                    }
                }
            }
            Some(cmd) = ws_cmd_rx.recv() => {
                if let Ok(text) = serde_json::to_string(&cmd) {
                    if let Err(err) = ws_tx.send(Message::Text(text)).await {
                        tracing::warn!(%err, "Failed to send command to hub WS");
                        break;
                    }
                }
            }
            Some(()) = hb_rx.recv() => {
                let count = (1 + active_peers.load(Ordering::Relaxed)) as u8;
                let hb_msg = ClientMessage::Heartbeat { players: Some(count), status: None };
                if let Ok(text) = serde_json::to_string(&hb_msg) {
                    if let Err(err) = ws_tx.send(Message::Text(text)).await {
                        tracing::warn!(%err, "Failed to send heartbeat; connection may be closed");
                        break;
                    }
                    tracing::debug!(count, "Sent heartbeat to hub");
                }
            }
            msg_opt = ws_rx.next() => {
                let Some(msg_result) = msg_opt else {
                    tracing::info!("Hub disconnected");
                    break;
                };

                let msg = match msg_result {
                    Ok(m) => m,
                    Err(err) => {
                        tracing::warn!(%err, "WebSocket error reading from hub");
                        break;
                    }
                };

                let text = match msg {
                    Message::Text(t) => t,
                    Message::Close(_) => {
                        tracing::info!("Hub closed WebSocket connection");
                        break;
                    }
                    _ => continue,
                };

                match serde_json::from_str::<HubMessage>(&text) {
                    Ok(hub_msg) => match hub_msg {
                        HubMessage::Registered { server_id } => {
                            tracing::info!(%server_id, name = %config.name, "Host successfully registered with hub!");
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::Registered { server_id }).await;
                            }
                        }
                        HubMessage::PeerCandidate { peer_addr, punch_token } => {
                            tracing::info!(
                                %peer_addr,
                                %punch_token,
                                "Received PeerCandidate! Managing peer connection"
                            );
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::PeerCandidateReceived { peer_addr, punch_token }).await;
                            }
                            let socket_clone = udp_socket.clone();
                            let cmd_tx_clone = ws_cmd_tx.clone();
                            let relay_rx = relay_tx.subscribe();
                            let bind_port = config.bind_port;
                            let target_game_addr = config.target_game_addr;
                            let sub_event_tx = event_tx.clone();
                            let active_peers_clone = active_peers.clone();
                            tokio::spawn(async move {
                                crate::punch::manage_peer_connection_with_events(
                                    &socket_clone,
                                    peer_addr,
                                    punch_token,
                                    cmd_tx_clone,
                                    relay_rx,
                                    bind_port,
                                    target_game_addr,
                                    sub_event_tx,
                                    Some(active_peers_clone),
                                ).await;
                            });
                        }
                        HubMessage::UseRelay { punch_token, relay_addr } => {
                            tracing::info!(%punch_token, %relay_addr, "Received UseRelay from hub");
                            let _ = relay_tx.send(relay_addr);
                        }
                        HubMessage::Error { message } => {
                            tracing::error!(%message, "Received error from hub");
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::Error(message.clone())).await;
                            }
                        }
                        HubMessage::ServerList { servers } => {
                            tracing::info!(count = servers.len(), "Received server list from hub");
                        }
                    },
                    Err(err) => {
                        tracing::warn!(%text, %err, "Received unparseable message from hub");
                    }
                }
            }
        }
    }

    hb_task.abort();
    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Stopped).await;
    }
    tracing::info!("Host sidecar terminated");
    Ok(())
}

/// Join mode session: discovers or selects a server, punches UDP, and requests to join.
pub async fn run_join_session(
    config: JoinConfig,
    event_tx: Option<tokio::sync::mpsc::Sender<RunnerEvent>>,
) -> Result<(), String> {
    crate::init_crypto_provider();
    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Status(format!("Connecting to hub at {}...", config.hub_ws_url))).await;
    }

    let (ws_stream, _) = connect_async(&config.hub_ws_url)
        .await
        .map_err(|e| format!("Failed to connect to hub WebSocket: {e}"))?;

    let (mut ws_tx, mut ws_rx) = ws_stream.split();
    let (ws_cmd_tx, mut ws_cmd_rx) = tokio::sync::mpsc::unbounded_channel::<ClientMessage>();
    let (relay_tx, _) = tokio::sync::broadcast::channel::<SocketAddr>(16);

    // Determine target server ID
    let target_server_id = match config.server_id {
        Some(id) => id,
        None => {
            // Request server list
            let list_msg = ClientMessage::ListServers;
            let text = serde_json::to_string(&list_msg)
                .map_err(|e| format!("Failed to serialize ListServers: {e}"))?;
            ws_tx
                .send(Message::Text(text))
                .await
                .map_err(|e| format!("Failed to send ListServers: {e}"))?;

            let mut found_id = None;
            while let Some(msg_res) = ws_rx.next().await {
                if let Ok(Message::Text(t)) = msg_res {
                    if let Ok(HubMessage::ServerList { servers }) = serde_json::from_str(&t) {
                        if servers.is_empty() {
                            return Err("No active servers currently registered on hub".to_string());
                        }
                        println!("\nAvailable servers:");
                        for (i, s) in servers.iter().enumerate() {
                            println!("  [{}] {} (id: {}, players: {}/{})", i, s.name, s.id, s.players, s.max_players);
                        }
                        let selected = &servers[0];
                        tracing::info!(selected_id = %selected.id, selected_name = %selected.name, "Automatically selecting first server");
                        found_id = Some(selected.id);
                        break;
                    }
                }
            }

            match found_id {
                Some(id) => id,
                None => return Err("Did not receive server list from hub".to_string()),
            }
        }
    };

    // 1. Generate punch token and send UDP punch packet to hub
    let punch_token = uuid::Uuid::new_v4();
    let udp_socket = punch_udp(config.hub_udp_addr, punch_token)
        .await
        .map_err(|e| format!("Failed to send UDP punch packet: {e}"))?;
    let udp_socket = Arc::new(udp_socket);

    // 2. Send JoinRequest
    let join_msg = ClientMessage::JoinRequest {
        server_id: target_server_id,
        punch_token,
    };
    let text = serde_json::to_string(&join_msg)
        .map_err(|e| format!("Failed to serialize JoinRequest: {e}"))?;
    ws_tx
        .send(Message::Text(text))
        .await
        .map_err(|e| format!("Failed to send JoinRequest: {e}"))?;

    tracing::info!(%target_server_id, %punch_token, "Sent JoinRequest, awaiting PeerCandidate from hub...");
    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Status("Sent JoinRequest, awaiting peer candidate...".to_string())).await;
    }

    // 3. Loop: keep WS connection open, handle commands (RelayFallback), and dispatch PeerCandidate / UseRelay
    loop {
        tokio::select! {
            Some(cmd) = ws_cmd_rx.recv() => {
                if let Ok(text) = serde_json::to_string(&cmd) {
                    if let Err(err) = ws_tx.send(Message::Text(text)).await {
                        tracing::warn!(%err, "Failed to send command to hub WS");
                        break;
                    }
                }
            }
            msg_opt = ws_rx.next() => {
                let Some(msg_res) = msg_opt else {
                    tracing::info!("Hub disconnected");
                    break;
                };

                let msg = match msg_res {
                    Ok(m) => m,
                    Err(err) => {
                        tracing::warn!(%err, "WebSocket error");
                        break;
                    }
                };

                let text = match msg {
                    Message::Text(t) => t,
                    Message::Close(_) => break,
                    _ => continue,
                };

                if let Ok(hub_msg) = serde_json::from_str::<HubMessage>(&text) {
                    match hub_msg {
                        HubMessage::PeerCandidate { peer_addr, punch_token } => {
                            tracing::info!(
                                %peer_addr,
                                %punch_token,
                                "Received PeerCandidate! Managing peer connection"
                            );
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::PeerCandidateReceived { peer_addr, punch_token }).await;
                            }
                            let socket_clone = udp_socket.clone();
                            let cmd_tx_clone = ws_cmd_tx.clone();
                            let relay_rx = relay_tx.subscribe();
                            let bind_port = config.bind_port;
                            let target_game_addr = config.target_game_addr;
                            let sub_event_tx = event_tx.clone();
                            tokio::spawn(async move {
                                crate::punch::manage_peer_connection_with_events(
                                    &socket_clone,
                                    peer_addr,
                                    punch_token,
                                    cmd_tx_clone,
                                    relay_rx,
                                    bind_port,
                                    target_game_addr,
                                    sub_event_tx,
                                    None,
                                ).await;
                            });
                        }
                        HubMessage::UseRelay { punch_token, relay_addr } => {
                            tracing::info!(%punch_token, %relay_addr, "Received UseRelay from hub");
                            let _ = relay_tx.send(relay_addr);
                        }
                        HubMessage::Error { message } => {
                            tracing::error!(%message, "Hub error during join session");
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::Error(message.clone())).await;
                            }
                            break;
                        }
                        other => {
                            tracing::debug!(?other, "Received other message from hub");
                        }
                    }
                }
            }
        }
    }

    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Stopped).await;
    }
    tracing::info!("Join client finished");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_probe_game_lobby_success() {
        let fake_game = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let fake_game_addr = fake_game.local_addr().unwrap();

        // Spawn fake game listener that mimics Sonic R net_transport
        tokio::spawn(async move {
            let mut buf = [0u8; 16];
            if let Ok((n, sender)) = fake_game.recv_from(&mut buf).await {
                if n >= 4 && u32::from_le_bytes(buf[..4].try_into().unwrap()) == NET_DISCOVER_MAGIC {
                    let reply = NET_DISCOVER_REPLY.to_le_bytes();
                    let _ = fake_game.send_to(&reply, sender).await;
                }
            }
        });

        let probe_sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let res = probe_game_lobby(&probe_sock, fake_game_addr).await;
        assert!(res, "probe_game_lobby should succeed with valid discovery reply");
    }

    #[tokio::test]
    async fn test_probe_game_lobby_unresponsive() {
        // Probe an unbound address
        let probe_sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let dead_addr: SocketAddr = "127.0.0.1:54321".parse().unwrap();
        let res = probe_game_lobby(&probe_sock, dead_addr).await;
        assert!(!res, "probe_game_lobby should return false if game is not listening");
    }

    #[tokio::test]
    async fn test_probe_game_lobby_invalid_reply() {
        let fake_game = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let fake_game_addr = fake_game.local_addr().unwrap();

        tokio::spawn(async move {
            let mut buf = [0u8; 16];
            if let Ok((_, sender)) = fake_game.recv_from(&mut buf).await {
                // Send garbage reply
                let _ = fake_game.send_to(b"GARBAGE_PAYLOAD", sender).await;
            }
        });

        let probe_sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let res = probe_game_lobby(&probe_sock, fake_game_addr).await;
        assert!(!res, "probe_game_lobby should return false on mismatched discovery reply");
    }
}
