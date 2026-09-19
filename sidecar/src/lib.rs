pub mod loopback;
pub mod punch;
pub mod runner;
pub mod ffi;
pub mod jni_bridge;

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use tokio::runtime::Runtime;
use tokio::task::JoinHandle;

pub fn init_crypto_provider() {
    let _ = rustls::crypto::ring::default_provider().install_default();
}

pub fn init_logging() {
    #[cfg(target_os = "android")]
    {
        use android_logger::Config;
        android_logger::init_once(
            Config::default()
                .with_max_level(log::LevelFilter::Debug)
                .with_tag("SonicRNetplay"),
        );
        log::info!("SonicRNetplay: native android logger initialized");
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = tracing_subscriber::fmt()
            .with_max_level(tracing::Level::INFO)
            .try_init();
    }
}

/// Global session management state
struct SessionState {
    runtime: Arc<Runtime>,
    task: Option<JoinHandle<()>>,
}

static SESSION_STATE: Mutex<Option<SessionState>> = Mutex::new(None);

pub fn get_or_create_runtime() -> Result<Arc<Runtime>, String> {
    let mut guard = SESSION_STATE.lock().map_err(|e| e.to_string())?;
    if let Some(ref state) = *guard {
        return Ok(state.runtime.clone());
    }
    let rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .thread_name("sonicr-netplay")
        .build()
        .map_err(|e| format!("Failed to build Tokio runtime: {e}"))?;
    let arc_rt = Arc::new(rt);
    *guard = Some(SessionState {
        runtime: arc_rt.clone(),
        task: None,
    });
    Ok(arc_rt)
}

pub fn stop_active_session() {
    if let Ok(mut guard) = SESSION_STATE.lock() {
        if let Some(ref mut state) = *guard {
            if let Some(task) = state.task.take() {
                task.abort();
                tracing::info!("Aborted active netplay session task");
            }
        }
    }
}

pub fn set_active_task(handle: JoinHandle<()>) -> Result<(), String> {
    let mut guard = SESSION_STATE.lock().map_err(|e| e.to_string())?;
    if let Some(ref mut state) = *guard {
        if let Some(prev) = state.task.take() {
            prev.abort();
        }
        state.task = Some(handle);
        Ok(())
    } else {
        Err("Session state not initialized".to_string())
    }
}

/// Helper function to parse and resolve hub WebSocket URL and UDP endpoint
pub fn extract_hub_parts(raw: &str) -> (String, String) {
    let raw = raw.trim();
    let (scheme, rest) = if raw.starts_with("ws://") {
        ("ws://", &raw[5..])
    } else if raw.starts_with("wss://") {
        ("wss://", &raw[6..])
    } else if raw.starts_with("http://") {
        ("ws://", &raw[7..])
    } else if raw.starts_with("https://") {
        ("wss://", &raw[8..])
    } else {
        ("ws://", raw)
    };

    let (authority, path) = match rest.find('/') {
        Some(idx) => {
            let auth = &rest[..idx];
            let p = &rest[idx..];
            let clean_p = if p.is_empty() || p == "/" {
                "/ws".to_string()
            } else {
                p.to_string()
            };
            (auth, clean_p)
        }
        None => (rest, "/ws".to_string()),
    };

    let ws_url = format!("{scheme}{authority}{path}");
    let host = authority.split(':').next().unwrap_or("127.0.0.1");
    let clean_host = if host.is_empty() { "127.0.0.1" } else { host };
    (ws_url, clean_host.to_string())
}

pub async fn resolve_hub_addr_async(raw: &str) -> (String, SocketAddr) {
    let (ws_url, host) = extract_hub_parts(raw);
    let udp_target = format!("{host}:9000");

    let udp_addr = match tokio::net::lookup_host(&udp_target).await {
        Ok(mut addrs) => addrs
            .find(|a| a.is_ipv4())
            .or_else(|| addrs.next())
            .unwrap_or_else(|| SocketAddr::from(([127, 0, 0, 1], 9000))),
        Err(e) => {
            tracing::warn!(%udp_target, error = %e, "Failed to resolve hub UDP host; falling back to 127.0.0.1:9000");
            SocketAddr::from(([127, 0, 0, 1], 9000))
        }
    };

    (ws_url, udp_addr)
}

pub async fn fetch_room_list_async(raw_hub_url: &str) -> Result<String, String> {
    let (ws_url, _) = extract_hub_parts(raw_hub_url);
    let servers = runner::fetch_server_list(&ws_url).await?;
    serde_json::to_string(&servers).map_err(|e| format!("Failed to serialize server list: {e}"))
}

pub fn fetch_room_list(raw_hub_url: &str) -> Result<String, String> {
    let rt = get_or_create_runtime()?;
    let url = raw_hub_url.to_string();
    rt.block_on(async move {
        fetch_room_list_async(&url).await
    })
}

