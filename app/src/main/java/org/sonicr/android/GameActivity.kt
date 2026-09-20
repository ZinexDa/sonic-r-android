package org.sonicr.android

import android.content.Context
import android.os.Bundle
import android.os.Process
import android.util.Log
import android.view.KeyEvent
import android.view.WindowManager
import org.libsdl.app.SDLActivity

/**
 * GameActivity hosts the native Sonic R C game engine and SDL2 window.
 * Configured in AndroidManifest.xml with android:process=":game" to ensure
 * clean memory isolation across launches and restarts.
 */
class GameActivity : SDLActivity() {

    companion object {
        private const val TAG = "SonicR_GameActivity"

        @JvmStatic
        external fun nativeSetMusicVolume(volume: Float)

        @JvmStatic
        external fun nativeSetSfxVolume(volume: Float)

        @JvmStatic
        external fun nativeSetControlLayout(
            customEnabled: Boolean,
            dpadX: Float, dpadY: Float, dpadScale: Float,
            driftLX: Float, driftLY: Float, driftLScale: Float,
            accelX: Float, accelY: Float, accelScale: Float,
            jumpX: Float, jumpY: Float, jumpScale: Float,
            driftRX: Float, driftRY: Float, driftRScale: Float,
            lookX: Float, lookY: Float, lookScale: Float,
            startX: Float, startY: Float, startScale: Float
        )

        @JvmStatic
        external fun nativeSetNetplayMode(
            isNetplay: Boolean,
            isHost: Boolean,
            hostIp: String,
            port: Int
        )

        @JvmStatic
        external fun nativeSaveSettings()
    }

    override fun getLibraries(): Array<String> {
        return arrayOf(
            "sonicr_netplay",
            "SDL2",
            "main"
        )
    }

    override fun getArguments(): Array<String> {
        val isNetplay = intent?.getBooleanExtra("IS_NETPLAY", false) ?: false
        if (!isNetplay) {
            return emptyArray()
        }
        val isHost = intent?.getBooleanExtra("IS_HOST", true) ?: true
        val port = intent?.getIntExtra("GAME_PORT", 5029) ?: 5029
        val enginePort = if (isHost) 5030 else 5031
        val args = mutableListOf<String>()
        if (isHost) {
            args.add("-H")
            args.add("-h")
            args.add("127.0.0.1")
            args.add("-p")
            args.add("5030")
        } else {
            args.add("-J")
            args.add("-h")
            args.add("127.0.0.1")
            args.add("-p")
            args.add("5029")
        }
        Log.i(TAG, "Passing SDL arguments for netplay: $args")
        return args.toTypedArray()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        applyNetplaySettings()
        applyAudioSettings()
        applyControlSettings()
    }

    override fun onResume() {
        super.onResume()
        applyNetplaySettings()
        applyAudioSettings()
        applyControlSettings()
    }

    override fun onPause() {
        super.onPause()
        try {
            nativeSaveSettings()
            Log.i(TAG, "onPause: saved settings to disk")
        } catch (e: Throwable) {
            Log.w(TAG, "nativeSaveSettings onPause failed: ${e.message}")
        }
    }

    private var isNetplayStarted = false

    private fun applyNetplaySettings() {
        val isNetplay = intent?.getBooleanExtra("IS_NETPLAY", false) ?: false
        if (!isNetplay) return
        val isHost = intent?.getBooleanExtra("IS_HOST", true) ?: true
        val port = intent?.getIntExtra("GAME_PORT", 5029) ?: 5029
        val enginePort = if (isHost) 5030 else 5031
        val hubUrl = intent?.getStringExtra("HUB_URL") ?: "127.0.0.1:8080"
        val roomName = intent?.getStringExtra("ROOM_NAME") ?: "Sonic Room"
        val roomId = intent?.getStringExtra("ROOM_ID")

        try {
            val targetHostPort = if (isHost) 5030 else 5029
            nativeSetNetplayMode(true, isHost, "127.0.0.1", targetHostPort)

            if (!isNetplayStarted) {
                isNetplayStarted = true
                Log.i(TAG, "Starting netplay session: isHost=$isHost, enginePort=$enginePort, hubUrl=$hubUrl, roomName=$roomName, roomId=$roomId")
                val initRes = NetplayBridge.init()
                Log.i(TAG, "NetplayBridge.init() returned $initRes")

                val startRes = if (isHost) {
                    NetplayBridge.startHost(hubUrl, roomName, enginePort)
                } else {
                    NetplayBridge.startJoin(hubUrl, roomId, enginePort)
                }
                Log.i(TAG, "NetplayBridge session started (res=$startRes, isHost=$isHost)")
            }
        } catch (e: UnsatisfiedLinkError) {
            Log.w(TAG, "Native netplay method not yet linked: ${e.message}")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to apply netplay settings: ${e.message}")
        }
    }

    private fun applyAudioSettings() {
        try {
            val prefs = getSharedPreferences(SettingsActivity.PREFS_NAME, Context.MODE_PRIVATE)
            val musicVol = intent?.getFloatExtra(SettingsActivity.KEY_MUSIC_VOLUME, -1f)?.takeIf { it >= 0f }
                ?: prefs.getFloat(SettingsActivity.KEY_MUSIC_VOLUME, SettingsActivity.DEFAULT_MUSIC_VOLUME)
            val sfxVol = intent?.getFloatExtra(SettingsActivity.KEY_SFX_VOLUME, -1f)?.takeIf { it >= 0f }
                ?: prefs.getFloat(SettingsActivity.KEY_SFX_VOLUME, SettingsActivity.DEFAULT_SFX_VOLUME)

            Log.i(TAG, "Applying audio settings: Music=$musicVol, SFX=$sfxVol")
            nativeSetMusicVolume(musicVol)
            nativeSetSfxVolume(sfxVol)
        } catch (e: UnsatisfiedLinkError) {
            Log.w(TAG, "Native audio methods not yet linked: ${e.message}")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to apply audio settings: ${e.message}")
        }
    }

    private fun applyControlSettings() {
        try {
            val prefs = getSharedPreferences(SettingsActivity.PREFS_NAME, Context.MODE_PRIVATE)
            val custom = prefs.getBoolean(SettingsActivity.KEY_CONTROLS_CUSTOM, false) &&
                prefs.contains(SettingsActivity.KEY_CTRL_DPAD_X)
            if (custom) {
                val dpX = prefs.getFloat(SettingsActivity.KEY_CTRL_DPAD_X, 0.10f)
                val dpY = prefs.getFloat(SettingsActivity.KEY_CTRL_DPAD_Y, 0.73f)
                val dpS = prefs.getFloat(SettingsActivity.KEY_CTRL_DPAD_SCALE, 1.0f)

                val dlX = prefs.getFloat(SettingsActivity.KEY_CTRL_DRIFT_L_X, 0.10f)
                val dlY = prefs.getFloat(SettingsActivity.KEY_CTRL_DRIFT_L_Y, 0.465f)
                val dlS = prefs.getFloat(SettingsActivity.KEY_CTRL_DRIFT_L_SCALE, 1.0f)

                val acX = prefs.getFloat(SettingsActivity.KEY_CTRL_ACCEL_X, 0.90f)
                val acY = prefs.getFloat(SettingsActivity.KEY_CTRL_ACCEL_Y, 0.71f)
                val acS = prefs.getFloat(SettingsActivity.KEY_CTRL_ACCEL_SCALE, 1.0f)

                val jmX = prefs.getFloat(SettingsActivity.KEY_CTRL_JUMP_X, 0.82f)
                val jmY = prefs.getFloat(SettingsActivity.KEY_CTRL_JUMP_Y, 0.81f)
                val jmS = prefs.getFloat(SettingsActivity.KEY_CTRL_JUMP_SCALE, 1.0f)

                val drX = prefs.getFloat(SettingsActivity.KEY_CTRL_DRIFT_R_X, 0.90f)
                val drY = prefs.getFloat(SettingsActivity.KEY_CTRL_DRIFT_R_Y, 0.53f)
                val drS = prefs.getFloat(SettingsActivity.KEY_CTRL_DRIFT_R_SCALE, 1.0f)

                val lkX = prefs.getFloat(SettingsActivity.KEY_CTRL_LOOK_X, 0.82f)
                val lkY = prefs.getFloat(SettingsActivity.KEY_CTRL_LOOK_Y, 0.63f)
                val lkS = prefs.getFloat(SettingsActivity.KEY_CTRL_LOOK_SCALE, 1.0f)

                val stX = prefs.getFloat(SettingsActivity.KEY_CTRL_START_X, 0.90f)
                val stY = prefs.getFloat(SettingsActivity.KEY_CTRL_START_Y, 0.09f)
                val stS = prefs.getFloat(SettingsActivity.KEY_CTRL_START_SCALE, 1.0f)

                Log.i(TAG, "Applying custom 7-control layout: DPad($dpX,$dpY,s=$dpS) DL($dlX,$dlY,s=$dlS) A($acX,$acY,s=$acS) B($jmX,$jmY,s=$jmS) DR($drX,$drY,s=$drS) Eye($lkX,$lkY,s=$lkS) Start($stX,$stY,s=$stS)")
                nativeSetControlLayout(
                    true,
                    dpX, dpY, dpS,
                    dlX, dlY, dlS,
                    acX, acY, acS,
                    jmX, jmY, jmS,
                    drX, drY, drS,
                    lkX, lkY, lkS,
                    stX, stY, stS
                )
            } else {
                Log.i(TAG, "Applying default control layout")
                nativeSetControlLayout(
                    false,
                    0f, 0f, 1f,
                    0f, 0f, 1f,
                    0f, 0f, 1f,
                    0f, 0f, 1f,
                    0f, 0f, 1f,
                    0f, 0f, 1f,
                    0f, 0f, 1f
                )
            }
        } catch (e: UnsatisfiedLinkError) {
            Log.w(TAG, "Native control layout methods not yet linked: ${e.message}")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to apply control settings: ${e.message}")
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BACK) {
            if (event.action == KeyEvent.ACTION_UP) {
                finish()
            }
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onDestroy() {
        try {
            nativeSaveSettings()
            Log.i(TAG, "onDestroy: saved settings before exit")
        } catch (_: Throwable) {}
        NetplayBridge.stop()
        // Terminate the isolated :game process immediately.
        // We intentionally do NOT call super.onDestroy() because SDLActivity.onDestroy()
        // calls mSDLThread.join() which hangs forever (the decompiled C engine runs an
        // infinite game loop). Killing the dedicated process allows the OS to immediately
        // and cleanly reclaim all native memory, threads, and GPU contexts.
        Process.killProcess(Process.myPid())
    }
}
