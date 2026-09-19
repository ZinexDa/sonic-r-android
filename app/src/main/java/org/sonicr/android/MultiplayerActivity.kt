package org.sonicr.android

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.recyclerview.widget.LinearLayoutManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sonicr.android.databinding.ActivityMultiplayerBinding

class MultiplayerActivity : Activity() {

    companion object {
        private const val TAG = "MultiplayerActivity"
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

    private lateinit var roomAdapter: RoomAdapter
    private var selectedRoom: RoomInfo? = null

    private var hubAddressText: String
        get() = if (isHostMode) {
            binding.etHubAddressHost.text.toString().trim()
        } else {
            binding.etHubAddressJoin.text.toString().trim()
        }
        set(value) {
            binding.etHubAddressHost.setText(value)
            binding.etHubAddressJoin.setText(value)
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMultiplayerBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupRecyclerView()
        loadSavedPreferences()
        updateRoleUI()

        binding.btnRoleHost.setOnClickListener {
            if (!isConnecting && !isHostMode) {
                syncHubAddressText(fromHost = false)
                isHostMode = true
                updateRoleUI()
            }
        }

        binding.btnRoleJoin.setOnClickListener {
            if (!isConnecting && isHostMode) {
                syncHubAddressText(fromHost = true)
                isHostMode = false
                updateRoleUI()
            }
        }

        binding.btnRefresh.setOnClickListener {
            if (!isConnecting) {
                refreshRoomList()
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

    private fun setupRecyclerView() {
        roomAdapter = RoomAdapter(
            onRoomSelected = { room ->
                selectedRoom = room
                if (!isHostMode) {
                    binding.btnStartMatchmaking.isEnabled = room.isJoinable
                }
                binding.tvStatus.text = "Selected: ${room.name} (${room.playerCountText}) - ${room.statusText}"
            },
            onJoinClicked = { room ->
                if (AssetInstaller.isAssetsInstalled(this)) {
                    joinRoomAndLaunch(room)
                } else {
                    Toast.makeText(this, "Game data files not installed. Please install assets first.", Toast.LENGTH_LONG).show()
                }
            }
        )
        binding.rvRooms.layoutManager = LinearLayoutManager(this)
        binding.rvRooms.adapter = roomAdapter
    }

    private fun syncHubAddressText(fromHost: Boolean) {
        if (fromHost) {
            val text = binding.etHubAddressHost.text.toString()
            binding.etHubAddressJoin.setText(text)
        } else {
            val text = binding.etHubAddressJoin.text.toString()
            binding.etHubAddressHost.setText(text)
        }
    }

    private fun loadSavedPreferences() {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val savedHub = prefs.getString(KEY_HUB_ADDRESS, DEFAULT_HUB_ADDRESS)
        val initialHub = if (savedHub.isNullOrBlank() || savedHub == "192.168.0.103:8080") DEFAULT_HUB_ADDRESS else savedHub
        hubAddressText = initialHub
        binding.etRoomName.setText(prefs.getString(KEY_ROOM_NAME, DEFAULT_ROOM_NAME))
        isHostMode = prefs.getBoolean(KEY_IS_HOST, true)
    }

    private fun savePreferences() {
        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putString(KEY_HUB_ADDRESS, hubAddressText)
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

            binding.layoutHostInputs.visibility = View.VISIBLE
            binding.layoutJoinHub.visibility = View.GONE
            binding.layoutServerBrowser.visibility = View.GONE
            binding.btnStartMatchmaking.text = "HOST & LAUNCH"
            binding.btnStartMatchmaking.isEnabled = true
            binding.tvStatus.text = "Configure host settings and click HOST & LAUNCH."
        } else {
            binding.btnRoleJoin.setBackgroundResource(R.drawable.btn_primary)
            binding.btnRoleJoin.setTextColor(0xFF261200.toInt())
            binding.btnRoleHost.setBackgroundResource(R.drawable.btn_secondary)
            binding.btnRoleHost.setTextColor(0xFFE0F2FE.toInt())

            binding.layoutHostInputs.visibility = View.GONE
            binding.layoutJoinHub.visibility = View.VISIBLE
            binding.layoutServerBrowser.visibility = View.VISIBLE
            binding.btnStartMatchmaking.text = "JOIN & LAUNCH"
            binding.btnStartMatchmaking.isEnabled = selectedRoom?.isJoinable == true

            refreshRoomList()
        }
    }

    private fun refreshRoomList() {
        val hubUrl = hubAddressText.ifEmpty { DEFAULT_HUB_ADDRESS }
        android.util.Log.i(TAG, "refreshRoomList: querying hub at $hubUrl")
        binding.progressBar.visibility = View.VISIBLE
        binding.tvStatus.text = "Connecting to hub at $hubUrl..."
        binding.btnRefresh.isEnabled = false

        scope.launch {
            val json = withContext(Dispatchers.IO) {
                try {
                    NetplayBridge.fetchRoomList(hubUrl)
                } catch (t: Throwable) {
                    android.util.Log.e(TAG, "fetchRoomList failed", t)
                    null
                }
            }
            android.util.Log.i(TAG, "refreshRoomList: received json: $json")
            binding.progressBar.visibility = View.GONE
            binding.btnRefresh.isEnabled = true

            val rooms = NetplayBridge.parseRooms(json)
            binding.tvRoomCount.text = "${rooms.size} online"

            if (rooms.isEmpty()) {
                binding.rvRooms.visibility = View.GONE
                binding.layoutEmptyState.visibility = View.VISIBLE
                binding.tvEmptyState.text = "No active rooms found.\nHost a room or tap Refresh."
                binding.tvStatus.text = "No active rooms found on hub."
                selectedRoom = null
                if (!isHostMode) {
                    binding.btnStartMatchmaking.isEnabled = false
                }
            } else {
                binding.layoutEmptyState.visibility = View.GONE
                binding.rvRooms.visibility = View.VISIBLE
                roomAdapter.setRooms(rooms)
                selectedRoom = roomAdapter.getSelectedRoom()
                if (!isHostMode) {
                    binding.btnStartMatchmaking.isEnabled = selectedRoom?.isJoinable == true
                }
                val joinableCount = rooms.count { it.isJoinable }
                binding.tvStatus.text = "Found ${rooms.size} room(s) ($joinableCount joinable). Tap JOIN on any card."
            }
        }
    }

    private fun joinRoomAndLaunch(room: RoomInfo) {
        val hubUrl = hubAddressText.ifEmpty { DEFAULT_HUB_ADDRESS }
        savePreferences()

        isConnecting = true
        binding.progressBar.visibility = View.VISIBLE
        binding.tvStatus.text = "Joining room \"${room.name}\"..."
        binding.btnStartMatchmaking.isEnabled = false
        binding.btnRoleHost.isEnabled = false
        binding.btnRoleJoin.isEnabled = false
        binding.btnRefresh.isEnabled = false

        val intent = Intent(this@MultiplayerActivity, GameActivity::class.java).apply {
            putExtra("IS_NETPLAY", true)
            putExtra("IS_HOST", false)
            putExtra("GAME_PORT", 5029)
            putExtra("HUB_URL", hubUrl)
            putExtra("ROOM_NAME", room.name)
            putExtra("ROOM_ID", room.id)
        }
        startActivity(intent)
        finish()
    }

    private fun startMatchmakingAndLaunch() {
        if (isHostMode) {
            val hubUrl = hubAddressText.ifEmpty { DEFAULT_HUB_ADDRESS }
            val roomName = binding.etRoomName.text.toString().trim().ifEmpty { DEFAULT_ROOM_NAME }

            savePreferences()

            isConnecting = true
            binding.progressBar.visibility = View.VISIBLE
            binding.tvStatus.text = "Launching multiplayer host session..."
            binding.btnStartMatchmaking.isEnabled = false
            binding.btnRoleHost.isEnabled = false
            binding.btnRoleJoin.isEnabled = false

            val intent = Intent(this@MultiplayerActivity, GameActivity::class.java).apply {
                putExtra("IS_NETPLAY", true)
                putExtra("IS_HOST", true)
                putExtra("GAME_PORT", 5029)
                putExtra("HUB_URL", hubUrl)
                putExtra("ROOM_NAME", roomName)
                putExtra("ROOM_ID", null as String?)
            }
            startActivity(intent)
            finish()
        } else {
            val room = selectedRoom ?: roomAdapter.getSelectedRoom()
            if (room != null) {
                joinRoomAndLaunch(room)
            } else {
                Toast.makeText(this, "Please select an active room to join", Toast.LENGTH_SHORT).show()
            }
        }
    }

    override fun onDestroy() {
        NetplayBridge.stop()
        super.onDestroy()
    }
}

