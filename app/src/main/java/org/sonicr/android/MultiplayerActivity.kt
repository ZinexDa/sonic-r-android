package org.sonicr.android

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sonicr.android.databinding.ActivityMultiplayerBinding

class MultiplayerActivity : Activity() {

    companion object {
        private const val PREFS_NAME = "sonicr_netplay_prefs"
        private const val KEY_HUB_ADDRESS = "last_hub_address"
        private const val KEY_ROOM_NAME = "last_room_name"
        private const val KEY_IS_HOST = "last_is_host"
        private const val DEFAULT_HUB_ADDRESS = "127.0.0.1:8080"
        private const val DEFAULT_ROOM_NAME = "Sonic Room"
    }

    private lateinit var binding: ActivityMultiplayerBinding
    private val scope = CoroutineScope(Dispatchers.Main + Job())
    private var isHostMode = true
    private var isConnecting = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMultiplayerBinding.inflate(layoutInflater)
        setContentView(binding.root)

        loadSavedPreferences()
        updateRoleUI()

        binding.btnRoleHost.setOnClickListener {
            if (!isConnecting) {
                isHostMode = true
                updateRoleUI()
            }
        }

        binding.btnRoleJoin.setOnClickListener {
            if (!isConnecting) {
                isHostMode = false
                updateRoleUI()
            }
        }

        binding.btnStartMatchmaking.setOnClickListener {
            if (AssetInstaller.isAssetsInstalled(this)) {
                startMatchmakingAndLaunch()
            } else {
                Toast.makeText(this, "Game data files not installed. Please install assets first.", Toast.LENGTH_LONG).show()
            }
        }

        binding.btnCancel.setOnClickListener {
            finish()
        }
    }

    private fun loadSavedPreferences() {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        binding.etHubAddress.setText(prefs.getString(KEY_HUB_ADDRESS, DEFAULT_HUB_ADDRESS))
        binding.etRoomName.setText(prefs.getString(KEY_ROOM_NAME, DEFAULT_ROOM_NAME))
        isHostMode = prefs.getBoolean(KEY_IS_HOST, true)
    }

    private fun savePreferences() {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putString(KEY_HUB_ADDRESS, binding.etHubAddress.text.toString().trim())
            .putString(KEY_ROOM_NAME, binding.etRoomName.text.toString().trim())
            .putBoolean(KEY_IS_HOST, isHostMode)
            .apply()
    }

    private fun updateRoleUI() {
        if (isHostMode) {
            binding.btnRoleHost.setBackgroundResource(R.drawable.btn_primary)
            binding.btnRoleHost.setTextColor(0xFF261200.toInt())
            binding.btnRoleJoin.setBackgroundResource(R.drawable.btn_secondary)
            binding.btnRoleJoin.setTextColor(0xFFE0F2FE.toInt())

            binding.layoutRoomName.visibility = View.VISIBLE
            binding.layoutRoomId.visibility = View.GONE
            binding.btnStartMatchmaking.text = "HOST & LAUNCH"
        } else {
            binding.btnRoleJoin.setBackgroundResource(R.drawable.btn_primary)
            binding.btnRoleJoin.setTextColor(0xFF261200.toInt())
            binding.btnRoleHost.setBackgroundResource(R.drawable.btn_secondary)
            binding.btnRoleHost.setTextColor(0xFFE0F2FE.toInt())

            binding.layoutRoomName.visibility = View.GONE
            binding.layoutRoomId.visibility = View.VISIBLE
            binding.btnStartMatchmaking.text = "JOIN & LAUNCH"
        }
    }

    private fun startMatchmakingAndLaunch() {
        val hubUrl = binding.etHubAddress.text.toString().trim().ifEmpty { DEFAULT_HUB_ADDRESS }
        val roomName = binding.etRoomName.text.toString().trim().ifEmpty { DEFAULT_ROOM_NAME }
        val roomId = binding.etRoomId.text.toString().trim().ifEmpty { null }

        savePreferences()

        isConnecting = true
        binding.progressBar.visibility = View.VISIBLE
        binding.tvStatus.text = if (isHostMode) {
            "Connecting to hub & registering room..."
        } else {
            "Connecting to hub & finding room..."
        }
        binding.btnStartMatchmaking.isEnabled = false
        binding.btnRoleHost.isEnabled = false
        binding.btnRoleJoin.isEnabled = false

        scope.launch(Dispatchers.IO) {
            val result = if (isHostMode) {
                NetplayBridge.startHost(hubUrl = hubUrl, roomName = roomName, gamePort = 5029)
            } else {
                NetplayBridge.startJoin(hubUrl = hubUrl, roomId = roomId, gamePort = 5029)
            }

            withContext(Dispatchers.Main) {
                if (result == 0) {
                    binding.tvStatus.text = "Session active! Launching game..."
                    val intent = Intent(this@MultiplayerActivity, GameActivity::class.java).apply {
                        putExtra("IS_NETPLAY", true)
                        putExtra("IS_HOST", isHostMode)
                        putExtra("GAME_PORT", 5029)
                    }
                    startActivity(intent)
                    finish()
                } else {
                    isConnecting = false
                    binding.progressBar.visibility = View.GONE
                    binding.btnStartMatchmaking.isEnabled = true
                    binding.btnRoleHost.isEnabled = true
                    binding.btnRoleJoin.isEnabled = true
                    binding.tvStatus.text = "Failed to start netplay session (error code $result)"
                }
            }
        }
    }

    override fun onDestroy() {
        NetplayBridge.stop()
        super.onDestroy()
    }
}
