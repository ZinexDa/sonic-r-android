package org.sonicr.android

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView

class RoomAdapter(
    private val onRoomSelected: (RoomInfo) -> Unit,
    private val onJoinClicked: (RoomInfo) -> Unit
) : RecyclerView.Adapter<RoomAdapter.RoomViewHolder>() {

    private val rooms = mutableListOf<RoomInfo>()
    private var selectedPosition = -1

    fun setRooms(newRooms: List<RoomInfo>) {
        rooms.clear()
        rooms.addAll(newRooms)
        selectedPosition = if (rooms.isNotEmpty()) 0 else -1
        notifyDataSetChanged()
        if (selectedPosition >= 0 && selectedPosition < rooms.size) {
            onRoomSelected(rooms[selectedPosition])
        }
    }

    fun getSelectedRoom(): RoomInfo? {
        if (selectedPosition in rooms.indices) {
            return rooms[selectedPosition]
        }
        return null
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RoomViewHolder {
        val view = LayoutInflater.from(parent.context).inflate(R.layout.item_room_card, parent, false)
        return RoomViewHolder(view)
    }

    override fun onBindViewHolder(holder: RoomViewHolder, position: Int) {
        val room = rooms[position]
        val isSelected = position == selectedPosition
        holder.bind(room, isSelected)
    }

    override fun getItemCount(): Int = rooms.size

    inner class RoomViewHolder(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val cardRoot: View = itemView.findViewById(R.id.card_root)
        private val tvRoomName: TextView = itemView.findViewById(R.id.tv_room_name)
        private val tvRoomStatus: TextView = itemView.findViewById(R.id.tv_room_status)
        private val tvRoomVersion: TextView = itemView.findViewById(R.id.tv_room_version)
        private val tvPlayerCount: TextView = itemView.findViewById(R.id.tv_player_count)
        private val btnJoin: Button = itemView.findViewById(R.id.btn_join_room)

        fun bind(room: RoomInfo, isSelected: Boolean) {
            cardRoot.isSelected = isSelected
            tvRoomName.text = room.name
            tvPlayerCount.text = room.playerCountText

            if (room.gameVersion.isNotBlank()) {
                tvRoomVersion.text = room.gameVersion
                tvRoomVersion.visibility = View.VISIBLE
            } else {
                tvRoomVersion.visibility = View.GONE
            }

            if (room.isJoinable) {
                tvRoomStatus.text = "In Lobby"
                tvRoomStatus.setBackgroundResource(R.drawable.bg_badge_lobby)
                tvRoomStatus.setTextColor(0xFF34D399.toInt())
                btnJoin.isEnabled = true
                btnJoin.alpha = 1.0f
            } else {
                tvRoomStatus.text = if (room.players >= room.maxPlayers) "Full" else "In Race"
                tvRoomStatus.setBackgroundResource(R.drawable.bg_badge_race)
                tvRoomStatus.setTextColor(0xFFFBBF24.toInt())
                btnJoin.isEnabled = false
                btnJoin.alpha = 0.5f
            }

            cardRoot.setOnClickListener {
                val prev = selectedPosition
                selectedPosition = bindingAdapterPosition
                if (prev != selectedPosition) {
                    notifyItemChanged(prev)
                    notifyItemChanged(selectedPosition)
                }
                onRoomSelected(room)
            }

            btnJoin.setOnClickListener {
                onJoinClicked(room)
            }
        }
    }
}
