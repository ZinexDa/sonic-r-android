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
    if let Err(err) = sock.send_to(bytes, hub_udp_addr).await {
        tracing::debug!(%hub_udp_addr, %err, "UDP punch to hub failed (likely single-port mode with UDP disabled)");
    } else {
        tracing::info!(%hub_udp_addr, %token, "Sent 16-byte UDP punch packet to hub");
    }
    Ok(sock)
}

/// Sonic R UDP discovery protocol constants.
pub const NET_DISCOVER_MAGIC: u32 = 0x534F4E52; // "SONR"
pub const NET_DISCOVER_REPLY: u32 = 0x534F4E48; // "SONH"

/// Probes the local Sonic R game process to verify it has bound its UDP socket in the lobby.
pub async fn probe_game_lobby(probe_sock: &UdpSocket, target_addr: SocketAddr) -> bool {
    let msg = NET_DISCOVER_MAGIC.to_le_bytes();

    // Drain any stale error state or unread packet on Windows
    let mut drain_buf = [0u8; 64];
    while let Ok(Ok(_)) = tokio::time::timeout(Duration::from_millis(1), probe_sock.recv_from(&mut drain_buf)).await {}

    if let Err(err) = probe_sock.send_to(&msg, target_addr).await {
        if err.raw_os_error() == Some(10054) {
            // Windows WSAECONNRESET from prior ICMP port unreachable; retry once
            if probe_sock.send_to(&msg, target_addr).await.is_err() {
                return false;
            }
        } else {
            return false;
        }
    }

    let mut buf = [0u8; 16];
    let sleep_fut = tokio::time::sleep(Duration::from_millis(300));
    tokio::pin!(sleep_fut);

    loop {
        tokio::select! {
            _ = &mut sleep_fut => return false,
            recv_res = probe_sock.recv_from(&mut buf) => {
                match recv_res {
                    Ok((n, from)) if from.port() == target_addr.port() && n >= 4 => {
                        let reply = u32::from_le_bytes(buf[..4].try_into().unwrap());
                        return reply == NET_DISCOVER_REPLY;
                    }
                    Ok(_) => continue,
                    Err(err) => {
                        if err.raw_os_error() == Some(10054) {
                            return false;
                        }
                        return false;
                    }
                }
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
    let last_relay: Arc<std::sync::Mutex<Option<SocketAddr>>> = Arc::new(std::sync::Mutex::new(None));
    let spawned_tokens: Arc<std::sync::Mutex<std::collections::HashSet<PunchToken>>> =
        Arc::new(std::sync::Mutex::new(std::collections::HashSet::new()));
    let active_peer_tokens: Arc<std::sync::Mutex<std::collections::HashSet<[u8; 16]>>> =
        Arc::new(std::sync::Mutex::new(std::collections::HashSet::new()));
    let (ws_tunnel_out_tx, mut ws_tunnel_out_rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();
    let (ws_tunnel_in_tx, _) = tokio::sync::broadcast::channel::<Vec<u8>>(128);

    // 1. Generate punch token and send UDP punch packet to hub
    let punch_token = uuid::Uuid::new_v4();
    let udp_socket = punch_udp(config.hub_udp_addr, punch_token)
        .await
        .map_err(|e| format!("Failed to send UDP punch packet: {e}"))?;
    let _udp_socket = Arc::new(udp_socket);

    // 2. Initialize Singleton UDP Loopback Proxy on 127.0.0.1:config.bind_port (5029)
    let proxy_bind_port = if config.bind_port > 0 { config.bind_port } else { 5029 };
    let host_proxy_sock = match UdpSocket::bind(SocketAddr::from(([127, 0, 0, 1], proxy_bind_port))).await {
        Ok(s) => Arc::new(s),
        Err(err)
            if err.kind() == std::io::ErrorKind::AddrInUse || err.raw_os_error() == Some(98) =>
        {
            tracing::warn!(proxy_bind_port, %err, "Host singleton proxy port in use; falling back to ephemeral port");
            log::warn!("Host singleton proxy port {} in use; falling back to ephemeral port: {}", proxy_bind_port, err);
            Arc::new(UdpSocket::bind("127.0.0.1:0").await.map_err(|e| format!("Failed to bind ephemeral host proxy: {e}"))?)
        }
        Err(err) => return Err(format!("Failed to bind host loopback proxy on 127.0.0.1:{}: {}", proxy_bind_port, err)),
    };
    let bound_proxy_port = host_proxy_sock.local_addr().map(|a| a.port()).unwrap_or(proxy_bind_port);
    tracing::info!(bound_proxy_port, "Singleton host UDP loopback proxy running on port 5029");
    log::info!("Singleton host UDP loopback proxy running on port {}", bound_proxy_port);
    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::ProxyBound { port: bound_proxy_port }).await;
    }

    // Spawn the singleton proxy forwarding loop
    let proxy_game_sock = host_proxy_sock.clone();
    let proxy_target_game = config.target_game_addr;
    let proxy_ws_out_tx = ws_tunnel_out_tx.clone();
    let mut proxy_ws_in_rx = ws_tunnel_in_tx.subscribe();
    let host_tokens = active_peer_tokens.clone();
    let host_proxy_task = tokio::spawn(async move {
        let mut game_buf = [0u8; 2048];
        let mut last_game_addr = proxy_target_game;
        loop {
            tokio::select! {
                recv_res = proxy_game_sock.recv_from(&mut game_buf) => {
                    match recv_res {
                        Ok((n, from)) => {
                            if last_game_addr != Some(from) {
                                last_game_addr = Some(from);
                            }
                            let tokens: Vec<[u8; 16]> = {
                                let lock = host_tokens.lock().unwrap();
                                lock.iter().cloned().collect()
                            };

                            for token in tokens {
                                let mut frame = Vec::with_capacity(17 + n);
                                frame.extend_from_slice(&token);
                                frame.push(0x01);
                                frame.extend_from_slice(&game_buf[..n]);
                                let _ = proxy_ws_out_tx.send(frame);
                            }
                        }
                        Err(err) => {
                            if err.raw_os_error() != Some(10054) {
                                tracing::warn!(%err, "Singleton proxy: error reading from local game socket");
                            }
                        }
                    }
                }
                ws_res = proxy_ws_in_rx.recv() => {
                    match ws_res {
                        Ok(pkt) => {
                            if pkt.len() >= 17 && pkt[16] == crate::loopback::MSG_TYPE_GAME_DATA {
                                let mut tok = [0u8; 16];
                                tok.copy_from_slice(&pkt[..16]);
                                host_tokens.lock().unwrap().insert(tok);
                            }
                            let payload: &[u8] = if pkt.len() >= 17 && pkt[16] == crate::loopback::MSG_TYPE_GAME_DATA {
                                &pkt[17..]
                            } else if pkt.len() >= 17 && pkt[16] == crate::loopback::MSG_TYPE_KEEPALIVE {
                                continue;
                            } else {
                                &pkt[..]
                            };
                            let dest_game = last_game_addr.unwrap_or_else(|| SocketAddr::from(([127, 0, 0, 1], 5030)));
                            if let Err(err) = proxy_game_sock.send_to(payload, dest_game).await {
                                tracing::warn!(%dest_game, %err, "Singleton proxy: failed delivering WS datagram to game");
                            } else {
                                tracing::trace!(%dest_game, payload_len = payload.len(), "Singleton proxy: delivered WS datagram to local game");
                                log::debug!("Delivered WS tunnel datagram ({} bytes) to local game process at {}", payload.len(), dest_game);
                            }
                        }
                        Err(tokio::sync::broadcast::error::RecvError::Lagged(skipped)) => {
                            tracing::warn!(skipped, "Singleton proxy: lagged behind on WS tunnel broadcast");
                        }
                        Err(tokio::sync::broadcast::error::RecvError::Closed) => {
                            tracing::info!("Singleton proxy: WS tunnel channel closed");
                            break;
                        }
                    }
                }
            }
        }
    });

    // 3. Send RegisterHost message immediately so room appears on hub without delay
    let host_udp_port = config
        .target_game_addr
        .map(|a| a.port())
        .filter(|&p| p > 0)
        .or(if config.bind_port > 0 { Some(config.bind_port) } else { None })
        .unwrap_or(5029);

    let register_msg = ClientMessage::RegisterHost {
        name: config.name.clone(),
        max_players: 4,
        game_version: "0.1.0".to_string(),
        udp_port: Some(host_udp_port),
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
        let _ = tx.send(RunnerEvent::Status("Host session registered. Waiting for Sonic R lobby (port 5029)...".to_string())).await;
    }

    // 5. Initial lobby check with relaxed timeout (up to 3 minutes for slow-loading game windows)
    let probe_sock = UdpSocket::bind("127.0.0.1:0")
        .await
        .map_err(|e| format!("Failed to bind UDP probe socket: {e}"))?;

    let mut game_lobby_active = false;
    if let Some(target_game_addr) = config.target_game_addr {
        if probe_game_lobby(&probe_sock, target_game_addr).await {
            game_lobby_active = true;
            if let Some(ref tx) = event_tx {
                let _ = tx
                    .send(RunnerEvent::Status("Sonic R host lobby active on port 5029! Ready for players.".to_string()))
                    .await;
            }
        } else {
            tracing::info!(%target_game_addr, "Host session registered on hub; awaiting Sonic R lobby on port 5029");
            if let Some(ref tx) = event_tx {
                let _ = tx
                    .send(RunnerEvent::Status("Host room active on hub. Waiting for Sonic R on port 5029...".to_string()))
                    .await;
            }
        }
    } else if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Status("Registered host, awaiting players...".to_string())).await;
    }

    // 5. Message loop with game lobby liveness monitor
    let mut liveness_interval = tokio::time::interval(Duration::from_secs(3));
    liveness_interval.tick().await; // skip immediate tick
    let mut failed_probes = 0;

    loop {
        tokio::select! {
            _ = liveness_interval.tick(), if config.target_game_addr.is_some() => {
                if let Some(target_game_addr) = config.target_game_addr {
                    if probe_game_lobby(&probe_sock, target_game_addr).await {
                        failed_probes = 0;
                        if !game_lobby_active {
                            game_lobby_active = true;
                            tracing::info!(%target_game_addr, "Sonic R host lobby detected active!");
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::Status("Sonic R host lobby active on port 5029! Ready for players.".to_string())).await;
                            }
                        }
                    } else if game_lobby_active {
                        failed_probes += 1;
                        if failed_probes >= 30 {
                            tracing::warn!(%target_game_addr, "Sonic R lobby closed or unresponsive; ending host session");
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::Status("Sonic R closed; shutting down host session.".to_string())).await;
                            }
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
            Some(bin) = ws_tunnel_out_rx.recv() => {
                if let Err(err) = ws_tx.send(Message::Binary(bin)).await {
                    tracing::warn!(%err, "Failed to send binary packet to hub WS");
                    break;
                }
            }
            Some(()) = hb_rx.recv() => {
                let count = active_peers.load(Ordering::Relaxed) as u8;
                let hb = ClientMessage::Heartbeat {
                    players: Some(1 + count),
                    status: None,
                };
                if let Ok(text) = serde_json::to_string(&hb) {
                    if let Err(err) = ws_tx.send(Message::Text(text)).await {
                        tracing::warn!(%err, "Failed to send heartbeat to hub WS");
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
                    Message::Binary(bin) => {
                        let _ = ws_tunnel_in_tx.send(bin);
                        continue;
                    }
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
                            active_peer_tokens.lock().unwrap().insert(*punch_token.as_bytes());
                            if let Some(ref tx) = event_tx {
                                let _ = tx.send(RunnerEvent::PeerCandidateReceived { peer_addr, punch_token }).await;
                            }
                            let should_spawn = spawned_tokens.lock().unwrap().insert(punch_token);
                            if should_spawn {
                                tracing::info!(%punch_token, %peer_addr, "Registering active peer token on host singleton session");
                                let cmd_tx_clone = ws_cmd_tx.clone();
                                let sub_event_tx = event_tx.clone();
                                let active_peers_clone = active_peers.clone();
                                tokio::spawn(async move {
                                    let _guard = crate::punch::PeerCountGuard::new(Some(active_peers_clone), cmd_tx_clone.clone());
                                    if let Some(ref tx) = sub_event_tx {
                                        let _ = tx.send(RunnerEvent::TunnelEstablished {
                                            peer_addr: SocketAddr::from(([127, 0, 0, 1], 9001)),
                                            is_relay: true,
                                        }).await;
                                    }
                                    let _ = cmd_tx_clone.send(proto::ClientMessage::RelayFallback { punch_token });
                                    futures_util::future::pending::<()>().await;
                                });
                            } else {
                                tracing::info!(%punch_token, "Peer connection already active for token; skipping duplicate candidate");
                            }
                        }
                        HubMessage::UseRelay { punch_token, relay_addr } => {
                            tracing::info!(%punch_token, %relay_addr, "Received UseRelay from hub");
                            *last_relay.lock().unwrap() = Some(relay_addr);
                            let _ = relay_tx.send(relay_addr);
                            active_peer_tokens.lock().unwrap().insert(*punch_token.as_bytes());

                            let should_spawn = spawned_tokens.lock().unwrap().insert(punch_token);
                            if should_spawn {
                                tracing::info!(
                                    %punch_token,
                                    %relay_addr,
                                    "Registering active peer token from UseRelay on host singleton session"
                                );
                                let cmd_tx_clone = ws_cmd_tx.clone();
                                let sub_event_tx = event_tx.clone();
                                let active_peers_clone = active_peers.clone();
                                tokio::spawn(async move {
                                    let _guard = crate::punch::PeerCountGuard::new(Some(active_peers_clone), cmd_tx_clone.clone());
                                    if let Some(ref tx) = sub_event_tx {
                                        let _ = tx.send(RunnerEvent::TunnelEstablished {
                                            peer_addr: relay_addr,
                                            is_relay: true,
                                        }).await;
                                    }
                                    let _ = cmd_tx_clone.send(proto::ClientMessage::RelayFallback { punch_token });
                                    futures_util::future::pending::<()>().await;
                                });
                            }
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

    host_proxy_task.abort();
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

    let punch_token = uuid::Uuid::new_v4();
    let (ws_cmd_tx, mut ws_cmd_rx) = tokio::sync::mpsc::unbounded_channel::<ClientMessage>();
    let (ws_tunnel_out_tx, mut ws_tunnel_out_rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();
    let (ws_tunnel_in_tx, _) = tokio::sync::broadcast::channel::<Vec<u8>>(128);
    let spawned_tokens: Arc<std::sync::Mutex<std::collections::HashSet<PunchToken>>> =
        Arc::new(std::sync::Mutex::new(std::collections::HashSet::new()));

    // 1. Initialize Singleton UDP Loopback Proxy on 127.0.0.1:config.bind_port (5029)
    // and emit ProxyBound IMMEDIATELY so port 5029 is listening BEFORE game engine boots!
    let proxy_bind_port = if config.bind_port > 0 { config.bind_port } else { 5029 };
    let client_proxy_sock = match UdpSocket::bind(SocketAddr::from(([127, 0, 0, 1], proxy_bind_port))).await {
        Ok(s) => Arc::new(s),
        Err(err)
            if err.kind() == std::io::ErrorKind::AddrInUse || err.raw_os_error() == Some(98) =>
        {
            tracing::warn!(proxy_bind_port, %err, "Client singleton proxy port in use; falling back to ephemeral port");
            log::warn!("Client singleton proxy port {} in use; falling back to ephemeral port: {}", proxy_bind_port, err);
            Arc::new(UdpSocket::bind("127.0.0.1:0").await.map_err(|e| format!("Failed to bind ephemeral client proxy: {e}"))?)
        }
        Err(err) => return Err(format!("Failed to bind client loopback proxy on 127.0.0.1:{}: {}", proxy_bind_port, err)),
    };
    let bound_proxy_port = client_proxy_sock.local_addr().map(|a| a.port()).unwrap_or(proxy_bind_port);
    tracing::info!(bound_proxy_port, "Singleton client UDP loopback proxy running on port 5029");
    log::info!("Singleton client UDP loopback proxy running on port {}", bound_proxy_port);
    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::ProxyBound { port: bound_proxy_port }).await;
    }

    // 2. Spawn the client proxy forwarding loop immediately so early engine packets are buffered in ws_tunnel_out_tx!
    let proxy_game_sock = client_proxy_sock.clone();
    let proxy_target_game = config.target_game_addr;
    let proxy_ws_out_tx = ws_tunnel_out_tx.clone();
    let mut proxy_ws_in_rx = ws_tunnel_in_tx.subscribe();
    let client_token_bytes = *punch_token.as_bytes();
    let client_proxy_task = tokio::spawn(async move {
        let mut game_buf = [0u8; 2048];
        let mut last_game_addr = proxy_target_game;
        loop {
            tokio::select! {
                recv_res = proxy_game_sock.recv_from(&mut game_buf) => {
                    match recv_res {
                        Ok((n, from)) => {
                            if last_game_addr != Some(from) {
                                last_game_addr = Some(from);
                            }
                            let mut framed = Vec::with_capacity(17 + n);
                            framed.extend_from_slice(&client_token_bytes);
                            framed.push(crate::loopback::MSG_TYPE_GAME_DATA);
                            framed.extend_from_slice(&game_buf[..n]);
                            if let Err(err) = proxy_ws_out_tx.send(framed) {
                                tracing::warn!(%err, "Client proxy: failed sending game datagram to WS tunnel");
                            } else {
                                tracing::trace!(payload_len = n, "Client proxy: tunneled local game datagram via WebSocket");
                                log::debug!("Tunneled local game datagram ({} bytes with 17-byte header) via WebSocket", n);
                            }
                        }
                        Err(err) => {
                            if err.raw_os_error() != Some(10054) {
                                tracing::warn!(%err, "Client proxy: error reading from local game socket");
                            }
                        }
                    }
                }
                ws_res = proxy_ws_in_rx.recv() => {
                    match ws_res {
                        Ok(pkt) => {
                            let payload: &[u8] = if pkt.len() >= 17 && pkt[16] == crate::loopback::MSG_TYPE_GAME_DATA {
                                &pkt[17..]
                            } else if pkt.len() >= 17 && pkt[16] == crate::loopback::MSG_TYPE_KEEPALIVE {
                                continue;
                            } else {
                                &pkt[..]
                            };
                            let dest_game = last_game_addr.unwrap_or_else(|| SocketAddr::from(([127, 0, 0, 1], 5031)));
                            if let Err(err) = proxy_game_sock.send_to(payload, dest_game).await {
                                tracing::warn!(%dest_game, %err, "Client proxy: failed delivering WS datagram to game");
                            } else {
                                tracing::trace!(%dest_game, payload_len = payload.len(), "Client proxy: delivered WS datagram to local game");
                                log::debug!("Delivered WS tunnel datagram ({} bytes) to local game process at {}", payload.len(), dest_game);
                            }
                        }
                        Err(tokio::sync::broadcast::error::RecvError::Lagged(skipped)) => {
                            tracing::warn!(skipped, "Client proxy: lagged behind on WS tunnel broadcast");
                        }
                        Err(tokio::sync::broadcast::error::RecvError::Closed) => {
                            tracing::info!("Client proxy: WS tunnel channel closed");
                            break;
                        }
                    }
                }
            }
        }
    });

    if let Some(ref tx) = event_tx {
        let _ = tx.send(RunnerEvent::Status(format!("Connecting to hub at {}...", config.hub_ws_url))).await;
    }

    let (ws_stream, _) = connect_async(&config.hub_ws_url)
        .await
        .map_err(|e| format!("Failed to connect to hub WebSocket: {e}"))?;

    let (mut ws_tx, mut ws_rx) = ws_stream.split();

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
                match msg_res {
                    Ok(Message::Text(t)) => {
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
                    Ok(Message::Binary(bin)) => {
                        let _ = ws_tunnel_in_tx.send(bin);
                    }
                    Ok(Message::Close(_)) => {
                        return Err("Hub closed connection prematurely while querying server list".to_string());
                    }
                    Err(e) => {
                        return Err(format!("WebSocket error while querying server list: {e}"));
                    }
                    _ => {}
                }
            }

            match found_id {
                Some(id) => id,
                None => return Err("Did not receive server list from hub".to_string()),
            }
        }
    };

    // 3. Send UDP punch packet to hub (best effort)
    let _ = punch_udp(config.hub_udp_addr, punch_token).await;

    let join_udp_port = config
        .target_game_addr
        .map(|a| a.port())
        .filter(|&p| p > 0)
        .or(if config.bind_port > 0 { Some(config.bind_port) } else { None })
        .unwrap_or(5029);

    // 4. Send JoinRequest
    let join_msg = ClientMessage::JoinRequest {
        server_id: target_server_id,
        punch_token,
        udp_port: Some(join_udp_port),
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

    // 5. Loop: keep WS connection open, tunnel datagrams, and handle signaling messages
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
            Some(bin) = ws_tunnel_out_rx.recv() => {
                if let Err(err) = ws_tx.send(Message::Binary(bin)).await {
                    tracing::warn!(%err, "Failed to send binary packet to hub WS");
                    break;
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
                    Message::Binary(bin) => {
                        let _ = ws_tunnel_in_tx.send(bin);
                        continue;
                    }
                    Message::Close(_) => break,
                    _ => continue,
                };

                if let Ok(hub_msg) = serde_json::from_str::<HubMessage>(&text) {
                    match hub_msg {
                        HubMessage::PeerCandidate { peer_addr, punch_token: cand_token } => {
                            let should_spawn = spawned_tokens.lock().unwrap().insert(cand_token);
                            if should_spawn {
                                tracing::info!(
                                    %peer_addr,
                                    %cand_token,
                                    "Received PeerCandidate! Switching to single-port WebSocket tunnel"
                                );
                                if let Some(ref tx) = event_tx {
                                    let _ = tx.send(RunnerEvent::PeerCandidateReceived { peer_addr, punch_token: cand_token }).await;
                                    let _ = tx.send(RunnerEvent::TunnelEstablished {
                                        peer_addr: SocketAddr::from(([127, 0, 0, 1], 9001)),
                                        is_relay: true,
                                    }).await;
                                }
                                let _ = ws_cmd_tx.send(ClientMessage::RelayFallback { punch_token: cand_token });
                            } else {
                                tracing::info!(%cand_token, "Peer connection already active for token; skipping duplicate candidate");
                            }
                        }
                        HubMessage::UseRelay { punch_token: relay_token, relay_addr } => {
                            tracing::info!(%relay_token, %relay_addr, "Received UseRelay from hub");
                            let should_spawn = spawned_tokens.lock().unwrap().insert(relay_token);
                            if should_spawn {
                                if let Some(ref tx) = event_tx {
                                    let _ = tx.send(RunnerEvent::TunnelEstablished {
                                        peer_addr: relay_addr,
                                        is_relay: true,
                                    }).await;
                                }
                                let _ = ws_cmd_tx.send(ClientMessage::RelayFallback { punch_token: relay_token });
                            }
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

    client_proxy_task.abort();
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
