package org.sonicr.android

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.SeekBar
import android.widget.Toast
import org.sonicr.android.databinding.ActivitySettingsBinding

class SettingsActivity : Activity() {

    companion object {
        const val PREFS_NAME = "sonicr_settings"
        const val KEY_MUSIC_VOLUME = "music_volume"
        const val KEY_SFX_VOLUME = "sfx_volume"
        const val DEFAULT_MUSIC_VOLUME = 0.8f
        const val DEFAULT_SFX_VOLUME = 1.0f

        const val KEY_CONTROLS_CUSTOM = "controls_custom"
        // 7 independent control keys
        const val KEY_CTRL_DPAD_X = "ctrl_dpad_x"
        const val KEY_CTRL_DPAD_Y = "ctrl_dpad_y"
        const val KEY_CTRL_DPAD_SCALE = "ctrl_dpad_scale"

        const val KEY_CTRL_DRIFT_L_X = "ctrl_driftL_x"
        const val KEY_CTRL_DRIFT_L_Y = "ctrl_driftL_y"
        const val KEY_CTRL_DRIFT_L_SCALE = "ctrl_driftL_scale"

        const val KEY_CTRL_ACCEL_X = "ctrl_accel_x"
        const val KEY_CTRL_ACCEL_Y = "ctrl_accel_y"
        const val KEY_CTRL_ACCEL_SCALE = "ctrl_accel_scale"

        const val KEY_CTRL_JUMP_X = "ctrl_jump_x"
        const val KEY_CTRL_JUMP_Y = "ctrl_jump_y"
        const val KEY_CTRL_JUMP_SCALE = "ctrl_jump_scale"

        const val KEY_CTRL_DRIFT_R_X = "ctrl_driftR_x"
        const val KEY_CTRL_DRIFT_R_Y = "ctrl_driftR_y"
        const val KEY_CTRL_DRIFT_R_SCALE = "ctrl_driftR_scale"

        const val KEY_CTRL_LOOK_X = "ctrl_look_x"
        const val KEY_CTRL_LOOK_Y = "ctrl_look_y"
        const val KEY_CTRL_LOOK_SCALE = "ctrl_look_scale"

        const val KEY_CTRL_START_X = "ctrl_start_x"
        const val KEY_CTRL_START_Y = "ctrl_start_y"
        const val KEY_CTRL_START_SCALE = "ctrl_start_scale"

        // Legacy cluster keys (cleaned up on reset)
        private const val LEGACY_KEY_DPAD_X = "controls_dpad_x"
        private const val LEGACY_KEY_DPAD_Y = "controls_dpad_y"
        private const val LEGACY_KEY_DPAD_SCALE = "controls_dpad_scale"
        private const val LEGACY_KEY_ACTIONS_X = "controls_actions_x"
        private const val LEGACY_KEY_ACTIONS_Y = "controls_actions_y"
        private const val LEGACY_KEY_ACTIONS_SCALE = "controls_actions_scale"
        private const val LEGACY_KEY_START_X = "controls_start_x"
        private const val LEGACY_KEY_START_Y = "controls_start_y"
        private const val LEGACY_KEY_START_SCALE = "controls_start_scale"
    }

    private lateinit var binding: ActivitySettingsBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

        val musicVolume = prefs.getFloat(KEY_MUSIC_VOLUME, DEFAULT_MUSIC_VOLUME)
        val sfxVolume = prefs.getFloat(KEY_SFX_VOLUME, DEFAULT_SFX_VOLUME)

        if (!prefs.contains(KEY_MUSIC_VOLUME) || !prefs.contains(KEY_SFX_VOLUME)) {
            prefs.edit()
                .putFloat(KEY_MUSIC_VOLUME, musicVolume)
                .putFloat(KEY_SFX_VOLUME, sfxVolume)
                .apply()
        }

        val musicProgress = (musicVolume * 100).toInt().coerceIn(0, 100)
        val sfxProgress = (sfxVolume * 100).toInt().coerceIn(0, 100)

        binding.seekMusicVolume.progress = musicProgress
        binding.musicVolumeText.text = "$musicProgress%"

        binding.seekSfxVolume.progress = sfxProgress
        binding.sfxVolumeText.text = "$sfxProgress%"

        binding.seekMusicVolume.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                binding.musicVolumeText.text = "$progress%"
                prefs.edit().putFloat(KEY_MUSIC_VOLUME, progress / 100f).apply()
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        binding.seekSfxVolume.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                binding.sfxVolumeText.text = "$progress%"
                prefs.edit().putFloat(KEY_SFX_VOLUME, progress / 100f).apply()
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        binding.btnCustomizeControls.setOnClickListener {
            startActivity(Intent(this, CustomizeControlsActivity::class.java))
        }

        binding.btnResetControls.setOnClickListener {
            prefs.edit()
                .putBoolean(KEY_CONTROLS_CUSTOM, false)
                // Remove 7 per-control keys
                .remove(KEY_CTRL_DPAD_X)
                .remove(KEY_CTRL_DPAD_Y)
                .remove(KEY_CTRL_DPAD_SCALE)
                .remove(KEY_CTRL_DRIFT_L_X)
                .remove(KEY_CTRL_DRIFT_L_Y)
                .remove(KEY_CTRL_DRIFT_L_SCALE)
                .remove(KEY_CTRL_ACCEL_X)
                .remove(KEY_CTRL_ACCEL_Y)
                .remove(KEY_CTRL_ACCEL_SCALE)
                .remove(KEY_CTRL_JUMP_X)
                .remove(KEY_CTRL_JUMP_Y)
                .remove(KEY_CTRL_JUMP_SCALE)
                .remove(KEY_CTRL_DRIFT_R_X)
                .remove(KEY_CTRL_DRIFT_R_Y)
                .remove(KEY_CTRL_DRIFT_R_SCALE)
                .remove(KEY_CTRL_LOOK_X)
                .remove(KEY_CTRL_LOOK_Y)
                .remove(KEY_CTRL_LOOK_SCALE)
                .remove(KEY_CTRL_START_X)
                .remove(KEY_CTRL_START_Y)
                .remove(KEY_CTRL_START_SCALE)
                // Clean up legacy cluster keys
                .remove(LEGACY_KEY_DPAD_X)
                .remove(LEGACY_KEY_DPAD_Y)
                .remove(LEGACY_KEY_DPAD_SCALE)
                .remove(LEGACY_KEY_ACTIONS_X)
                .remove(LEGACY_KEY_ACTIONS_Y)
                .remove(LEGACY_KEY_ACTIONS_SCALE)
                .remove(LEGACY_KEY_START_X)
                .remove(LEGACY_KEY_START_Y)
                .remove(LEGACY_KEY_START_SCALE)
                .apply()
            Toast.makeText(this, R.string.toast_controls_reset, Toast.LENGTH_SHORT).show()
        }

        binding.btnAbout.setOnClickListener {
            startActivity(Intent(this, AboutActivity::class.java))
        }

        binding.btnBack.setOnClickListener {
            finish()
        }
    }
}
