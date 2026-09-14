package com.retrosave.core.sync

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * AD-36 (Q32) : une fraîcheur affichée est une fraîcheur vérifiée.
 *
 * Miroir des vecteurs Python `test_freshness.py`. La règle est petite mais
 * porte tout le sens de la décision : c'est elle qui décide si un silence de
 * trois jours reste invisible ou non.
 */
class FreshnessTest {
    private val now = 1_700_000_000_000L

    @Test
    fun `a periodic pass that never ran is stale`() {
        assertTrue(periodicSyncIsStale(0L, now))
    }

    @Test
    fun `a recent periodic pass is fresh`() {
        assertFalse(periodicSyncIsStale(now - 60_000, now))
    }

    @Test
    fun `a periodic pass older than a day is stale`() {
        assertTrue(periodicSyncIsStale(now - STALE_PERIODIC_MS - 1, now))
        assertFalse(periodicSyncIsStale(now - STALE_PERIODIC_MS + 1, now))
    }

    @Test
    fun `a device never seen is quiet`() {
        assertTrue(deviceIsQuiet(null, now))
    }

    @Test
    fun `a device offline for a day is not alarming`() {
        // Un appareil éteint une nuit, ou un week-end, est normal.
        assertFalse(deviceIsQuiet(now - STALE_DEVICE_MS + 1, now))
        assertTrue(deviceIsQuiet(now - STALE_DEVICE_MS - 1, now))
    }

    @Test
    fun `the thresholds are pinned because they are product decisions`() {
        assertTrue(STALE_PERIODIC_MS == 24L * 60 * 60 * 1000)
        assertTrue(STALE_DEVICE_MS == 48L * 60 * 60 * 1000)
    }
}
