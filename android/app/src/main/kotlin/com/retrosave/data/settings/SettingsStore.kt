package com.retrosave.data.settings

import android.content.Context
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map

private val Context.retroSaveSettings by preferencesDataStore(name = "retrosave_settings")

data class SettingsSnapshot(
    val serverUrl: String = "",
    val token: String = "",
    val deviceId: String? = null,
    val deviceName: String = "",
    // M7 §5 : curseur de lecture des événements d'usage. Persisté pour que deux
    // passes ne relisent pas la même fenêtre et ne comptent pas deux fois la
    // même partie ; plafonné à 24 h à la première lecture.
    val lastUsageQueryMs: Long = 0L,
    // M7 §5 : état de session **persisté**, et pas seulement gardé en mémoire.
    // Un `SyncWorker` est un objet neuf à chaque réveil et le process peut mourir
    // entre deux cycles ; sans persistance on repartirait de zéro précisément
    // quand la partie a été longue — le cas que la détection existe pour couvrir.
    val runningSessions: Set<String> = emptySet(),
    // AD-36 (Q32) : « à jour » doit vouloir dire « vérifié à l'instant », jamais
    // « je n'ai rien à envoyer ». Sans ces dates, une synchronisation morte
    // depuis trois jours affiche exactement la même chose qu'une saine.
    val lastSuccessAtMs: Long = 0L,
    val lastSuccessTrigger: String = "",
    // Suivie à part : c'est la passe à l'ouverture qui masque la panne, en
    // remettant tout à jour au moment précis où l'utilisateur regarde.
    val lastPeriodicSuccessAtMs: Long = 0L,
    // CLD-01 : la session d'un compte hébergé. Vide en auto-hébergement, où
    // l'authentification reste le jeton d'invitation.
    val accessToken: String = "",
    val refreshToken: String = "",
    val accessExpiresAtMs: Long = 0L,
    val accountEmail: String = "",
) {
    /** Un compte hébergé est relié : plus d'adresse ni de jeton à saisir. */
    val hasCloudAccount: Boolean
        get() = accessToken.isNotBlank() && refreshToken.isNotBlank()

    val isConnected: Boolean
        get() = serverUrl.isNotBlank() && (token.isNotBlank() || hasCloudAccount) && deviceId != null
}

class SettingsStore(
    context: Context,
) {
    private val dataStore = context.retroSaveSettings

    val values: Flow<SettingsSnapshot> =
        dataStore.data.map { preferences ->
            SettingsSnapshot(
                serverUrl = preferences[Keys.serverUrl].orEmpty(),
                token = preferences[Keys.token].orEmpty(),
                deviceId = preferences[Keys.deviceId],
                deviceName = preferences[Keys.deviceName].orEmpty(),
                lastUsageQueryMs = preferences[Keys.lastUsageQueryMs] ?: 0L,
                runningSessions = preferences[Keys.runningSessions].orEmpty(),
                lastSuccessAtMs = preferences[Keys.lastSuccessAtMs] ?: 0L,
                lastSuccessTrigger = preferences[Keys.lastSuccessTrigger].orEmpty(),
                lastPeriodicSuccessAtMs = preferences[Keys.lastPeriodicSuccessAtMs] ?: 0L,
                accessToken = preferences[Keys.accessToken].orEmpty(),
                refreshToken = preferences[Keys.refreshToken].orEmpty(),
                accessExpiresAtMs = preferences[Keys.accessExpiresAtMs] ?: 0L,
                accountEmail = preferences[Keys.accountEmail].orEmpty(),
            )
        }

    suspend fun snapshot(): SettingsSnapshot = values.first()

    suspend fun saveConnection(
        serverUrl: String,
        token: String,
        deviceName: String,
    ) {
        dataStore.edit {
            it[Keys.serverUrl] = serverUrl.trimEnd('/')
            it[Keys.token] = token
            it[Keys.deviceName] = deviceName
            it.remove(Keys.deviceId)
        }
    }

    /**
     * Relier un compte hébergé (CLD-01).
     *
     * L'adresse du serveur est posée par l'application, pas par l'utilisateur :
     * c'est tout l'intérêt du parcours hébergé. Le jeton d'invitation est
     * effacé au passage — deux authentifications concurrentes pour le même
     * appareil ne mèneraient qu'à des surprises.
     */
    suspend fun saveCloudAccount(
        serverUrl: String,
        accessToken: String,
        refreshToken: String,
        expiresAtMs: Long,
        email: String,
        deviceName: String,
    ) {
        dataStore.edit {
            it[Keys.serverUrl] = serverUrl.trimEnd('/')
            it[Keys.accessToken] = accessToken
            it[Keys.refreshToken] = refreshToken
            it[Keys.accessExpiresAtMs] = expiresAtMs
            it[Keys.accountEmail] = email
            it[Keys.deviceName] = deviceName
            it.remove(Keys.token)
            it.remove(Keys.deviceId)
        }
    }

    /**
     * Ranger une session renouvelée, sans toucher au reste.
     *
     * Écriture distincte de `saveCloudAccount` : un renouvellement ne doit PAS
     * effacer l'identifiant d'appareil, sinon chaque expiration de jeton
     * réenregistrerait un appareil de plus sur le compte.
     */
    suspend fun saveCloudSession(
        accessToken: String,
        refreshToken: String,
        expiresAtMs: Long,
    ) {
        dataStore.edit {
            it[Keys.accessToken] = accessToken
            it[Keys.refreshToken] = refreshToken
            it[Keys.accessExpiresAtMs] = expiresAtMs
        }
    }

    suspend fun saveRegisteredDevice(deviceId: String) {
        dataStore.edit {
            it[Keys.deviceId] = deviceId
        }
    }

    /**
     * Curseur et état de session avancent **ensemble**, dans la même écriture.
     *
     * Les séparer ouvrirait la fenêtre où le curseur a bougé mais pas l'état :
     * la partie serait alors comptée comme jamais commencée et jamais finie.
     */
    suspend fun saveSessionState(
        nowMs: Long,
        running: Set<String>,
    ) {
        dataStore.edit {
            it[Keys.lastUsageQueryMs] = nowMs
            it[Keys.runningSessions] = running
        }
    }

    /** AD-36 : dater une passe réussie, et retenir laquelle. */
    suspend fun recordSuccessfulPass(
        nowMs: Long,
        trigger: String,
    ) {
        dataStore.edit {
            it[Keys.lastSuccessAtMs] = nowMs
            it[Keys.lastSuccessTrigger] = trigger
            if (trigger == "PERIODIC") {
                it[Keys.lastPeriodicSuccessAtMs] = nowMs
            }
        }
    }

    suspend fun clearConnection() {
        dataStore.edit {
            it.remove(Keys.serverUrl)
            it.remove(Keys.token)
            it.remove(Keys.deviceId)
            it.remove(Keys.deviceName)
            it.remove(Keys.accessToken)
            it.remove(Keys.refreshToken)
            it.remove(Keys.accessExpiresAtMs)
            it.remove(Keys.accountEmail)
        }
    }

    private object Keys {
        val lastUsageQueryMs: Preferences.Key<Long> = longPreferencesKey("last_usage_query_ms")
        val runningSessions: Preferences.Key<Set<String>> =
            stringSetPreferencesKey("running_sessions")
        val lastSuccessAtMs: Preferences.Key<Long> = longPreferencesKey("last_success_at_ms")
        val lastSuccessTrigger: Preferences.Key<String> =
            stringPreferencesKey("last_success_trigger")
        val lastPeriodicSuccessAtMs: Preferences.Key<Long> =
            longPreferencesKey("last_periodic_success_at_ms")
        val serverUrl: Preferences.Key<String> = stringPreferencesKey("server_url")
        val token: Preferences.Key<String> = stringPreferencesKey("token")
        val deviceId: Preferences.Key<String> = stringPreferencesKey("device_id")
        val deviceName: Preferences.Key<String> = stringPreferencesKey("device_name")
        val accessToken: Preferences.Key<String> = stringPreferencesKey("account_access_token")
        val refreshToken: Preferences.Key<String> = stringPreferencesKey("account_refresh_token")
        val accessExpiresAtMs: Preferences.Key<Long> =
            longPreferencesKey("account_access_expires_at_ms")
        val accountEmail: Preferences.Key<String> = stringPreferencesKey("account_email")
    }
}
