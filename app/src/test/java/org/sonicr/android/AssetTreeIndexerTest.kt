package org.sonicr.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AssetTreeIndexerTest {

    @Test
    fun testNormalizeRelativePath_handlesCasingAndSlashes() {
        assertEquals("GENERAL/SONICR.BIT", AssetInstaller.normalizeRelativePath("general/sonicr.bit"))
        assertEquals("BIN/OBJECTS/SONIC/SONIC_H.BIN", AssetInstaller.normalizeRelativePath("BIN\\OBJECTS\\Sonic\\sonic_h.bin"))
        assertEquals("ISLAND/ISLAND_E.BIN", AssetInstaller.normalizeRelativePath("/island/./island_e.bin/"))
        assertEquals("MULTIPLE/SLASHES/AND/MIXED/CASE.RAW", AssetInstaller.normalizeRelativePath("multiple///slashes\\\\and\\mixed/Case.RAW"))
    }

    @Test
    fun testFindGameRootPrefix_identifiesRootAndNestedPrefix() {
        // Direct root
        val directSet = setOf(
            "GENERAL/SONICR.BIT",
            "GENERAL/SONICR.RAW",
            "ISLAND/ISLAND_E.BIN"
        )
        assertEquals("", AssetInstaller.findGameRootPrefix(directSet))

        // Nested inside a subfolder
        val nestedSet = setOf(
            "SONICR_PC/GENERAL/SONICR.BIT",
            "SONICR_PC/GENERAL/SONICR.RAW",
            "SONICR_PC/ISLAND/ISLAND_E.BIN"
        )
        assertEquals("SONICR_PC/", AssetInstaller.findGameRootPrefix(nestedSet))

        // Deeply nested
        val deepSet = setOf(
            "GAMES/PC/SONICR/GENERAL/SONICR.BIT",
            "GAMES/PC/SONICR/ISLAND/ISLAND_E.BIN"
        )
        assertEquals("GAMES/PC/SONICR/", AssetInstaller.findGameRootPrefix(deepSet))

        // Missing probe file
        val missingSet = setOf(
            "OTHER_GAME/DATA.BIN",
            "MUSIC/TRACK.MP3"
        )
        assertNull(AssetInstaller.findGameRootPrefix(missingSet))
    }

    @Test
    fun testValidateRequiredFiles_checksAllManifestEntries() {
        val completeSet = setOf(
            "GENERAL/SONICR.BIT",
            "GENERAL/SONICR.RAW",
            "ISLAND/ISLAND_E.BIN",
            "ISLAND/ISLAND01.RAW",
            "BIN/OBJECTS/SONIC/SONIC_H.BIN",
            "BIN/OBJECTS/SONIC/SONIC_H.GRD",
            "AI/AISTUFFI.BIN",
            "EXTRA/UNNEEDED.FILE"
        )
        val missingWhenComplete = AssetInstaller.validateRequiredFiles(completeSet)
        assertTrue("Expected 0 missing files for complete set, got: $missingWhenComplete", missingWhenComplete.isEmpty())

        val partialSet = setOf(
            "GENERAL/SONICR.BIT",
            "GENERAL/SONICR.RAW"
        )
        val missingWhenPartial = AssetInstaller.validateRequiredFiles(partialSet)
        assertEquals(5, missingWhenPartial.size)
        assertTrue(missingWhenPartial.contains("ISLAND/ISLAND_E.BIN"))
        assertTrue(missingWhenPartial.contains("AI/AISTUFFI.BIN"))

        val emptySet = emptySet<String>()
        val missingWhenEmpty = AssetInstaller.validateRequiredFiles(emptySet)
        assertEquals(AssetInstaller.REQUIRED_CORE_FILES.size, missingWhenEmpty.size)
    }
}
