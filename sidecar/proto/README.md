# Proto

Shared protocol definitions and serialization models for the Sonic R network signaling system.

## Overview

All signaling between clients/hosts (sidecar) and the central signaling server (hub) is transported via JSON over WebSocket.

Each message enum uses internally tagged JSON (`#[serde(tag = "type")]`).

## Messages

### Client to Hub (`ClientMessage`)

- `RegisterHost`:
  ```json
  {
    "type": "RegisterHost",
    "name": "SpeedyZone",
    "max_players": 4,
    "game_version": "0.1.0",
    "punch_token": "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d"
  }
  ```
- `ListServers`:
  ```json
  {
    "type": "ListServers"
  }
  ```
- `JoinRequest`:
  ```json
  {
    "type": "JoinRequest",
    "server_id": "00000000-0000-0000-0000-000000000000",
    "punch_token": "b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e"
  }
  ```
- `Heartbeat`:
  ```json
  {
    "type": "Heartbeat",
    "players": 2
  }
  ```
- `RelayFallback`:
  ```json
  {
    "type": "RelayFallback",
    "punch_token": "b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e"
  }
  ```

### Hub to Client (`HubMessage`)

- `Registered`:
  ```json
  {
    "type": "Registered",
    "server_id": "00000000-0000-0000-0000-000000000000"
  }
  ```
- `ServerList`:
  ```json
  {
    "type": "ServerList",
    "servers": [
      {
        "id": "00000000-0000-0000-0000-000000000000",
        "name": "SpeedyZone",
        "players": 1,
        "max_players": 4,
        "game_version": "0.1.0"
      }
    ]
  }
  ```
- `PeerCandidate`:
  ```json
  {
    "type": "PeerCandidate",
    "peer_addr": "203.0.113.10:54321",
    "punch_token": "b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e"
  }
  ```
- `UseRelay`:
  ```json
  {
    "type": "UseRelay",
    "punch_token": "b2c3d4e5-f6a7-4b8c-9d0e-1f2a3b4c5d6e",
    "relay_addr": "203.0.113.1:9001"
  }
  ```
- `Error`:
  ```json
  {
    "type": "Error",
    "message": "Punch timeout"
  }
  ```
