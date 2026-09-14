package com.retrosave.data.api

import kotlinx.serialization.encodeToString
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Test

class DtosTest {
    @Test
    fun `device payload uses frozen snake case keys`() {
        val encoded =
            WireJson.encodeToString(
                DeviceCreateDto(
                    name = "Thor",
                    os = "android",
                    appVersion = "0.1.0",
                ),
            )

        assertEquals(
            """{"name":"Thor","os":"android","app_version":"0.1.0"}""",
            encoded,
        )
    }

    @Test
    fun `unit response maps the exact remote identity`() {
        val response =
            WireJson.decodeFromString<UnitsResponseDto>(
                """
                {
                  "units": [{
                    "id": "unit-1",
                    "emulator": "azahar",
                    "unit_key": "00112233",
                    "unit_type": "dir",
                    "game_key": "3ds:00112233",
                    "game_label": "00112233",
                    "head_version": 4,
                    "state": "active",
                    "updated_at": "2026-07-31T08:00:00Z",
                    "head": {
                      "number": 4,
                      "content_sha256": "${"a".repeat(64)}",
                      "size_bytes": 42,
                      "created_at": "2026-07-31T08:00:00Z",
                      "origin_device_name": "Thor"
                    }
                  }]
                }
                """.trimIndent(),
            )

        val unit = response.units.single().toRemote()
        assertEquals("unit-1", unit.id)
        assertEquals("00112233", unit.unitKey)
        assertEquals(4, unit.headVersion)
        assertEquals(42L, unit.head?.sizeBytes)
    }

    @Test
    fun `prepare duplicate does not invent an upload target`() {
        val response =
            WireJson.decodeFromString<PrepareResponseDto>(
                """{"duplicate":true,"version":7}""",
            )

        assertEquals(true, response.duplicate)
        assertEquals(7, response.version)
        assertNull(response.upload)
    }

    @Test
    fun `nullable confirm timestamp is explicit on the wire`() {
        val payload =
            VersionConfirmDto(
                objectKey = "u/user/unit/hash.tar.zst",
                contentSha256 = "a".repeat(64),
                archiveSha256 = "b".repeat(64),
                baseVersion = 2,
                env = emptyMap(),
                clientMtime = null,
            )

        val encoded = WireJson.encodeToString(payload)
        assertFalse(encoded.contains("objectKey"))
        assertFalse(encoded.contains("clientMtime"))
        assertEquals(
            true,
            encoded.contains(""""client_mtime":null"""),
        )
    }
}
