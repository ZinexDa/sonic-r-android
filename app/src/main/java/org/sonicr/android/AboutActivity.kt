package org.sonicr.android

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import org.sonicr.android.databinding.ActivityAboutBinding

class AboutActivity : Activity() {

    companion object {
        private const val UPSTREAM_REPO_URL = "https://github.com/jnmartin84/sonic-r"
    }

    private lateinit var binding: ActivityAboutBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityAboutBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.btnUpstreamGithub.setOnClickListener {
            try {
                val intent = Intent(Intent.ACTION_VIEW, Uri.parse(UPSTREAM_REPO_URL))
                startActivity(intent)
            } catch (_: Exception) {
                // Ignore if no web browser is installed on device
            }
        }

        binding.btnBack.setOnClickListener {
            finish()
        }
    }
}
