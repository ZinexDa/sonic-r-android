use proto::PunchToken;
use std::{net::SocketAddr, time::Duration};
use tokio::net::UdpSocket;

pub const MSG_TYPE_KEEPALIVE: u8 = 0x00;
pub const MSG_TYPE_GAME_DATA: u8 = 0x01;
pub const KEEPALIVE_INTERVAL: Duration = Duration::from_secs(5);

/// Channels for tunneling game datagrams over an active WebSocket connection.
#[derive(Debug)]
pub struct WsTunnelChannels {
    pub out_tx: tokio::sync::mpsc::UnboundedSender<Vec<u8>>,
    pub in_rx: tokio::sync::broadcast::Receiver<Vec<u8>>,
}

/// Runs the local UDP loopback proxy by binding to `127.0.0.1:<bind_port>`
/// and executing `run_tunnel_session_with_socket`.
///
/// In `host` mode, `bind_port` is typically `0` (ephemeral port) and `target_game_addr` is `Some(127.0.0.1:<game_port>)`.
/// In `join` mode, `bind_port` is the port the client game connects to, and `target_game_addr` is `None` (dynamic learning).
#[allow(clippy::too_many_arguments)]
pub async fn run_tunnel_session(
    tunnel_sock: &UdpSocket,
    target_addr: SocketAddr,
    punch_token: PunchToken,
    bind_port: u16,
    target_game_addr: Option<SocketAddr>,
    is_relay: bool,
    direct_punch_success: bool,
    relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
) -> Result<(), std::io::Error> {
    run_tunnel_session_with_events(
        tunnel_sock,
        target_addr,
        punch_token,
        bind_port,
        target_game_addr,
        is_relay,
        direct_punch_success,
        relay_rx,
        None,
    )
    .await
}

/// Runs the local UDP loopback proxy with optional runner event notifications.
#[allow(clippy::too_many_arguments)]
pub async fn run_tunnel_session_with_events(
    tunnel_sock: &UdpSocket,
    target_addr: SocketAddr,
    punch_token: PunchToken,
    bind_port: u16,
    target_game_addr: Option<SocketAddr>,
    is_relay: bool,
    direct_punch_success: bool,
    relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
    event_tx: Option<tokio::sync::mpsc::Sender<crate::runner::RunnerEvent>>,
) -> Result<(), std::io::Error> {
    run_tunnel_session_with_events_and_ws(
        tunnel_sock,
        target_addr,
        punch_token,
        bind_port,
        target_game_addr,
        is_relay,
        direct_punch_success,
        relay_rx,
        event_tx,
        None,
    )
    .await
}

/// Runs the local UDP loopback proxy with optional runner event notifications and WebSocket tunnel channels.
#[allow(clippy::too_many_arguments)]
pub async fn run_tunnel_session_with_events_and_ws(
    tunnel_sock: &UdpSocket,
    target_addr: SocketAddr,
    punch_token: PunchToken,
    bind_port: u16,
    target_game_addr: Option<SocketAddr>,
    is_relay: bool,
    direct_punch_success: bool,
    relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
    event_tx: Option<tokio::sync::mpsc::Sender<crate::runner::RunnerEvent>>,
    ws_tunnel: Option<WsTunnelChannels>,
) -> Result<(), std::io::Error> {
    let local_game_bind = SocketAddr::from(([127, 0, 0, 1], bind_port));
    let game_sock = match UdpSocket::bind(local_game_bind).await {
        Ok(sock) => sock,
        Err(err)
            if bind_port != 0
                && (err.kind() == std::io::ErrorKind::AddrInUse || err.raw_os_error() == Some(98)) =>
        {
            tracing::warn!(bind_port, %err, "Loopback bind port already in use; falling back to ephemeral port");
            log::warn!("Loopback bind port {} already in use; falling back to ephemeral port: {}", bind_port, err);
            UdpSocket::bind(SocketAddr::from(([127, 0, 0, 1], 0))).await?
        }
        Err(err) => return Err(err),
    };
    let bound_port = game_sock.local_addr()?.port();
    if let Some(ref tx) = event_tx {
        let _ = tx.send(crate::runner::RunnerEvent::ProxyBound { port: bound_port }).await;
    }
    run_tunnel_session_full(
        tunnel_sock,
        target_addr,
        punch_token,
        game_sock,
        target_game_addr,
        is_relay,
        direct_punch_success,
        relay_rx,
        ws_tunnel,
    )
    .await
}

/// Runs the local UDP loopback proxy with a pre-bound game socket.
///
/// - **Local Game -> Tunnel**: Datagrams received on `game_sock` are encapsulated
///   as `[16-byte punch_token][0x01 (MSG_TYPE_GAME_DATA)][payload...]` and forwarded to `target_addr`
///   via `tunnel_sock`. Learns the local game client/server's ephemeral address.
///
/// - **Tunnel -> Local Game**: Datagrams received on `tunnel_sock` are demultiplexed:
///   - `0x00` (MSG_TYPE_KEEPALIVE): Consumed as keepalive ping, not forwarded to game socket.
///   - `0x01` (MSG_TYPE_GAME_DATA): Stripped of 17-byte prefix and forwarded byte-for-byte to the
///     learned or preset local game address. Dropped if no local game address is known.
///
/// - **Tunnel Keepalive**: Sends `[16-byte punch_token][0x00][b"PING"]` immediately and every 5 seconds.
///
/// - **Relay Transition**: If a `UseRelay` message arrives on `relay_rx`, switches `target_addr`
///   to the hub relay address without interrupting game traffic (ignored if `direct_punch_success` is true).
#[allow(clippy::too_many_arguments)]
pub async fn run_tunnel_session_with_socket(
    tunnel_sock: &UdpSocket,
    target_addr: SocketAddr,
    punch_token: PunchToken,
    game_sock: UdpSocket,
    initial_game_addr: Option<SocketAddr>,
    is_relay: bool,
    direct_punch_success: bool,
    relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
) -> Result<(), std::io::Error> {
    run_tunnel_session_full(
        tunnel_sock,
        target_addr,
        punch_token,
        game_sock,
        initial_game_addr,
        is_relay,
        direct_punch_success,
        relay_rx,
        None,
    )
    .await
}

/// Runs the local UDP loopback proxy with optional WebSocket tunneling support.
#[allow(clippy::too_many_arguments)]
pub async fn run_tunnel_session_full(
    tunnel_sock: &UdpSocket,
    mut target_addr: SocketAddr,
    punch_token: PunchToken,
    game_sock: UdpSocket,
    initial_game_addr: Option<SocketAddr>,
    mut is_relay: bool,
    direct_punch_success: bool,
    mut relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
    mut ws_tunnel: Option<WsTunnelChannels>,
) -> Result<(), std::io::Error> {
    let bound_game_addr = game_sock.local_addr()?;
    tracing::info!(
        %bound_game_addr,
        ?initial_game_addr,
        %target_addr,
        %punch_token,
        is_relay,
        direct_punch_success,
        has_ws_tunnel = ws_tunnel.is_some(),
        "Local UDP loopback proxy running"
    );

    let token_bytes = punch_token.as_bytes();
    let mut last_game_addr: Option<SocketAddr> = initial_game_addr;

    let mut game_buf = [0u8; 2048];
    let mut tunnel_buf = [0u8; 2048];
    let mut out_pkt = [0u8; 2048];
    out_pkt[..16].copy_from_slice(token_bytes);
    out_pkt[16] = MSG_TYPE_GAME_DATA;

    let mut ping_pkt = [0u8; 21];
    ping_pkt[..16].copy_from_slice(token_bytes);
    ping_pkt[16] = MSG_TYPE_KEEPALIVE;
    ping_pkt[17..21].copy_from_slice(b"PING");

    let is_single_port = (target_addr.ip().is_loopback() && target_addr.port() == 9001) || target_addr.port() == 9001;
    if is_single_port {
        tracing::info!(%target_addr, "Single-port WebSocket datagram tunneling activated");
    }

    let mut keepalive_timer = tokio::time::interval(KEEPALIVE_INTERVAL);
    // If starting in relay mode, immediately register endpoint with relay (skip if single-port)
    if is_relay && !is_single_port {
        if !target_addr.ip().is_loopback() || ws_tunnel.is_none() {
            if let Err(err) = tunnel_sock.send_to(&ping_pkt, target_addr).await {
                tracing::warn!(%target_addr, %err, "Failed sending initial tunnel keepalive to relay");
            } else {
                tracing::debug!(%target_addr, is_relay, "Sent initial tunnel keepalive to relay");
            }
        }
        if let Some(ref ws) = ws_tunnel {
            let _ = ws.out_tx.send(ping_pkt.to_vec());
        }
    }
    // Skip immediate tick of the interval timer
    keepalive_timer.tick().await;

    loop {
        tokio::select! {
            // 1. Inbound from local game process -> forward out to tunnel
            recv_game = game_sock.recv_from(&mut game_buf) => {
                match recv_game {
                    Ok((n, from)) => {
                        if last_game_addr != Some(from) {
                            tracing::info!(%from, "Learned local game process address");
                            last_game_addr = Some(from);
                        }

                        if 17 + n > out_pkt.len() {
                            tracing::warn!(len = n, "Game datagram too large for tunnel buffer; dropping");
                            continue;
                        }

                        out_pkt[17..17 + n].copy_from_slice(&game_buf[..n]);
                        let full_pkt = &out_pkt[..17 + n];

                        let cur_is_single_port = (target_addr.ip().is_loopback() && target_addr.port() == 9001) || target_addr.port() == 9001;
                        if cur_is_single_port {
                            if let Some(ref ws) = ws_tunnel {
                                if let Err(err) = ws.out_tx.send(full_pkt.to_vec()) {
                                    tracing::warn!(%err, "Failed sending game datagram to WebSocket tunnel");
                                } else {
                                    tracing::info!(payload_len = n, "Tunneled local game datagram (with 17-byte header) via WebSocket");
                                    log::info!("Tunneled local game datagram ({} bytes with 17-byte header) via WebSocket", n);
                                }
                            }
                            continue;
                        }

                        // Forward to UDP target unless target is loopback when WS tunnel is available
                        if !target_addr.ip().is_loopback() || ws_tunnel.is_none() {
                            if let Err(err) = tunnel_sock.send_to(full_pkt, target_addr).await {
                                tracing::warn!(%target_addr, %err, "Failed to send game datagram over tunnel");
                            } else {
                                tracing::trace!(%target_addr, payload_len = n, "Forwarded game datagram to tunnel");
                            }
                        }

                        // Also send via WebSocket tunnel if in relay mode or target is loopback
                        if is_relay || target_addr.ip().is_loopback() {
                            if let Some(ref ws) = ws_tunnel {
                                let _ = ws.out_tx.send(full_pkt.to_vec());
                            }
                        }
                    }
                    Err(err) => {
                        if err.raw_os_error() == Some(10054) {
                            // WSAECONNRESET on Windows (ICMP port unreachable from previous send_to). Suppress.
                            continue;
                        }
                        tracing::warn!(%err, "Error reading from local game socket");
                    }
                }
            }

            // 2. Inbound from tunnel -> demux keepalive vs game data -> forward to game process
            recv_tunnel = tunnel_sock.recv_from(&mut tunnel_buf) => {
                match recv_tunnel {
                    Ok((n, from)) => {
                        if n < 17 {
                            tracing::debug!(%from, len = n, "Ignoring short tunnel packet (< 17 bytes)");
                            continue;
                        }

                        if &tunnel_buf[..16] != token_bytes {
                            tracing::debug!(%from, "Ignoring tunnel packet: punch token mismatch");
                            continue;
                        }

                        // Validate source address:
                        // In relay mode, packets must come from relay_addr (or same IP / loopback).
                        // In direct P2P mode, packets must come from peer's confirmed IP.
                        if is_relay {
                            if from.ip() != target_addr.ip()
                                && !from.ip().is_loopback()
                                && !target_addr.ip().is_loopback()
                            {
                                tracing::warn!(
                                    expected_ip = %target_addr.ip(),
                                    actual_ip = %from.ip(),
                                    %from,
                                    "Ignoring tunnel packet: token matched but source IP does not match expected relay IP"
                                );
                                continue;
                            }
                            if from != target_addr {
                                tracing::info!(
                                    old_relay = %target_addr,
                                    actual_relay = %from,
                                    "Updated relay target address to confirmed source address"
                                );
                                target_addr = from;
                            }
                        } else {
                            if from.ip() != target_addr.ip()
                                && !(from.ip().is_loopback() && target_addr.ip().is_loopback())
                            {
                                tracing::warn!(
                                    expected_ip = %target_addr.ip(),
                                    actual_ip = %from.ip(),
                                    %from,
                                    "Ignoring tunnel packet: token matched but source IP does not match expected peer IP"
                                );
                                continue;
                            }
                            if from.port() != target_addr.port() {
                                tracing::info!(
                                    old_peer = %target_addr,
                                    actual_peer = %from,
                                    "Updated peer target port from confirmed packet"
                                );
                                target_addr = from;
                            }
                        }

                        let msg_type = tunnel_buf[16];
                        match msg_type {
                            MSG_TYPE_KEEPALIVE => {
                                tracing::debug!(%from, len = n - 17, "Received tunnel keepalive");
                            }
                            MSG_TYPE_GAME_DATA => {
                                let payload = &tunnel_buf[17..n];
                                let dest_game_addr = last_game_addr
                                    .or(initial_game_addr)
                                    .unwrap_or_else(|| SocketAddr::from(([127, 0, 0, 1], 5029)));

                                if last_game_addr.is_none() {
                                    last_game_addr = Some(dest_game_addr);
                                }
                                if let Err(err) = game_sock.send_to(payload, dest_game_addr).await {
                                    tracing::warn!(%dest_game_addr, %err, "Failed delivering game datagram to local game process");
                                } else {
                                    tracing::trace!(%dest_game_addr, payload_len = payload.len(), "Delivered game datagram to local game process");
                                }
                            }
                            other => {
                                tracing::debug!(%from, msg_type = other, "Ignoring tunnel packet with unknown message type");
                            }
                        }
                    }
                    Err(err) => {
                        if err.raw_os_error() == Some(10054) {
                            // WSAECONNRESET on Windows (ICMP port unreachable from previous send_to). Suppress.
                            continue;
                        }
                        tracing::warn!(%err, "Error reading from tunnel socket");
                    }
                }
            }

            // 3. Inbound from WebSocket tunnel -> demux keepalive vs game data -> forward to game process
            ws_pkt_res = async {
                if let Some(ref mut ws) = ws_tunnel {
                    ws.in_rx.recv().await
                } else {
                    std::future::pending().await
                }
            } => {
                if let Ok(pkt) = ws_pkt_res {
                    // Support both 17-byte framed packets ([16-byte token][0x01][data]) and raw datagrams
                    let payload: &[u8] = if pkt.len() >= 17 && pkt[16] == MSG_TYPE_GAME_DATA {
                        &pkt[17..]
                    } else if pkt.len() >= 17 && pkt[16] == MSG_TYPE_KEEPALIVE {
                        tracing::debug!(len = pkt.len() - 17, "Received WS tunnel keepalive");
                        continue;
                    } else {
                        &pkt[..]
                    };

                    let dest_game_addr = last_game_addr
                        .or(initial_game_addr)
                        .unwrap_or_else(|| SocketAddr::from(([127, 0, 0, 1], 5029)));

                    if last_game_addr.is_none() {
                        last_game_addr = Some(dest_game_addr);
                    }

                    if let Err(err) = game_sock.send_to(payload, dest_game_addr).await {
                        tracing::warn!(%dest_game_addr, %err, "Failed delivering WS tunnel datagram to local game process");
                    } else {
                        tracing::info!(%dest_game_addr, payload_len = payload.len(), "Delivered WS tunnel datagram to local game process");
                        log::info!("Delivered WS tunnel datagram ({} bytes) to local game process at {}", payload.len(), dest_game_addr);
                    }
                }
            }

            // 4. Periodic tunnel keepalive
            _ = keepalive_timer.tick() => {
                let cur_is_single_port = (target_addr.ip().is_loopback() && target_addr.port() == 9001) || target_addr.port() == 9001;
                if cur_is_single_port {
                    continue;
                }
                if !target_addr.ip().is_loopback() || ws_tunnel.is_none() {
                    if let Err(err) = tunnel_sock.send_to(&ping_pkt, target_addr).await {
                        tracing::warn!(%target_addr, %err, "Failed sending tunnel keepalive");
                    } else {
                        tracing::debug!(%target_addr, is_relay, "Sent tunnel keepalive");
                    }
                }
                if is_relay || target_addr.ip().is_loopback() {
                    if let Some(ref ws) = ws_tunnel {
                        let _ = ws.out_tx.send(ping_pkt.to_vec());
                    }
                }
            }

            // 5. Dynamic relay switch from hub
            relay_res = relay_rx.recv() => {
                if let Ok(new_relay_addr) = relay_res {
                    if direct_punch_success {
                        tracing::warn!(
                            %new_relay_addr,
                            "Direct hole punch already succeeded; ignoring incoming UseRelay to prevent downgrading active P2P session"
                        );
                    } else {
                        tracing::info!(
                            previous_target = %target_addr,
                            %new_relay_addr,
                            "Switched tunnel to RELAY mode via hub"
                        );
                        target_addr = new_relay_addr;
                        is_relay = true;
                        let is_now_single_port = (target_addr.ip().is_loopback() && target_addr.port() == 9001) || target_addr.port() == 9001;
                        if !is_now_single_port {
                            if !target_addr.ip().is_loopback() || ws_tunnel.is_none() {
                                if let Err(err) = tunnel_sock.send_to(&ping_pkt, target_addr).await {
                                    tracing::warn!(%target_addr, %err, "Failed sending immediate keepalive to relay");
                                } else {
                                    tracing::debug!(%target_addr, "Sent immediate keepalive to relay");
                                }
                            }
                            if let Some(ref ws) = ws_tunnel {
                                let _ = ws.out_tx.send(ping_pkt.to_vec());
                            }
                        }
                    }
                }
            }
        }
    }
}
