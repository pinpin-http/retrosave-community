// Ce fichier décrit la structure du build Android indépendant du reste du monorepo.
pluginManagement {
    repositories {
        // AGP et les plugins Android sont publiés dans le dépôt Google.
        google()
        // Kotlin et les bibliothèques JVM sont résolus depuis Maven Central.
        mavenCentral()
        // Le portail Gradle fournit les plugins Gradle qui ne sont pas chez Google.
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    // Interdit aux modules d'ajouter discrètement leurs propres dépôts.
    // Toutes les sources de dépendances restent ainsi auditées à un seul endroit.
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "RetroSaveAndroid"

// Le POC garde volontairement un seul module applicatif, conformément au brief.
include(":app")
