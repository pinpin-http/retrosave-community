package com.retrosave.core.adapters.azahar

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.junit.Assert.assertEquals
import org.junit.Test

/** Exécute en JVM les mêmes cas AD-23 que le client Python. */
class AzaharIdentityVectorsTest {
    @Test
    fun `shared double identity vectors`() {
        val resource =
            checkNotNull(
                javaClass.getResourceAsStream("/azahar_identity_double.json"),
            ) {
                "shared Azahar identity vector is missing"
            }
        val cases =
            resource.bufferedReader(Charsets.UTF_8).use { reader ->
                Json.parseToJsonElement(reader.readText()).jsonArray
            }

        cases.forEach { element ->
            val case = element.jsonObject
            val input = case.getValue("input").jsonObject
            val identities =
                input.getValue("identities").jsonArray.map { identityElement ->
                    val identity = identityElement.jsonObject
                    AzaharIdentityOption(
                        identity = identity.getValue("identity").jsonPrimitive.content,
                        latestModified =
                            identity.getValue("latest_modified").jsonPrimitive.long,
                    )
                }
            val activeIdentity =
                input.getValue("active_identity").let { value ->
                    if (value is JsonNull) null else value.jsonPrimitive.content
                }
            val expected = case.getValue("expected").jsonObject
            val result = selectAzaharIdentity(identities, activeIdentity)

            assertEquals(
                case.getValue("name").jsonPrimitive.content,
                expected.getValue("status").jsonPrimitive.content,
                result.status,
            )
            assertEquals(expected.nullableString("active_identity"), result.activeIdentity)
            assertEquals(expected.nullableString("proposed_identity"), result.proposedIdentity)
            assertEquals(
                expected
                    .getValue("ignored_identities")
                    .jsonArray
                    .map { it.jsonPrimitive.content },
                result.ignoredIdentities,
            )
        }
    }

    private fun kotlinx.serialization.json.JsonObject.nullableString(key: String): String? {
        val value = getValue(key)
        return if (value is JsonNull) null else value.jsonPrimitive.content
    }
}
