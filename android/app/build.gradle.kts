import groovy.json.JsonOutput
import org.gradle.api.DefaultTask
import org.gradle.api.file.ConfigurableFileCollection
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.tasks.CacheableTask
import org.gradle.api.tasks.InputFiles
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.PathSensitive
import org.gradle.api.tasks.PathSensitivity
import org.gradle.api.tasks.TaskAction
import java.util.zip.ZipFile

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
    alias(libs.plugins.ksp)
    alias(libs.plugins.ktlint)
}

@CacheableTask
abstract class GenerateAdapterAssetsTask : DefaultTask() {
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val adapterFiles: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun generate() {
        val destination =
            outputDirectory.get().dir("adapters").asFile.also {
                it.mkdirs()
            }
        adapterFiles.files.sortedBy { it.name }.forEach { source ->
            val parsed = parseAdapterToml(source.readText(Charsets.UTF_8))
            destination
                .resolve("${source.nameWithoutExtension}.json")
                .writeText(
                    JsonOutput.prettyPrint(JsonOutput.toJson(parsed)),
                    Charsets.UTF_8,
                )
        }
    }

    private fun parseAdapterToml(content: String): Map<String, Any> {
        val root = linkedMapOf<String, Any>()
        var section: MutableMap<String, Any> = root
        content.lineSequence().forEach { rawLine ->
            val line = stripTomlComment(rawLine).trim()
            if (line.isEmpty()) return@forEach
            if (line.startsWith("[") && line.endsWith("]")) {
                section = root
                line.removeSurrounding("[", "]").split('.').forEach { name ->
                    @Suppress("UNCHECKED_CAST")
                    section =
                        section.getOrPut(name) {
                            linkedMapOf<String, Any>()
                        } as MutableMap<String, Any>
                }
            } else {
                val separator = line.indexOf('=')
                require(separator > 0) { "Invalid adapter TOML line: $rawLine" }
                section[line.substring(0, separator).trim()] =
                    parseTomlValue(line.substring(separator + 1))
            }
        }
        return root
    }

    private fun stripTomlComment(line: String): String {
        var quoted = false
        var escaped = false
        line.forEachIndexed { index, character ->
            when {
                escaped -> escaped = false
                character == '\\' && quoted -> escaped = true
                character == '"' -> quoted = !quoted
                character == '#' && !quoted -> return line.substring(0, index)
            }
        }
        return line
    }

    private fun parseTomlValue(value: String): Any {
        val trimmed = value.trim()
        return when {
            trimmed.startsWith("[") ->
                Regex(""""(?:\\.|[^"])*"""")
                    .findAll(trimmed)
                    .map { parseTomlString(it.value) }
                    .toList()
            trimmed.startsWith("\"") -> parseTomlString(trimmed)
            else -> trimmed.toInt()
        }
    }

    private fun parseTomlString(value: String): String =
        value
            .removePrefix("\"")
            .removeSuffix("\"")
            .replace("\\\"", "\"")
            .replace("\\\\", "\\")
}

val generatedAdapterAssets = layout.buildDirectory.dir("generated/adapterAssets")
val generatedLegalAssets = layout.buildDirectory.dir("generated/legalAssets")
val generateAdapterAssets =
    tasks.register<GenerateAdapterAssetsTask>("generateAdapterAssets") {
        adapterFiles.from(
            fileTree(rootProject.projectDir.parentFile.resolve("adapters")) {
                include("*.toml")
            },
        )
        outputDirectory.set(generatedAdapterAssets)
    }

val generateLegalAssets =
    tasks.register<Copy>("generateLegalAssets") {
        from(rootProject.file("../LICENSE"), rootProject.file("../NOTICE.md"))
        into(generatedLegalAssets.map { it.dir("legal") })
    }

android {
    namespace = "com.retrosave"

    // Le brief verrouille la compilation et la cible sur l'API 35.
    compileSdk = 35

    defaultConfig {
        applicationId = "cloud.retrosave.community"

        // Android 10 minimum, Android 15 cible, conformément au brief.
        minSdk = 29
        targetSdk = 35

        versionCode = 1
        // VERSION est partagé avec le serveur et le desktop.
        versionName =
            rootProject
                .file("../VERSION")
                .takeIf { it.isFile }
                ?.readText()
                ?.trim()
                ?: "inconnue"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            // Conserver des traces lisibles pendant l'alpha.
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        // AGP s'exécute avec le JDK 21 d'Android Studio, mais le bytecode de
        // l'application reste en Java 17 pour une compatibilité Android stable.
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        // Aucun layout XML applicatif : l'interface est entièrement en Compose.
        compose = true
    }

    packaging {
        resources {
            // Évite les doublons de licences embarqués par plusieurs dépendances.
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }

    sourceSets {
        getByName("main").assets.srcDir(generatedAdapterAssets)
        getByName("main").assets.srcDir(generatedLegalAssets)
        getByName("test").resources.srcDirs(
            "../../tests/vectors",
            generatedAdapterAssets,
        )
    }
}

tasks.named("preBuild").configure {
    dependsOn(generateAdapterAssets)
    dependsOn(generateLegalAssets)
}

// Garde de régression : reproduit sans appareil le crash M4.5b
// (UnsatisfiedLinkError « libzstd-jni… not found ») en refusant un APK
// dépourvu du binaire natif zstd pour l'ABI du Thor.
val assertZstdNativeLibrary =
    tasks.register("assertZstdNativeLibrary") {
        val apkDirectory = layout.buildDirectory.dir("outputs/apk/debug")
        doLast {
            val apk =
                apkDirectory
                    .get()
                    .asFile
                    .listFiles()
                    ?.firstOrNull { it.extension == "apk" }
                    ?: error("APK debug introuvable pour la vérification zstd.")
            val entries =
                ZipFile(apk).use { zip ->
                    zip.entries().toList().map { it.name }
                }
            val missing =
                listOf("arm64-v8a").filterNot { abi ->
                    entries.any { it.startsWith("lib/$abi/") && it.contains("zstd") }
                }
            check(missing.isEmpty()) {
                "zstd-jni natif absent de l'APK pour $missing : utilisez la variante @aar."
            }
        }
    }

tasks.matching { it.name == "assembleDebug" }.configureEach {
    finalizedBy(assertZstdNativeLibrary)
}

kotlin {
    compilerOptions {
        // Aligne le bytecode Kotlin sur le bytecode Java configuré plus haut.
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(platform(libs.compose.bom))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.room.runtime)
    implementation(libs.androidx.room.ktx)
    implementation(libs.androidx.work.runtime.ktx)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.bundles.ktor.client)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.commons.compress)
    // zstd-jni : variante @aar obligatoire sur Android — le JAR ne contient que
    // les binaires natifs desktop, d'où UnsatisfiedLinkError au premier
    // createArchive sur appareil. Le JAR reste utilisé par les tests JVM.
    implementation("${libs.zstd.jni.get()}@aar")
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.compose.material3)

    ksp(libs.androidx.room.compiler)

    debugImplementation(libs.compose.ui.tooling)

    testImplementation(libs.junit)
    testImplementation(libs.kotlinx.coroutines.test)
    testImplementation(libs.androidx.room.testing)
    // Les tests JVM tournent sur le poste : ils ont besoin des natifs desktop du JAR.
    testImplementation(libs.zstd.jni)
}

ksp {
    arg("room.schemaLocation", "$projectDir/schemas")
}

ktlint {
    version.set(libs.versions.ktlint.asProvider())
}

// AD-29 — la preuve mécanique du tampon constant.
//
// `StreamingArchiveTest` fait transiter une unité de 200 Mio sous un tas de
// 64 Mio : le test ne peut passer que si aucun chemin ne la matérialise. Il
// tourne à part parce que ce plafond étoufferait les autres tests JVM, et il
// est volontairement hors de `testDebugUnitTest` pour ne pas doubler leur durée.
tasks.register<Test>("constrainedHeapTest") {
    description = "Fait passer 200 Mio d'archive sous -Xmx64m (AD-29)."
    group = "verification"

    val debugTest = tasks.named<Test>("testDebugUnitTest").get()
    testClassesDirs = debugTest.testClassesDirs
    classpath = debugTest.classpath
    dependsOn("testDebugUnitTest")

    maxHeapSize = "64m"
    filter { includeTestsMatching("com.retrosave.core.archive.StreamingArchiveTest") }
}
