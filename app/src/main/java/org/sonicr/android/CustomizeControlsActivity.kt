package org.sonicr.android

import android.app.Activity
import android.content.Context
import android.os.Bundle
import android.view.View
import android.widget.SeekBar
import android.widget.Toast
import org.sonicr.android.databinding.ActivityCustomizeControlsBinding

class CustomizeControlsActivity : Activity() {

    private lateinit var binding: ActivityCustomizeControlsBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        binding = ActivityCustomizeControlsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        hideSystemUI()

        val prefs = getSharedPreferences(SettingsActivity.PREFS_NAME, Context.MODE_PRIVATE)
        val hasCustom = prefs.getBoolean(SettingsActivity.KEY_CONTROLS_CUSTOM, false) &&
            prefs.contains(SettingsActivity.KEY_CTRL_DPAD_X)

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

        binding.touchControlsEditView.setLayoutData(
            hasCustom,
            dpX, dpY, dpS,
            dlX, dlY, dlS,
            acX, acY, acS,
            jmX, jmY, jmS,
            drX, drY, drS,
            lkX, lkY, lkS,
            stX, stY, stS
        )

        updateSelectionUI(
            binding.touchControlsEditView.selectedControl,
            binding.touchControlsEditView.getSelectedScale()
        )

        binding.touchControlsEditView.onSelectionChanged = { control, scale ->
            updateSelectionUI(control, scale)
        }

        binding.seekScale.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser) {
                    val scale = 0.70f + (progress / 100f)
                    binding.touchControlsEditView.setSelectedScale(scale)
                    binding.textScalePercent.text = "${(scale * 100).toInt()}%"
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        binding.btnScaleMinus.setOnClickListener {
            val cur = binding.seekScale.progress
            binding.seekScale.progress = (cur - 5).coerceAtLeast(0)
            val scale = 0.70f + (binding.seekScale.progress / 100f)
            binding.touchControlsEditView.setSelectedScale(scale)
            binding.textScalePercent.text = "${(scale * 100).toInt()}%"
        }

        binding.btnScalePlus.setOnClickListener {
            val cur = binding.seekScale.progress
            binding.seekScale.progress = (cur + 5).coerceAtMost(70)
            val scale = 0.70f + (binding.seekScale.progress / 100f)
            binding.touchControlsEditView.setSelectedScale(scale)
            binding.textScalePercent.text = "${(scale * 100).toInt()}%"
        }

        binding.btnReset.setOnClickListener {
            binding.touchControlsEditView.resetToDefault()
            Toast.makeText(this, R.string.toast_controls_reset, Toast.LENGTH_SHORT).show()
        }

        binding.btnSave.setOnClickListener {
            val edit = binding.touchControlsEditView
            prefs.edit()
                .putBoolean(SettingsActivity.KEY_CONTROLS_CUSTOM, true)
                .putFloat(SettingsActivity.KEY_CTRL_DPAD_X, edit.dpadX)
                .putFloat(SettingsActivity.KEY_CTRL_DPAD_Y, edit.dpadY)
                .putFloat(SettingsActivity.KEY_CTRL_DPAD_SCALE, edit.dpadScale)
                .putFloat(SettingsActivity.KEY_CTRL_DRIFT_L_X, edit.driftLX)
                .putFloat(SettingsActivity.KEY_CTRL_DRIFT_L_Y, edit.driftLY)
                .putFloat(SettingsActivity.KEY_CTRL_DRIFT_L_SCALE, edit.driftLScale)
                .putFloat(SettingsActivity.KEY_CTRL_ACCEL_X, edit.accelX)
                .putFloat(SettingsActivity.KEY_CTRL_ACCEL_Y, edit.accelY)
                .putFloat(SettingsActivity.KEY_CTRL_ACCEL_SCALE, edit.accelScale)
                .putFloat(SettingsActivity.KEY_CTRL_JUMP_X, edit.jumpX)
                .putFloat(SettingsActivity.KEY_CTRL_JUMP_Y, edit.jumpY)
                .putFloat(SettingsActivity.KEY_CTRL_JUMP_SCALE, edit.jumpScale)
                .putFloat(SettingsActivity.KEY_CTRL_DRIFT_R_X, edit.driftRX)
                .putFloat(SettingsActivity.KEY_CTRL_DRIFT_R_Y, edit.driftRY)
                .putFloat(SettingsActivity.KEY_CTRL_DRIFT_R_SCALE, edit.driftRScale)
                .putFloat(SettingsActivity.KEY_CTRL_LOOK_X, edit.lookX)
                .putFloat(SettingsActivity.KEY_CTRL_LOOK_Y, edit.lookY)
                .putFloat(SettingsActivity.KEY_CTRL_LOOK_SCALE, edit.lookScale)
                .putFloat(SettingsActivity.KEY_CTRL_START_X, edit.startX)
                .putFloat(SettingsActivity.KEY_CTRL_START_Y, edit.startY)
                .putFloat(SettingsActivity.KEY_CTRL_START_SCALE, edit.startScale)
                .apply()

            Toast.makeText(this, R.string.toast_controls_saved, Toast.LENGTH_SHORT).show()
            finish()
        }
    }

    private fun updateSelectionUI(control: TouchControlsEditView.SelectedControl, scale: Float) {
        val label = when (control) {
            TouchControlsEditView.SelectedControl.DPAD -> getString(R.string.label_selected_dpad)
            TouchControlsEditView.SelectedControl.DRIFT_L -> getString(R.string.label_selected_drift_l)
            TouchControlsEditView.SelectedControl.ACCEL -> getString(R.string.label_selected_accel)
            TouchControlsEditView.SelectedControl.JUMP -> getString(R.string.label_selected_jump)
            TouchControlsEditView.SelectedControl.DRIFT_R -> getString(R.string.label_selected_drift_r)
            TouchControlsEditView.SelectedControl.LOOK -> getString(R.string.label_selected_look)
            TouchControlsEditView.SelectedControl.START -> getString(R.string.label_selected_start)
            TouchControlsEditView.SelectedControl.NONE -> "Tap a control to select & resize"
        }
        binding.textSelectedLabel.text = label

        val isEnabled = control != TouchControlsEditView.SelectedControl.NONE
        binding.seekScale.isEnabled = isEnabled
        binding.btnScaleMinus.isEnabled = isEnabled
        binding.btnScalePlus.isEnabled = isEnabled

        val progress = ((scale - 0.70f) * 100).toInt().coerceIn(0, 70)
        binding.seekScale.progress = progress
        binding.textScalePercent.text = "${(scale * 100).toInt()}%"
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) {
            hideSystemUI()
        }
    }

    private fun hideSystemUI() {
        window.decorView.post {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            )
        }
    }
}
