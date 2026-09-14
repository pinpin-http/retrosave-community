package com.retrosave.ui.onboarding

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.retrosave.R
import com.retrosave.data.settings.SettingsSnapshot
import com.retrosave.ui.theme.CardBody
import com.retrosave.ui.theme.PaperCard
import com.retrosave.ui.theme.PillButton
import com.retrosave.ui.theme.RetroField
import com.retrosave.ui.theme.RetroSaveColors
import com.retrosave.ui.theme.Reveal
import com.retrosave.ui.theme.ScreenTitle
import kotlinx.coroutines.launch

@Composable
fun ConnectScreen(
    initial: SettingsSnapshot,
    onConnect: suspend (serverUrl: String, token: String, deviceName: String) -> Result<Unit>,
) {
    val scope = rememberCoroutineScope()
    var serverUrl by remember(initial.serverUrl) { mutableStateOf(initial.serverUrl) }
    var token by remember(initial.token) { mutableStateOf(initial.token) }
    var deviceName by remember(initial.deviceName) { mutableStateOf(initial.deviceName) }
    var connecting by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    val genericError = stringResource(R.string.error_connect_generic)
    val invalid =
        serverUrl.isBlank() ||
            token.isBlank() ||
            deviceName.isBlank() ||
            !(serverUrl.startsWith("http://") || serverUrl.startsWith("https://"))

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Reveal(index = 0) {
                ScreenTitle(
                    bold = stringResource(R.string.connect_title),
                )
            }
        }
        item {
            Reveal(index = 1) {
                PaperCard {
                    RetroField(
                        value = serverUrl,
                        onValueChange = { serverUrl = it },
                        label = stringResource(R.string.connect_server_url),
                        enabled = !connecting,
                    )
                    RetroField(
                        value = token,
                        onValueChange = { token = it },
                        label = stringResource(R.string.connect_token),
                        enabled = !connecting,
                        // Un écran partagé ne doit pas livrer le jeton.
                        visualTransformation = PasswordVisualTransformation(),
                    )
                    RetroField(
                        value = deviceName,
                        onValueChange = { deviceName = it },
                        label = stringResource(R.string.connect_device_name),
                        enabled = !connecting,
                    )
                    error?.let { message ->
                        CardBody(text = message, tone = RetroSaveColors.error)
                    }
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        if (connecting) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(20.dp),
                                color = RetroSaveColors.secondary,
                                strokeWidth = 2.dp,
                            )
                        }
                        PillButton(
                            text =
                                stringResource(
                                    if (connecting) {
                                        R.string.connect_in_progress
                                    } else {
                                        R.string.connect_action
                                    },
                                ),
                            onClick = {
                                connecting = true
                                error = null
                                scope.launch {
                                    val outcome =
                                        onConnect(
                                            serverUrl.trim(),
                                            token.trim(),
                                            deviceName.trim(),
                                        )
                                    outcome.onFailure { error = it.message ?: genericError }
                                    connecting = false
                                }
                            },
                            modifier = Modifier.weight(1f),
                            enabled = !connecting && !invalid,
                            haptic = true,
                        )
                    }
                }
            }
        }
    }
}
