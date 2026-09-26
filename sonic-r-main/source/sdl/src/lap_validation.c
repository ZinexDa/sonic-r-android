/**
 * lap_validation.c — Checkpoint and lap validation system for anti-cheat & fairness
 */

#include "lap_validation.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned char checkpointMask;   /* Bitmask of visited checkpoints (0x01, 0x02, 0x04) */
    unsigned int  lapStartMs;       /* Local timeGetTime() timestamp when current lap began */
    short         lastLap;          /* Last observed lapsCompleted for player */
} LapPlayerState;

static LapPlayerState s_lapState[MAX_PLAYERS];

void LapValidation_ResetAll(void)
{
    unsigned int now = (unsigned int)timeGetTime();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        s_lapState[i].checkpointMask = 0;
        s_lapState[i].lapStartMs = now;
        s_lapState[i].lastLap = 0;
    }
}

void LapValidation_ResetPlayer(int playerIdx)
{
    if (playerIdx >= 0 && playerIdx < MAX_PLAYERS) {
        s_lapState[playerIdx].checkpointMask = 0;
        s_lapState[playerIdx].lapStartMs = (unsigned int)timeGetTime();
        s_lapState[playerIdx].lastLap = 0;
    }
}

void LapValidation_Update(int playerIdx, const Player *player)
{
    if (playerIdx < 0 || playerIdx >= MAX_PLAYERS || player == NULL) {
        return;
    }

    /* Tag mode and Balloon mode do not use laps */
    if (g_raceSubMode >= SUBMODE_TAG) {
        return;
    }

    /* Spline waypoints must be valid */
    if (g_splineWaypoints == NULL || g_posDataCount4 < 10) {
        return;
    }

    /* If still in intro countdown, keep state reset so race start is clean */
    if (g_introCountdown > 0) {
        s_lapState[playerIdx].checkpointMask = 0;
        s_lapState[playerIdx].lapStartMs = (unsigned int)timeGetTime();
        s_lapState[playerIdx].lastLap = player->lapsCompleted;
        return;
    }

    /* If player lap completed changed externally or locally, sync state */
    if (player->lapsCompleted != s_lapState[playerIdx].lastLap) {
        s_lapState[playerIdx].checkpointMask = 0;
        s_lapState[playerIdx].lapStartMs = (unsigned int)timeGetTime();
        s_lapState[playerIdx].lastLap = player->lapsCompleted;
    }

    int playerX = player->posX >> 12;
    int playerY = -(player->posY >> 12);
    int playerZ = player->posZ >> 12;
    int count = g_posDataCount4;

    int wp = FindNearestWaypoint((int *)g_splineWaypoints,
                                 playerX, playerY, playerZ,
                                 count);

    int cp1_min, cp1_max, cp2_min, cp2_max, cp3_min, cp3_max;

    if (g_raceSubMode == SUBMODE_REVERSE) {
        /* Reverse Mode ranges with 10% overlap */
        cp1_min = (count * 55) / 100;
        cp1_max = (count * 90) / 100;

        cp2_min = (count * 25) / 100;
        cp2_max = (count * 65) / 100;

        cp3_min = (count * 2)  / 100;
        cp3_max = (count * 35) / 100;
    } else {
        /* Forward Mode ranges with 10% overlap */
        cp1_min = (count * 10) / 100;
        cp1_max = (count * 45) / 100;

        cp2_min = (count * 35) / 100;
        cp2_max = (count * 75) / 100;

        cp3_min = (count * 65) / 100;
        cp3_max = (count * 98) / 100;
    }

    /* Sequential checkpoint gating */
    if (wp >= cp1_min && wp <= cp1_max) {
        s_lapState[playerIdx].checkpointMask |= LAP_CP_1;
    }
    if (wp >= cp2_min && wp <= cp2_max) {
        if (s_lapState[playerIdx].checkpointMask & LAP_CP_1) {
            s_lapState[playerIdx].checkpointMask |= LAP_CP_2;
        }
    }
    if (wp >= cp3_min && wp <= cp3_max) {
        if (s_lapState[playerIdx].checkpointMask & LAP_CP_2) {
            s_lapState[playerIdx].checkpointMask |= LAP_CP_3;
        }
    }
}

int LapValidation_CheckLocalLap(int playerIdx, const Player *player)
{
    if (playerIdx < 0 || playerIdx >= MAX_PLAYERS || player == NULL) {
        return 1;
    }

    if (g_raceSubMode >= SUBMODE_TAG) {
        return 1;
    }

    if (g_splineWaypoints == NULL || g_posDataCount4 < 10) {
        return 1;
    }

    /* Gate A: Checkpoints traversal check */
    if ((s_lapState[playerIdx].checkpointMask & LAP_CP_ALL) != LAP_CP_ALL) {
        return 0;
    }

    /* Gate B: Minimum lap time check */
    int lapTicks = 0;
    short lap = player->lapsCompleted;
    if (lap == 0) lapTicks = player->lap1Time;
    else if (lap == 1) lapTicks = player->lap2Time;
    else if (lap == 2) lapTicks = player->lap3Time;
    else return 1;

    if (lapTicks < MIN_PLAUSIBLE_LAP_TICKS) {
        return 0;
    }

    return 1;
}

int LapValidation_CheckRemoteLap(int playerIdx, const Player *player, short prevLaps)
{
    if (playerIdx < 0 || playerIdx >= MAX_PLAYERS || player == NULL) {
        return 1;
    }

    if (g_raceSubMode >= SUBMODE_TAG) {
        return 1;
    }

    if (g_splineWaypoints == NULL || g_posDataCount4 < 10) {
        return 1;
    }

    /* Reject multi-lap jump (e.g. 0 -> 2, 0 -> 3) */
    if (player->lapsCompleted > prevLaps + 1) {
        printf("[ANTI-CHEAT] Rejecting remote player %d: illegal lap jump %d -> %d\n",
               playerIdx, (int)prevLaps, (int)player->lapsCompleted);
        return 0;
    }

    /* Gate A: Checkpoint traversal check */
    if ((s_lapState[playerIdx].checkpointMask & LAP_CP_ALL) != LAP_CP_ALL) {
        printf("[ANTI-CHEAT] Rejecting remote player %d: claimed lap %d without checkpoints (mask: 0x%02X)\n",
               playerIdx, (int)player->lapsCompleted, (unsigned int)s_lapState[playerIdx].checkpointMask);
        return 0;
    }

    /* Gate B: Minimum elapsed time check */
    unsigned int nowMs = (unsigned int)timeGetTime();
    unsigned int elapsedMs = nowMs - s_lapState[playerIdx].lapStartMs;
    if (elapsedMs < MIN_PLAUSIBLE_LAP_MS) {
        printf("[ANTI-CHEAT] Rejecting remote player %d: claimed lap %d in %u ms (< %u ms)\n",
               playerIdx, (int)player->lapsCompleted, elapsedMs, (unsigned int)MIN_PLAUSIBLE_LAP_MS);
        return 0;
    }

    /* Valid remote lap accepted — reset for next lap */
    LapValidation_OnLapCredited(playerIdx);
    return 1;
}

void LapValidation_OnLapCredited(int playerIdx)
{
    if (playerIdx >= 0 && playerIdx < MAX_PLAYERS) {
        s_lapState[playerIdx].checkpointMask = 0;
        s_lapState[playerIdx].lapStartMs = (unsigned int)timeGetTime();
        if (g_playerBase != NULL) {
            s_lapState[playerIdx].lastLap = g_playerBase[playerIdx].lapsCompleted;
        }
    }
}
