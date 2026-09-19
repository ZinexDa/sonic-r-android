package org.sonicr.android

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.View
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.sonicr.android.databinding.ActivityLauncherBinding

class LauncherActivity : Activity() {

    companion object {
        private const val REQUEST_CODE_PICK_FOLDER = 1001
    }

    private lateinit var binding: ActivityLauncherBinding

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityLauncherBinding.inflate(layoutInflater)
        setContentView(binding.root)

        // Runtime verification: Initialize Rust netplay sidecar crypto & logging
        val initRes = NetplayBridge.init()
        android.util.Log.i("LauncherActivity", "NetplayBridge initialized (res=$initRes, loaded=${NetplayBridge.isLibraryLoaded()})")

        binding.btnPlay.setOnClickListener {
            if (AssetInstaller.isAssetsInstalled(this)) {
                launchGame()
            } else {
                showInstallInstructionsDialog()
            }
        }

        binding.btnReinstall.setOnClickListener {
            showInstallInstructionsDialog()
        }

        binding.btnSettings.setOnClickListener {
            startActivity(Intent(this, SettingsActivity::class.java))
        }

        binding.btnMultiplayer.setOnClickListener {
            startActivity(Intent(this, MultiplayerActivity::class.java))
        }

        binding.btnMods.setOnClickListener {
            startActivity(Intent(this, ModsActivity::class.java))
        }
    }

    override fun onResume() {
        super.onResume()
        updateAssetStatusUI()
    }

    private fun updateAssetStatusUI() {
        val installed = AssetInstaller.isAssetsInstalled(this)
        if (installed) {
            AssetInstaller.ensureDirectoriesExist(this)
            binding.assetStatusBadge.setText(R.string.asset_status_ready)
            binding.assetStatusBadge.setTextColor(Color.parseColor("#4ADE80"))
            binding.btnPlay.setText(R.string.btn_play)
            binding.btnReinstall.visibility = View.VISIBLE
        } else {
            binding.assetStatusBadge.setText(R.string.asset_status_missing)
            binding.assetStatusBadge.setTextColor(Color.parseColor("#FBBF24"))
            binding.btnPlay.setText(R.string.btn_install_assets)
            binding.btnReinstall.visibility = View.GONE
        }
    }

    private fun showInstallInstructionsDialog() {
        AlertDialog.Builder(this)
            .setTitle(R.string.dialog_install_title)
            .setMessage(R.string.dialog_install_desc)
            .setPositiveButton(R.string.dialog_install_btn_pick) { _, _ ->
                launchFolderPicker()
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    private fun launchFolderPicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(
                Intent.FLAG_GRANT_READ_URI_PERMISSION or
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
            )
        }
        startActivityForResult(intent, REQUEST_CODE_PICK_FOLDER)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_CODE_PICK_FOLDER) {
            if (resultCode == RESULT_OK && data?.data != null) {
                startAssetInstallation(data.data!!)
            }
        }
    }

    private fun startAssetInstallation(treeUri: Uri) {
        binding.menuLayout.visibility = View.GONE
        binding.progressCard.visibility = View.VISIBLE
        binding.progressTitle.setText(R.string.status_scanning)
        binding.progressBar.progress = 0
        binding.progressPercent.text = "0%"
        binding.progressFile.text = ""

        CoroutineScope(Dispatchers.Main).launch {
            val result = AssetInstaller.installFromTreeUri(this@LauncherActivity, treeUri) { progress ->
                binding.progressTitle.text = getString(
                    R.string.status_copying,
                    progress.filesCopied,
                    progress.totalFiles
                )
                binding.progressBar.progress = progress.percent
                binding.progressPercent.text = "${progress.percent}%"
                binding.progressFile.text = progress.currentFile
            }

            when (result) {
                is AssetInstaller.InstallResult.Success -> {
                    binding.progressTitle.setText(R.string.status_complete)
                    binding.progressBar.progress = 100
                    binding.progressPercent.text = "100%"
                    delay(1000)
                    binding.progressCard.visibility = View.GONE
                    binding.menuLayout.visibility = View.VISIBLE
                    updateAssetStatusUI()
                    launchGame()
                }
                is AssetInstaller.InstallResult.MissingFiles -> {
                    binding.progressCard.visibility = View.GONE
                    binding.menuLayout.visibility = View.VISIBLE
                    updateAssetStatusUI()
                    val missingFormatted = result.missing.joinToString("\n• ", prefix = "• ")
                    AlertDialog.Builder(this@LauncherActivity)
                        .setTitle(R.string.error_missing_files_title)
                        .setMessage(getString(R.string.error_missing_files_desc, missingFormatted))
                        .setPositiveButton(R.string.btn_ok, null)
                        .show()
                }
                is AssetInstaller.InstallResult.Failure -> {
                    binding.progressCard.visibility = View.GONE
                    binding.menuLayout.visibility = View.VISIBLE
                    updateAssetStatusUI()
                    AlertDialog.Builder(this@LauncherActivity)
                        .setTitle(R.string.status_failed)
                        .setMessage(getString(R.string.error_copy_failed, result.errorMessage))
                        .setPositiveButton(R.string.btn_ok, null)
                        .show()
                }
            }
        }
    }

    private fun launchGame() {
        val prefs = getSharedPreferences(SettingsActivity.PREFS_NAME, Context.MODE_PRIVATE)
        val intent = Intent(this, GameActivity::class.java).apply {
            putExtra(SettingsActivity.KEY_MUSIC_VOLUME, prefs.getFloat(SettingsActivity.KEY_MUSIC_VOLUME, SettingsActivity.DEFAULT_MUSIC_VOLUME))
            putExtra(SettingsActivity.KEY_SFX_VOLUME, prefs.getFloat(SettingsActivity.KEY_SFX_VOLUME, SettingsActivity.DEFAULT_SFX_VOLUME))
        }
        startActivity(intent)
    }
}
