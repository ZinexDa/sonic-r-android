use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::net::SocketAddr;
use proto::ServerId;
use crate::runner::{HostConfig, JoinConfig, RunnerEvent};

/// Helper to convert a C string pointer to a Rust String.
/// Returns None if pointer is null or contains invalid UTF-8.
unsafe fn c_str_to_string(ptr: *const c_char) -> Option<String> {
    if ptr.is_null() {
        None
    } else {
        CStr::from_ptr(ptr).to_str().ok().map(|s| s.trim().to_string())
    }
}

/// Initialize netplay runtime, crypto provider, and Android/native logging.
/// Returns 0 on success, -1 on failure.
#[no_mangle]
pub extern "C" fn netplay_init() -> c_int {
    crate::init_crypto_provider();
    crate::init_logging();

    match crate::get_or_create_runtime() {
        Ok(_) => {
            tracing::info!("Netplay C FFI initialized successfully");
            0
        }
        Err(err) => {
            eprintln!("Failed to initialize netplay runtime: {err}");
            -1
        }
    }
}

/// Start hosting a netplay room in a background thread and proxy traffic to/from local game port.
/// Returns:
///   0: Session started in background
///  -1: Invalid argument (NULL hub_url or invalid string)
///  -2: Failed to build runtime or spawn session task
#[no_mangle]
pub extern "C" fn netplay_start_host(
    hub_url: *const c_char,
    room_name: *const c_char,
    game_port: u16,
) -> c_int {
    crate::init_crypto_provider();
    crate::init_logging();

    let hub_str = match unsafe { c_str_to_string(hub_url) } {
        Some(s) if !s.is_empty() => s,
        _ => {
            tracing::error!("netplay_start_host: hub_url is required");
            return -1;
        }
    };

    let name = unsafe { c_str_to_string(room_name) }
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "Sonic Host".to_string());

    let port = if game_port == 0 { 5029 } else { game_port };

    let rt = match crate::get_or_create_runtime() {
        Ok(r) => r,
        Err(err) => {
            tracing::error!(%err, "netplay_start_host: runtime unavailable");
            return -2;
        }
    };

    // Stop any previously running session
    crate::stop_active_session();

    log::info!("netplay_start_host: hub={}, name={}, port={}", hub_str, name, port);
    tracing::info!(hub = %hub_str, name = %name, port, "Starting netplay host session");

    let (event_tx, mut event_rx) = tokio::sync::mpsc::channel::<RunnerEvent>(64);

    let task = rt.spawn(async move {
        let (hub_ws_url, hub_udp_addr) = crate::resolve_hub_addr_async(&hub_str).await;
        let bind_port = if port == 5029 { 0 } else { 5029 };
        let config = HostConfig {
            hub_ws_url,
            hub_udp_addr,
            name,
            bind_port,
            target_game_addr: Some(SocketAddr::from(([127, 0, 0, 1], port))),
        };

        tokio::spawn(async move {
            while let Some(ev) = event_rx.recv().await {
                log::info!("Host RunnerEvent: {:?}", ev);
                tracing::info!(?ev, "Host RunnerEvent");
            }
        });

        if let Err(err) = crate::runner::run_host_session(config, Some(event_tx)).await {
            log::error!("Host session ended with error: {}", err);
            tracing::error!(%err, "Host session ended with error");
        } else {
            log::info!("Host session completed cleanly");
            tracing::info!("Host session completed cleanly");
        }
    });

    if let Err(err) = crate::set_active_task(task) {
        log::error!("Failed to register active host task: {}", err);
        tracing::error!(%err, "Failed to register active host task");
        return -2;
    }

    0
}

/// Join an existing netplay room on the hub.
/// Returns:
///   0: Session started in background
///  -1: Invalid argument (NULL hub_url or invalid string)
///  -2: Invalid room_id UUID
///  -3: Failed to build runtime or spawn session task
#[no_mangle]
pub extern "C" fn netplay_start_join(
    hub_url: *const c_char,
    room_id: *const c_char,
    game_port: u16,
) -> c_int {
    crate::init_crypto_provider();
    crate::init_logging();

    let hub_str = match unsafe { c_str_to_string(hub_url) } {
        Some(s) if !s.is_empty() => s,
        _ => {
            tracing::error!("netplay_start_join: hub_url is required");
            return -1;
        }
    };

    let server_id: Option<ServerId> = match unsafe { c_str_to_string(room_id) } {
        Some(ref s) if !s.is_empty() => match uuid::Uuid::parse_str(s) {
            Ok(uuid) => Some(uuid),
            Err(err) => {
                tracing::error!(%err, id = %s, "netplay_start_join: invalid room_id UUID");
                return -2;
            }
        },
        _ => None, // Auto-selects first available server
    };

    let engine_port = if game_port == 0 || game_port == 5029 { 5031 } else { game_port };
    let bind_port = 5029;

    let rt = match crate::get_or_create_runtime() {
        Ok(r) => r,
        Err(err) => {
            tracing::error!(%err, "netplay_start_join: runtime unavailable");
            return -3;
        }
    };

    // Stop any previously running session
    crate::stop_active_session();

    log::info!("netplay_start_join: hub={}, server_id={:?}, engine_port={}, proxy_bind_port={}", hub_str, server_id, engine_port, bind_port);
    tracing::info!(hub = %hub_str, ?server_id, engine_port, bind_port, "Starting netplay join session");

    let (event_tx, mut event_rx) = tokio::sync::mpsc::channel::<RunnerEvent>(64);

    let task = rt.spawn(async move {
        let (hub_ws_url, hub_udp_addr) = crate::resolve_hub_addr_async(&hub_str).await;
        log::info!("Join task: resolved ws={}, udp={:?}", hub_ws_url, hub_udp_addr);
        let config = JoinConfig {
            hub_ws_url,
            hub_udp_addr,
            server_id,
            bind_port,
            target_game_addr: Some(SocketAddr::from(([127, 0, 0, 1], engine_port))),
        };

        tokio::spawn(async move {
            while let Some(ev) = event_rx.recv().await {
                log::info!("Join RunnerEvent: {:?}", ev);
                tracing::info!(?ev, "Join RunnerEvent");
            }
        });

        if let Err(err) = crate::runner::run_join_session(config, Some(event_tx)).await {
            log::error!("Join session ended with error: {}", err);
            tracing::error!(%err, "Join session ended with error");
        } else {
            log::info!("Join session completed cleanly");
            tracing::info!("Join session completed cleanly");
        }
    });

    if let Err(err) = crate::set_active_task(task) {
        log::error!("Failed to register active join task: {}", err);
        tracing::error!(%err, "Failed to register active join task");
        return -3;
    }

    0
}

/// Stop any active netplay host or join session and close sockets.
/// Idempotent and thread-safe.
#[no_mangle]
pub extern "C" fn netplay_stop() {
    tracing::info!("Stopping active netplay session...");
    crate::stop_active_session();
}

/// Query the signaling hub for online rooms and return JSON string.
/// Returns null on error or empty hub URL.
/// The caller MUST free the returned string using `netplay_free_string`.
#[no_mangle]
pub extern "C" fn netplay_fetch_room_list(hub_url: *const c_char) -> *mut c_char {
    let hub_str = match unsafe { c_str_to_string(hub_url) } {
        Some(s) if !s.is_empty() => s,
        _ => return std::ptr::null_mut(),
    };

    match crate::fetch_room_list(&hub_str) {
        Ok(json) => match std::ffi::CString::new(json) {
            Ok(c_str) => c_str.into_raw(),
            Err(_) => std::ptr::null_mut(),
        },
        Err(err) => {
            tracing::error!(%err, "netplay_fetch_room_list failed");
            std::ptr::null_mut()
        }
    }
}

/// Free a string returned by `netplay_fetch_room_list`.
#[no_mangle]
pub extern "C" fn netplay_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        unsafe {
            let _ = std::ffi::CString::from_raw(ptr);
        }
    }
}

