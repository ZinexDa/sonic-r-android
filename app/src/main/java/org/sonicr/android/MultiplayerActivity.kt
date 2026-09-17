package org.sonicr.android

import android.app.Activity
import android.os.Bundle
import org.sonicr.android.databinding.ActivityPlaceholderBinding

class MultiplayerActivity : Activity() {

    private lateinit var binding: ActivityPlaceholderBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityPlaceholderBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.placeholderTitle.setText(R.string.title_multiplayer)
        binding.placeholderDesc.setText(R.string.desc_multiplayer_coming_soon)

        binding.btnBack.setOnClickListener {
            finish()
        }

        val initResult = NetplayBridge.init()
        android.util.Log.i("MultiplayerActivity", "NetplayBridge init result: $initResult (loaded=${NetplayBridge.isLibraryLoaded()})")
    }
}
