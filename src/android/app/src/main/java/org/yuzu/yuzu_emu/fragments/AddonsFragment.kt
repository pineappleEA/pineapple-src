// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.annotation.SuppressLint
import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.documentfile.provider.DocumentFile
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.findNavController
import androidx.navigation.fragment.navArgs
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.transition.MaterialSharedAxis
import kotlinx.coroutines.launch
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.adapters.AddonAdapter
import org.yuzu.yuzu_emu.databinding.FragmentAddonsBinding
import org.yuzu.yuzu_emu.model.AddonViewModel
import org.yuzu.yuzu_emu.model.HomeViewModel
import org.yuzu.yuzu_emu.utils.AddonUtil
import org.yuzu.yuzu_emu.utils.FileUtil.copyFilesTo
import java.io.File

class AddonsFragment : Fragment() {
    private var _binding: FragmentAddonsBinding? = null
    private val binding get() = _binding!!

    private val homeViewModel: HomeViewModel by activityViewModels()
    private val addonViewModel: AddonViewModel by activityViewModels()

    private val args by navArgs<AddonsFragmentArgs>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        addonViewModel.onOpenAddons(args.game)
        enterTransition = MaterialSharedAxis(MaterialSharedAxis.X, true)
        returnTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
        reenterTransition = MaterialSharedAxis(MaterialSharedAxis.X, false)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentAddonsBinding.inflate(inflater)
        return binding.root
    }

    // This is using the correct scope, lint is just acting up
    @SuppressLint("UnsafeRepeatOnLifecycleDetector")
    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        homeViewModel.setNavigationVisibility(visible = false, animated = false)
        homeViewModel.setStatusBarShadeVisibility(false)

        binding.toolbarAddons.setNavigationOnClickListener {
            binding.root.findNavController().popBackStack()
        }

        binding.toolbarAddons.title = getString(R.string.addons_game, args.game.title)

        binding.listAddons.apply {
            layoutManager = LinearLayoutManager(requireContext())
            adapter = AddonAdapter()
        }

        viewLifecycleOwner.lifecycleScope.apply {
            launch {
                repeatOnLifecycle(Lifecycle.State.STARTED) {
                    addonViewModel.addonList.collect {
                        (binding.listAddons.adapter as AddonAdapter).submitList(it)
                    }
                }
            }
            launch {
                repeatOnLifecycle(Lifecycle.State.STARTED) {
                    addonViewModel.showModInstallPicker.collect {
                        if (it) {
                            installAddon.launch(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).data)
                            addonViewModel.showModInstallPicker(false)
                        }
                    }
                }
            }
            launch {
                repeatOnLifecycle(Lifecycle.State.STARTED) {
                    addonViewModel.showModNoticeDialog.collect {
                        if (it) {
                            MessageDialogFragment.newInstance(
                                requireActivity(),
                                titleId = R.string.addon_notice,
                                descriptionId = R.string.addon_notice_description,
                                positiveAction = { addonViewModel.showModInstallPicker(true) }
                            ).show(parentFragmentManager, MessageDialogFragment.TAG)
                            addonViewModel.showModNoticeDialog(false)
                        }
                    }
                }
            }
        }

        binding.buttonInstall.setOnClickListener {
            ContentTypeSelectionDialogFragment().show(
                parentFragmentManager,
                ContentTypeSelectionDialogFragment.TAG
            )
        }

        setInsets()
    }

    override fun onResume() {
        super.onResume()
        addonViewModel.refreshAddons()
    }

    override fun onDestroy() {
        super.onDestroy()
        addonViewModel.onCloseAddons()
    }

    val installAddon =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { result ->
            if (result == null) {
                return@registerForActivityResult
            }

            val externalAddonDirectory = DocumentFile.fromTreeUri(requireContext(), result)
            if (externalAddonDirectory == null) {
                MessageDialogFragment.newInstance(
                    requireActivity(),
                    titleId = R.string.invalid_directory,
                    descriptionId = R.string.invalid_directory_description
                ).show(parentFragmentManager, MessageDialogFragment.TAG)
                return@registerForActivityResult
            }

            val isValid = externalAddonDirectory.listFiles()
                .any { AddonUtil.validAddonDirectories.contains(it.name) }
            val errorMessage = MessageDialogFragment.newInstance(
                requireActivity(),
                titleId = R.string.invalid_directory,
                descriptionId = R.string.invalid_directory_description
            )
            if (isValid) {
                IndeterminateProgressDialogFragment.newInstance(
                    requireActivity(),
                    R.string.installing_game_content,
                    false
                ) {
                    val parentDirectoryName = externalAddonDirectory.name
                    val internalAddonDirectory =
                        File(args.game.addonDir + parentDirectoryName)
                    try {
                        externalAddonDirectory.copyFilesTo(internalAddonDirectory)
                    } catch (_: Exception) {
                        return@newInstance errorMessage
                    }
                    addonViewModel.refreshAddons()
                    return@newInstance getString(R.string.addon_installed_successfully)
                }.show(parentFragmentManager, IndeterminateProgressDialogFragment.TAG)
            } else {
                errorMessage.show(parentFragmentManager, MessageDialogFragment.TAG)
            }
        }

    private fun setInsets() =
        ViewCompat.setOnApplyWindowInsetsListener(
            binding.root
        ) { _: View, windowInsets: WindowInsetsCompat ->
            val barInsets = windowInsets.getInsets(WindowInsetsCompat.Type.systemBars())
            val cutoutInsets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())

            val leftInsets = barInsets.left + cutoutInsets.left
            val rightInsets = barInsets.right + cutoutInsets.right

            val mlpToolbar = binding.toolbarAddons.layoutParams as ViewGroup.MarginLayoutParams
            mlpToolbar.leftMargin = leftInsets
            mlpToolbar.rightMargin = rightInsets
            binding.toolbarAddons.layoutParams = mlpToolbar

            val mlpAddonsList = binding.listAddons.layoutParams as ViewGroup.MarginLayoutParams
            mlpAddonsList.leftMargin = leftInsets
            mlpAddonsList.rightMargin = rightInsets
            binding.listAddons.layoutParams = mlpAddonsList
            binding.listAddons.updatePadding(
                bottom = barInsets.bottom +
                    resources.getDimensionPixelSize(R.dimen.spacing_bottom_list_fab)
            )

            val fabSpacing = resources.getDimensionPixelSize(R.dimen.spacing_fab)
            val mlpFab =
                binding.buttonInstall.layoutParams as ViewGroup.MarginLayoutParams
            mlpFab.leftMargin = leftInsets + fabSpacing
            mlpFab.rightMargin = rightInsets + fabSpacing
            mlpFab.bottomMargin = barInsets.bottom + fabSpacing
            binding.buttonInstall.layoutParams = mlpFab

            windowInsets
        }
}
