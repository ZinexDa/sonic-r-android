/**
 * lap_validation.h — Checkpoint and lap validation system for anti-cheat & fairness
 */

#ifndef LAP_VALIDATION_H
#define LAP_VALIDATION_H

#include "sonicr_types.h"
#include "player_struct.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimum plausible lap time in ticks (60Hz) and milliseconds */
#define MIN_PLAUSIBLE_LAP_TICKS 480     /* 8.0 seconds at 60Hz */
#define MIN_PLAUSIBLE_LAP_MS    8000    /* 8,000 ms wall clock */

/* Checkpoint bitmasks */
#define LAP_CP_1   (1 << 0)             /* 0x01: First Leg */
#define LAP_CP_2   (1 << 1)             /* 0x02: Midfield */
#define LAP_CP_3   (1 << 2)             /* 0x04: Final Stretch */
#define LAP_CP_ALL (LAP_CP_1 | LAP_CP_2 | LAP_CP_3) /* 0x07 */

/* Initialize or reset validation tracking for all players (at race/level start) */
void LapValidation_ResetAll(void);

/* Reset validation tracking for a single player slot */
void LapValidation_ResetPlayer(int playerIdx);

/* Update checkpoint progress for a player based on their current 3D position */
void LapValidation_Update(int playerIdx, const Player *player);

/* Validate local player lap completion upon Sector 7->6 crossing.
 * Returns 1 if valid (traversed checkpoints in order and met minimum lap time), 0 if invalid. */
int  LapValidation_CheckLocalLap(int playerIdx, const Player *player);

/* Validate remote player lap completion claimed via network packet.
 * Returns 1 if valid, 0 if rejected (bogus/premature/skip). */
int  LapValidation_CheckRemoteLap(int playerIdx, const Player *player, short prevLaps);

/* Called immediately after a valid lap is credited to advance tracking state for the next lap */
void LapValidation_OnLapCredited(int playerIdx);

#ifdef __cplusplus
}
#endif

#endif /* LAP_VALIDATION_H */
