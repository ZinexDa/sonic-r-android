use proto::PunchToken;
use std::{
    net::SocketAddr,
    sync::{
        atomic::{AtomicU32, Ordering},
        Arc,
    },
    time::{Duration, Instant},
};
use tokio::net::UdpSocket;

pub const DEFAULT_PUNCH_TIMEOUT: Duration = Duration::from_secs(10);
pub const PUNCH_INTERVAL: Duration = Duration::from_millis(200);
pub const KEEPALIVE_INTERVAL: Duration = Duration::from_secs(5);

#[derive(Debug, PartialEq, Eq)]
pub enum PunchError {
    Timeout,
    #[allow(dead_code)]
    Io(String),
}

impl std::fmt::Display for PunchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Timeout => write!(
                f,
                "Hole punch failed, peer unreachable — likely symmetric NAT on one or both sides"
            ),
            Self::Io(e) => write!(f, "Hole punch I/O error: {e}"),
        }
    }
}

impl std::error::Error for PunchError {}

/// Performs bidirectional UDP hole punching against `peer_addr` using the shared `punch_token`.
///
/// Sends repeated 16-byte punch packets every 200ms while simultaneously listening for
/// incoming packets on `socket`. Validates that incoming packets contain the expected
/// `punch_token` bytes and originate from the expected peer IP.
///
/// Upon receiving the first valid packet:
/// - Marks hole punch as successful.
/// - Sends a small confirmation burst to ensure the reverse NAT path is also open.
/// - Calculates and logs round-trip duration.
/// - Returns the confirmed peer `SocketAddr`.
///
/// If no valid packet is received within `timeout`, returns `Err(PunchError::Timeout)`.
pub async fn perform_hole_punch(
    socket: &UdpSocket,
    peer_addr: SocketAddr,
    punch_token: PunchToken,
    timeout: Duration,
) -> Result<SocketAddr, PunchError> {
    tracing::info!(%peer_addr, %punch_token, "Starting UDP hole punching with peer");

    let mut token_packet = [0u8; 17];
    token_packet[..16].copy_from_slice(punch_token.as_bytes());
    token_packet[16] = 0x00;

    let token_bytes = punch_token.as_bytes();
    let start_time = Instant::now();

    let mut send_interval = tokio::time::interval(PUNCH_INTERVAL);
    // Suppress first immediate tick so we control the timing in the select loop
    send_interval.tick().await;

    let timeout_fut = tokio::time::sleep(timeout);
    tokio::pin!(timeout_fut);

    let mut recv_buf = [0u8; 64];

    loop {
        tokio::select! {
            _ = send_interval.tick() => {
                if let Err(err) = socket.send_to(&token_packet, peer_addr).await {
                    tracing::debug!(%peer_addr, %err, "Failed sending hole-punch packet");
                } else {
                    tracing::debug!(%peer_addr, "Sent UDP hole-punch packet");
                }
            }

            recv_res = socket.recv_from(&mut recv_buf) => {
                let (n, from) = match recv_res {
                    Ok(r) => r,
                    Err(err) => {
                        if err.raw_os_error() == Some(10054) {
                            continue;
                        }
                        tracing::debug!(%err, "Error receiving UDP packet during hole punch");
                        continue;
                    }
                };

                // 1. Validate packet size
                if n < 17 {
                    tracing::debug!(
                        %from,
                        len = n,
                        "Ignoring UDP packet: unexpected length during hole punch (expected >= 17 bytes)"
                    );
                    continue;
                }

                // 2. Validate token payload
                if &recv_buf[..16] != token_bytes {
                    tracing::debug!(
                        %from,
                        "Ignoring UDP packet: token payload does not match expected punch token"
                    );
                    continue;
                }

                // 3. Validate message type (0x00 is keepalive/punch ping)
                if recv_buf[16] != 0x00 {
                    tracing::debug!(
                        %from,
                        msg_type = recv_buf[16],
                        "Ignoring UDP packet: unexpected msg_type during hole punch (expected 0x00)"
                    );
                    continue;
                }

                // 4. Validate source IP (tolerate port rewriting from NAT)
                if from.ip() != peer_addr.ip() {
                    tracing::warn!(
                        expected_ip = %peer_addr.ip(),
                        actual_ip = %from.ip(),
                        %from,
                        "Ignoring UDP packet: token matched but IP does not match expected peer IP"
                    );
                    continue;
                }

                let rtt = start_time.elapsed();
                if from.port() != peer_addr.port() {
                    tracing::info!(
                        expected = %peer_addr,
                        actual = %from,
                        "Peer source port was translated by NAT (using confirmed address)"
                    );
                }

                tracing::info!(
                    confirmed_peer = %from,
                    ?rtt,
                    "Hole punch succeeded! Confirmed bidirectional connectivity with peer"
                );

                // Send a burst of confirmation packets to guarantee reverse NAT traversal
                for _ in 0..5 {
                    let _ = socket.send_to(&token_packet, from).await;
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }

                return Ok(from);
            }

            _ = &mut timeout_fut => {
                tracing::error!(
                    %peer_addr,
                    "Hole punch failed, peer unreachable — likely symmetric NAT on one or both sides"
                );
                return Err(PunchError::Timeout);
            }
        }
    }
}

/// Demonstrative application-level keepalive loop.
///
/// Sends lightweight datagrams every 5 seconds to prevent NAT mapping expiration,
/// while draining incoming datagrams from the peer.
#[allow(dead_code)]
pub async fn run_keepalive(socket: &UdpSocket, peer_addr: SocketAddr) {
    tracing::info!(%peer_addr, "Starting UDP keepalive loop with peer");

    let mut interval = tokio::time::interval(KEEPALIVE_INTERVAL);
    // Skip immediate tick
    interval.tick().await;

    let mut buf = [0u8; 64];

    loop {
        tokio::select! {
            _ = interval.tick() => {
                if let Err(err) = socket.send_to(b"PING", peer_addr).await {
                    tracing::warn!(%peer_addr, %err, "Failed to send keepalive datagram; terminating keepalive");
                    break;
                }
                tracing::info!(%peer_addr, "Sent UDP keepalive to peer");
            }

            recv_res = socket.recv_from(&mut buf) => {
                match recv_res {
                    Ok((n, from)) => {
                        let msg = String::from_utf8_lossy(&buf[..n]);
                        tracing::debug!(%from, len = n, text = %msg, "Received UDP datagram during keepalive");
                    }
                    Err(err) => {
                        if err.raw_os_error() == Some(10054) {
                            continue;
                        }
                        tracing::warn!(%err, "Error reading from UDP socket during keepalive");
                    }
                }
            }
        }
    }
}

/// Demonstrative application-level keepalive loop for UDP Relay mode.
///
/// Sends lightweight datagrams prefixed with the 16-byte `punch_token` to the hub's `relay_addr`
/// every 5 seconds, while receiving and verifying incoming relayed datagrams.
pub async fn run_relay_keepalive(
    socket: &UdpSocket,
    relay_addr: SocketAddr,
    punch_token: PunchToken,
) {
    tracing::info!(
        %relay_addr,
        %punch_token,
        "Operating in RELAY mode via hub"
    );

    let token_bytes = punch_token.as_bytes();
    let mut packet = Vec::with_capacity(16 + 1 + 4);
    packet.extend_from_slice(token_bytes);
    packet.push(0x00);
    packet.extend_from_slice(b"PING");

    let mut interval = tokio::time::interval(KEEPALIVE_INTERVAL);
    // Send immediate initial ping to register endpoint with relay
    if let Err(err) = socket.send_to(&packet, relay_addr).await {
        tracing::warn!(%relay_addr, %err, "Failed to send initial relay keepalive datagram");
    } else {
        tracing::info!(%relay_addr, "Sent initial relayed UDP keepalive to hub");
    }
    // Skip immediate tick of the interval timer
    interval.tick().await;

    let mut buf = [0u8; 128];

    loop {
        tokio::select! {
            _ = interval.tick() => {
                if let Err(err) = socket.send_to(&packet, relay_addr).await {
                    tracing::warn!(%relay_addr, %err, "Failed to send relay keepalive datagram; terminating keepalive");
                    break;
                }
                tracing::info!(%relay_addr, "Sent relayed UDP keepalive to hub");
            }

            recv_res = socket.recv_from(&mut buf) => {
                match recv_res {
                    Ok((n, from)) => {
                        if n < 17 {
                            tracing::debug!(%from, len = n, "Ignoring short datagram in relay mode");
                            continue;
                        }
                        if &buf[..16] != token_bytes {
                            tracing::debug!(%from, "Ignoring datagram with mismatched token prefix in relay mode");
                            continue;
                        }
                        if buf[16] != 0x00 {
                            tracing::debug!(%from, msg_type = buf[16], "Ignoring non-keepalive datagram in relay keepalive");
                            continue;
                        }
                        let payload = String::from_utf8_lossy(&buf[17..n]);
                        tracing::debug!(%from, payload = %payload, "Received relayed UDP datagram from peer via hub");
                    }
                    Err(err) => {
                        if err.raw_os_error() == Some(10054) {
                            continue;
                        }
                        tracing::warn!(%err, "Error reading from UDP socket during relay keepalive");
                    }
                }
            }
        }
    }
}

pub struct PeerCountGuard {
    active_peers: Option<Arc<AtomicU32>>,
    ws_cmd_tx: tokio::sync::mpsc::UnboundedSender<proto::ClientMessage>,
}

impl PeerCountGuard {
    pub fn new(
        active_peers: Option<Arc<AtomicU32>>,
        ws_cmd_tx: tokio::sync::mpsc::UnboundedSender<proto::ClientMessage>,
    ) -> Self {
        if let Some(ref counter) = active_peers {
            let new_peers = counter.fetch_add(1, Ordering::SeqCst) + 1;
            let total_players = 1 + new_peers;
            let _ = ws_cmd_tx.send(proto::ClientMessage::Heartbeat {
                players: Some(total_players as u8),
                status: None,
            });
            tracing::info!(total_players, "Peer connected; updated hub player count");
        }
        Self {
            active_peers,
            ws_cmd_tx,
        }
    }
}

impl Drop for PeerCountGuard {
    fn drop(&mut self) {
        if let Some(ref counter) = self.active_peers {
            let new_peers = counter
                .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |val| {
                    Some(val.saturating_sub(1))
                })
                .unwrap_or(0)
                .saturating_sub(1);
            let total_players = 1 + new_peers;
            let _ = self.ws_cmd_tx.send(proto::ClientMessage::Heartbeat {
                players: Some(total_players as u8),
                status: None,
            });
            tracing::info!(total_players, "Peer disconnected; updated hub player count");
        }
    }
}

/// Symmetrically coordinates connection to a peer:
/// 1. If `SIDECAR_FORCE_RELAY=1` is set, skips direct punching and requests relay immediately.
/// 2. Otherwise, attempts direct hole punching with `perform_hole_punch`.
/// 3. If direct punching times out, signals `ClientMessage::RelayFallback` to the hub and awaits `UseRelay`.
/// 4. If `UseRelay` arrives at any point (even if direct punching or direct keepalive was active),
///    switches to `run_relay_keepalive`.
pub async fn manage_peer_connection(
    socket: &UdpSocket,
    peer_addr: SocketAddr,
    punch_token: PunchToken,
    ws_cmd_tx: tokio::sync::mpsc::UnboundedSender<proto::ClientMessage>,
    relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
    bind_port: u16,
    target_game_addr: Option<SocketAddr>,
) {
    manage_peer_connection_with_events(
        socket,
        peer_addr,
        punch_token,
        ws_cmd_tx,
        relay_rx,
        None,
        bind_port,
        target_game_addr,
        None,
        None,
        None,
    )
    .await;
}

/// Symmetrically coordinates connection to a peer with optional runner event notifications:
#[allow(clippy::too_many_arguments)]
pub async fn manage_peer_connection_with_events(
    socket: &UdpSocket,
    peer_addr: SocketAddr,
    punch_token: PunchToken,
    ws_cmd_tx: tokio::sync::mpsc::UnboundedSender<proto::ClientMessage>,
    mut relay_rx: tokio::sync::broadcast::Receiver<SocketAddr>,
    cached_relay: Option<SocketAddr>,
    bind_port: u16,
    target_game_addr: Option<SocketAddr>,
    event_tx: Option<tokio::sync::mpsc::Sender<crate::runner::RunnerEvent>>,
    active_peers: Option<Arc<AtomicU32>>,
    ws_tunnel: Option<crate::loopback::WsTunnelChannels>,
) {
    let is_single_port = |addr: &SocketAddr| {
        (addr.ip().is_loopback() && addr.port() == 9001) || addr.port() == 9001
    };

    let force_relay = std::env::var("SIDECAR_FORCE_RELAY")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);

    let has_ws_tunnel = ws_tunnel.is_some();
    let relay_target: Option<SocketAddr> = if let Some(addr) = cached_relay.filter(|a| is_single_port(a)) {
        tracing::info!(
            %addr,
            %punch_token,
            "[Single-Port] Single-port WebSocket relay (127.0.0.1:9001) detected; suppressing UDP hole punch"
        );
        Some(addr)
    } else if is_single_port(&peer_addr) {
        tracing::info!(
            %peer_addr,
            %punch_token,
            "[Single-Port] Peer target is single-port relay (127.0.0.1:9001); suppressing UDP hole punch"
        );
        Some(peer_addr)
    } else if has_ws_tunnel {
        tracing::info!(
            %punch_token,
            "[Single-Port] WebSocket tunnel active; bypassing 10-second UDP hole punch and activating relay immediately"
        );
        let _ = ws_cmd_tx.send(proto::ClientMessage::RelayFallback { punch_token });
        Some(SocketAddr::from(([127, 0, 0, 1], 9001)))
    } else if force_relay {
        tracing::info!(
            %punch_token,
            "[DEBUG] SIDECAR_FORCE_RELAY is set; bypassing direct punch and requesting relay fallback"
        );
        let _ = ws_cmd_tx.send(proto::ClientMessage::RelayFallback { punch_token });
        match tokio::time::timeout(Duration::from_secs(5), relay_rx.recv()).await {
            Ok(Ok(addr)) => Some(addr),
            _ => {
                tracing::error!("Timeout awaiting UseRelay from hub after forced fallback");
                None
            }
        }
    } else {
        let punch_fut = perform_hole_punch(socket, peer_addr, punch_token, DEFAULT_PUNCH_TIMEOUT);
        tokio::pin!(punch_fut);

        let mut direct_succeeded = false;
        let mut confirmed_peer = peer_addr;

        tokio::select! {
            early_relay = relay_rx.recv() => {
                if let Ok(addr) = early_relay {
                    tracing::info!(
                        %addr,
                        "Received UseRelay while direct punch in progress; switching to relay mode immediately"
                    );
                    if let Some(ref tx) = event_tx {
                        let _ = tx.send(crate::runner::RunnerEvent::TunnelEstablished {
                            peer_addr: addr,
                            is_relay: true,
                        }).await;
                    }
                    let _guard = PeerCountGuard::new(active_peers, ws_cmd_tx);
                    if let Err(err) = crate::loopback::run_tunnel_session_with_events_and_ws(
                        socket,
                        addr,
                        punch_token,
                        bind_port,
                        target_game_addr,
                        true,
                        false,
                        relay_rx,
                        event_tx,
                        ws_tunnel,
                    ).await {
                        tracing::error!(%err, "Tunnel session error in relay mode");
                    }
                    return;
                }
            }
            punch_res = &mut punch_fut => {
                match punch_res {
                    Ok(addr) => {
                        direct_succeeded = true;
                        confirmed_peer = addr;
                    }
                    Err(PunchError::Timeout) => {
                        tracing::info!(
                            %peer_addr,
                            "Direct hole punch timed out; requesting relay fallback from hub"
                        );
                        let _ = ws_cmd_tx.send(proto::ClientMessage::RelayFallback { punch_token });
                    }
                    Err(err) => {
                        tracing::error!(
                            %peer_addr,
                            %err,
                            "Direct hole punch encountered error; requesting relay fallback"
                        );
                        let _ = ws_cmd_tx.send(proto::ClientMessage::RelayFallback { punch_token });
                    }
                }
            }
        }

        if direct_succeeded {
            // Direct hole punch succeeded! Run loopback tunnel session directly to peer.
            // Any incoming UseRelay messages will be ignored by run_tunnel_session.
            if let Some(ref tx) = event_tx {
                let _ = tx.send(crate::runner::RunnerEvent::TunnelEstablished {
                    peer_addr: confirmed_peer,
                    is_relay: false,
                }).await;
            }
            let _guard = PeerCountGuard::new(active_peers, ws_cmd_tx);
            if let Err(err) = crate::loopback::run_tunnel_session_with_events_and_ws(
                socket,
                confirmed_peer,
                punch_token,
                bind_port,
                target_game_addr,
                false,
                true,
                relay_rx,
                event_tx,
                ws_tunnel,
            ).await {
                tracing::error!(%err, "Tunnel session error in direct P2P mode");
            }
            return;
        }

        // Direct punch failed, check cached_relay first or await UseRelay from hub with 5s timeout
        if let Some(addr) = cached_relay {
            Some(addr)
        } else {
            match tokio::time::timeout(Duration::from_secs(5), relay_rx.recv()).await {
                Ok(Ok(addr)) => Some(addr),
                _ => {
                    tracing::error!("No direct path found and relay coordination failed; connection failed");
                    None
                }
            }
        }
    };

    if let Some(relay_addr) = relay_target {
        if let Some(ref tx) = event_tx {
            let _ = tx.send(crate::runner::RunnerEvent::TunnelEstablished {
                peer_addr: relay_addr,
                is_relay: true,
            }).await;
        }
        let _guard = PeerCountGuard::new(active_peers, ws_cmd_tx);
        if let Err(err) = crate::loopback::run_tunnel_session_with_events_and_ws(
            socket,
            relay_addr,
            punch_token,
            bind_port,
            target_game_addr,
            true,
            false,
            relay_rx,
            event_tx,
            ws_tunnel,
        ).await {
            tracing::error!(%err, "Tunnel session error in relay mode");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_loopback_hole_punch_success() {
        let sock1 = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let sock2 = UdpSocket::bind("127.0.0.1:0").await.unwrap();

        let addr1 = sock1.local_addr().unwrap();
        let addr2 = sock2.local_addr().unwrap();
        let token = uuid::Uuid::new_v4();

        let timeout = Duration::from_secs(3);

        let h1 = tokio::spawn(async move {
            perform_hole_punch(&sock1, addr2, token, timeout).await
        });

        let h2 = tokio::spawn(async move {
            perform_hole_punch(&sock2, addr1, token, timeout).await
        });

        let (res1, res2) = tokio::try_join!(h1, h2).unwrap();
        let peer_confirmed_by_1 = res1.expect("sock1 should succeed punching sock2");
        let peer_confirmed_by_2 = res2.expect("sock2 should succeed punching sock1");

        assert_eq!(peer_confirmed_by_1, addr2);
        assert_eq!(peer_confirmed_by_2, addr1);
    }

    #[tokio::test]
    async fn test_hole_punch_timeout_failure() {
        let sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        // Pick an unused local port
        let dummy_sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let unreachable_addr = dummy_sock.local_addr().unwrap();
        drop(dummy_sock); // close socket so no one answers

        let token = uuid::Uuid::new_v4();
        let short_timeout = Duration::from_millis(500);

        let result = perform_hole_punch(&sock, unreachable_addr, token, short_timeout).await;
        assert_eq!(result, Err(PunchError::Timeout));
    }

    #[tokio::test]
    async fn test_ignore_garbage_packets() {
        let sock1 = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let sock2 = UdpSocket::bind("127.0.0.1:0").await.unwrap();

        let addr1 = sock1.local_addr().unwrap();
        let addr2 = sock2.local_addr().unwrap();
        let token = uuid::Uuid::new_v4();

        let h1 = tokio::spawn(async move {
            perform_hole_punch(&sock1, addr2, token, Duration::from_secs(3)).await
        });

        // sock2 sends malformed packets first
        sock2.send_to(b"random bytes", addr1).await.unwrap();
        sock2.send_to(&[0u8; 17], addr1).await.unwrap(); // wrong token

        // Small pause
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Now send valid token + 0x00
        let mut valid = Vec::new();
        valid.extend_from_slice(token.as_bytes());
        valid.push(0x00);
        sock2.send_to(&valid, addr1).await.unwrap();

        let res = h1.await.unwrap();
        assert_eq!(res.unwrap(), addr2);
    }

    #[tokio::test]
    async fn test_relay_keepalive() {
        let mock_relay = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let relay_addr = mock_relay.local_addr().unwrap();

        let peer_sock = UdpSocket::bind("127.0.0.1:0").await.unwrap();
        let token = uuid::Uuid::new_v4();

        // Spawn relay keepalive with a short lifespan
        let keepalive_handle = tokio::spawn(async move {
            run_relay_keepalive(&peer_sock, relay_addr, token).await;
        });

        // Mock relay should receive a PING packet prefixed with token + 0x00
        let mut buf = [0u8; 64];
        let (n, from) = mock_relay.recv_from(&mut buf).await.unwrap();
        assert!(n >= 21);
        assert_eq!(&buf[..16], token.as_bytes());
        assert_eq!(buf[16], 0x00);
        assert_eq!(&buf[17..n], b"PING");

        // Send a response packet back to peer
        let mut resp = Vec::new();
        resp.extend_from_slice(token.as_bytes());
        resp.push(0x00);
        resp.extend_from_slice(b"PONG");
        mock_relay.send_to(&resp, from).await.unwrap();

        keepalive_handle.abort();
    }

    #[tokio::test]
    async fn test_peer_count_guard() {
        let (cmd_tx, mut cmd_rx) = tokio::sync::mpsc::unbounded_channel();
        let active_peers = Arc::new(AtomicU32::new(0));

        // When first peer connects
        let guard1 = PeerCountGuard::new(Some(active_peers.clone()), cmd_tx.clone());
        assert_eq!(active_peers.load(Ordering::SeqCst), 1);
        let msg1 = cmd_rx.recv().await.unwrap();
        assert_eq!(
            msg1,
            proto::ClientMessage::Heartbeat {
                players: Some(2),
                status: None,
            }
        );

        // When second peer connects
        let guard2 = PeerCountGuard::new(Some(active_peers.clone()), cmd_tx.clone());
        assert_eq!(active_peers.load(Ordering::SeqCst), 2);
        let msg2 = cmd_rx.recv().await.unwrap();
        assert_eq!(
            msg2,
            proto::ClientMessage::Heartbeat {
                players: Some(3),
                status: None,
            }
        );

        // Drop guard2 (second peer disconnects)
        drop(guard2);
        assert_eq!(active_peers.load(Ordering::SeqCst), 1);
        let msg3 = cmd_rx.recv().await.unwrap();
        assert_eq!(
            msg3,
            proto::ClientMessage::Heartbeat {
                players: Some(2),
                status: None,
            }
        );

        // Drop guard1 (first peer disconnects)
        drop(guard1);
        assert_eq!(active_peers.load(Ordering::SeqCst), 0);
        let msg4 = cmd_rx.recv().await.unwrap();
        assert_eq!(
            msg4,
            proto::ClientMessage::Heartbeat {
                players: Some(1),
                status: None,
            }
        );

        // None guard (e.g. join session) does not send messages
        let guard_none = PeerCountGuard::new(None, cmd_tx.clone());
        drop(guard_none);
        assert!(cmd_rx.try_recv().is_err());
    }
}
