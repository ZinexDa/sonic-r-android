package org.sonicr.android

import android.util.Log

/**
 * JNI Bridge to the native Rust netplay sidecar library (libsonicr_netplay.so).
 *
 * Provides control over P2P session hosting, joining, loopback proxying,
 * and background hole punching.
 */
object NetplayBridge {
    private const val TAG = "NetplayBridge"

    private var isLoaded = false

    init {
        try {
            System.loadLibrary("sonicr_netplay")
            isLoaded = true
            Log.i(TAG, "libsonicr_netplay.so loaded successfully")
        } catch (e: UnsatisfiedLinkError) {
            Log.e(TAG, "Failed to load libsonicr_netplay.so", e)
        }
    }

    fun isLibraryLoaded(): Boolean = isLoaded

    fun init(): Int {
        if (!isLoaded) return -1
        return nativeInit()
    }

    fun startHost(hubUrl: String, roomName: String?, gamePort: Int = 5029): Int {
        if (!isLoaded) return -1
        return nativeStartHost(hubUrl, roomName, gamePort)
    }

    fun startJoin(hubUrl: String, roomId: String?, gamePort: Int = 5029): Int {
        if (!isLoaded) return -1
        return nativeStartJoin(hubUrl, roomId, gamePort)
    }

    fun stop() {
        if (isLoaded) {
            nativeStop()
        }
    }

    @JvmStatic
    external fun fetchRoomList(hubUrl: String): String?

    fun parseRooms(jsonStr: String?): List<RoomInfo> {
        if (jsonStr.isNullOrBlank()) return emptyList()
        val list = mutableListOf<RoomInfo>()
        try {
            val arr = org.json.JSONArray(jsonStr)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                list.add(
                    RoomInfo(
                        id = obj.getString("id"),
                        name = obj.optString("name", "Sonic Room"),
                        players = obj.optInt("players", 1),
                        maxPlayers = obj.optInt("max_players", 4),
                        gameVersion = obj.optString("game_version", ""),
                        status = obj.optString("status", "InLobby")
                    )
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to parse room list JSON: ${e.message}")
        }
        return list
    }

    // Native JNI functions
    @JvmStatic
    private external fun nativeInit(): Int

    @JvmStatic
    private external fun nativeStartHost(hubUrl: String, roomName: String?, gamePort: Int): Int

    @JvmStatic
    private external fun nativeStartJoin(hubUrl: String, roomId: String?, gamePort: Int): Int

    @JvmStatic
    private external fun nativeStop()
}

/**
 * Information about a room discovered from the signaling hub.
 */
data class RoomInfo(
    val id: String,
    val name: String,
    val players: Int,
    val maxPlayers: Int,
    val gameVersion: String = "",
    val status: String = "InLobby"
) {
    val isInLobby: Boolean
        get() = status.replace("_", "").equals("inlobby", ignoreCase = true)

    val isJoinable: Boolean
        get() = isInLobby && players < maxPlayers

    val playerCountText: String
        get() = "$players/$maxPlayers"

    val statusText: String
        get() = if (isInLobby) "In Lobby" else "In Race"
}

