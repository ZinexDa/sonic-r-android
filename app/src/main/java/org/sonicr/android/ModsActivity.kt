package org.sonicr.android

import android.app.Activity
import android.os.Bundle
import org.sonicr.android.databinding.ActivityPlaceholderBinding

class ModsActivity : Activity() {

    private lateinit var binding: ActivityPlaceholderBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityPlaceholderBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.placeholderTitle.setText(R.string.title_mods)
        binding.placeholderDesc.setText(R.string.desc_mods_coming_soon)

        binding.btnBack.setOnClickListener {
            finish()
        }
    }
}
