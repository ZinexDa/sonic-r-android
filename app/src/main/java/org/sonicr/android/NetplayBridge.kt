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
