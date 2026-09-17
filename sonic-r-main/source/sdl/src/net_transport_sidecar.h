#ifndef NET_TRANSPORT_SIDECAR_H
#define NET_TRANSPORT_SIDECAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize netplay runtime, crypto provider, and logging.
 * Returns 0 on success, -1 on failure.
 */
int netplay_init(void);

/**
 * Start hosting a netplay room in a background thread and proxy traffic to/from local game port.
 * Returns 0 on success, -1 on invalid argument, -2 on runtime failure.
 */
int netplay_start_host(const char *hub_url, const char *room_name, uint16_t game_port);

/**
 * Join an existing netplay room on the hub.
 * Returns 0 on success, -1 on invalid argument, -2 on invalid UUID, -3 on runtime failure.
 */
int netplay_start_join(const char *hub_url, const char *room_id, uint16_t game_port);

/**
 * Stop any active netplay host or join session and close sockets.
 * Safe to call multiple times (idempotent).
 */
void netplay_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_TRANSPORT_SIDECAR_H */
