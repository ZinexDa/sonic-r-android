package org.sonicr.android

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * Unit tests verifying music track candidate path resolution across:
 * 1. Zero-padded (%02d) vs non-padded (%d) track numbers.
 * 2. Directory casing: MUSIC vs music vs Music.
 * 3. Filename prefix casing: TRACK vs Track vs track.
 * 4. Format priority and extension variants.
 */
class MusicTrackResolutionTest {

    @get:Rule
    val tempFolder = TemporaryFolder()

    companion object {
        val DIRS = listOf("MUSIC", "music", "Music")
        val PREFIXES = listOf("TRACK", "Track", "track")
        val SON_EXTS = listOf("SON", "son")
        val ADX_EXTS = listOf("ADX", "adx")
        val ADP_EXTS = listOf("ADP", "adp")
        val MIXER_EXTS = listOf(
            "ogg", "OGG",
            "mp3", "MP3",
            "flac", "FLAC",
            "wav", "WAV"
        )

        /**
         * Candidate generation logic mirroring try_load_candidates in music_sdl.c.
         */
        fun generateCandidates(baseDir: File, trackNum: Int, exts: List<String>): List<File> {
            val numStrs = if (trackNum < 10) {
                listOf(trackNum.toString(), String.format("%02d", trackNum))
            } else {
                listOf(trackNum.toString())
            }

            val candidates = mutableListOf<File>()
            for (ext in exts) {
                for (dir in DIRS) {
                    for (prefix in PREFIXES) {
                        for (numStr in numStrs) {
                            candidates.add(File(baseDir, "$dir/$prefix$numStr.$ext"))
                        }
                    }
                }
            }
            return candidates
        }

        fun resolveFirstExisting(baseDir: File, trackNum: Int, exts: List<String>): File? {
            val candidates = generateCandidates(baseDir, trackNum, exts)
            return candidates.firstOrNull { it.exists() }
        }

        /**
         * Legacy candidate logic from previous music_sdl.c before the bugfix.
         */
        fun legacyResolveFallback(baseDir: File, trackNum: Int, exts: List<String>): File? {
            for (ext in exts) {
                val p1 = File(baseDir, "MUSIC/TRACK$trackNum.$ext")
                if (p1.exists()) return p1
                val p2 = File(baseDir, "MUSIC/track$trackNum.$ext")
                if (p2.exists()) return p2
            }
            return null
        }
    }

    @Test
    fun testCandidateGeneration_includesZeroPaddingForSingleDigits() {
        val root = tempFolder.root
        val candidates = generateCandidates(root, 2, listOf("ogg"))
        val candidatePaths = candidates.map { it.path.replace('\\', '/') }

        // Must include non-padded
        assertTrue(candidatePaths.any { it.endsWith("MUSIC/TRACK2.ogg") })
        assertTrue(candidatePaths.any { it.endsWith("music/track2.ogg") })

        // Must include zero-padded
        assertTrue(candidatePaths.any { it.endsWith("MUSIC/TRACK02.ogg") })
        assertTrue(candidatePaths.any { it.endsWith("music/track02.ogg") })
        assertTrue(candidatePaths.any { it.endsWith("MUSIC/Track02.ogg") })
        assertTrue(candidatePaths.any { it.endsWith("music/Track02.ogg") })
    }

    @Test
    fun testCandidateGeneration_noRedundantDuplicatesForDoubleDigits() {
        val root = tempFolder.root
        val candidates = generateCandidates(root, 10, listOf("ogg"))
        // 3 dirs * 3 prefixes * 1 numStr * 1 ext = 9 candidates
        assertEquals(9, candidates.size)
    }

    @Test
    fun testResolution_resolvesAllReportedBrokenVariants() {
        val root = tempFolder.root

        // Create test directories
        File(root, "music").mkdirs()
        File(root, "MUSIC").mkdirs()

        // 1. Zero-padded lowercase in music/: track02.ogg
        val file1 = File(root, "music/track02.ogg").apply { writeText("dummy") }
        val resolved1 = resolveFirstExisting(root, 2, MIXER_EXTS)
        assertNotNull("Should resolve music/track02.ogg", resolved1)
        assertEquals(file1.canonicalPath, resolved1?.canonicalPath)
        // Verify legacy code fails on this file
        assertNull("Legacy logic should NOT find zero-padded in music/", legacyResolveFallback(root, 2, MIXER_EXTS))

        // 2. Zero-padded TitleCase in MUSIC/: Track03.mp3
        val file2 = File(root, "MUSIC/Track03.mp3").apply { writeText("dummy") }
        val resolved2 = resolveFirstExisting(root, 3, MIXER_EXTS)
        assertNotNull("Should resolve MUSIC/Track03.mp3", resolved2)
        assertEquals(file2.canonicalPath, resolved2?.canonicalPath)
        assertNull("Legacy logic should NOT find Track03.mp3", legacyResolveFallback(root, 3, MIXER_EXTS))

        // 3. Lowercase directory with uppercase prefix: music/TRACK4.wav
        val file3 = File(root, "music/TRACK4.wav").apply { writeText("dummy") }
        val resolved3 = resolveFirstExisting(root, 4, MIXER_EXTS)
        assertNotNull("Should resolve music/TRACK4.wav", resolved3)
        // On case-sensitive filesystems (Linux/Android), legacy logic would fail to find music/ because it only checked MUSIC/
        val isCaseSensitiveFs = File(root, "music").canonicalPath != File(root, "MUSIC").canonicalPath
        if (isCaseSensitiveFs) {
            assertNull("Legacy logic should NOT find fallback format in music/ on case-sensitive filesystem", legacyResolveFallback(root, 4, MIXER_EXTS))
        }

        // 4. Non-padded TitleCase in MUSIC/: Track5.flac
        val file4 = File(root, "MUSIC/Track5.flac").apply { writeText("dummy") }
        val resolved4 = resolveFirstExisting(root, 5, MIXER_EXTS)
        assertNotNull("Should resolve MUSIC/Track5.flac", resolved4)
        assertEquals(file4.canonicalPath, resolved4?.canonicalPath)

        // 5. Zero-padded TitleCase .son: music/Track06.son
        val file5 = File(root, "music/Track06.son").apply { writeText("dummy") }
        val resolved5 = resolveFirstExisting(root, 6, SON_EXTS)
        assertNotNull("Should resolve music/Track06.son", resolved5)
        assertEquals(file5.canonicalPath, resolved5?.canonicalPath)

        // 6. Zero-padded lowercase .adx: MUSIC/track07.adx
        val file6 = File(root, "MUSIC/track07.adx").apply { writeText("dummy") }
        val resolved6 = resolveFirstExisting(root, 7, ADX_EXTS)
        assertNotNull("Should resolve MUSIC/track07.adx", resolved6)
        assertEquals(file6.canonicalPath, resolved6?.canonicalPath)

        // 7. Zero-padded uppercase .adp: music/TRACK08.adp
        val file7 = File(root, "music/TRACK08.adp").apply { writeText("dummy") }
        val resolved7 = resolveFirstExisting(root, 8, ADP_EXTS)
        assertNotNull("Should resolve music/TRACK08.adp", resolved7)
        assertEquals(file7.canonicalPath, resolved7?.canonicalPath)
    }
}
