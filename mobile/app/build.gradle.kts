plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "top.ikee002.ikeback"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "top.ikee002.ikeback"
        minSdk = 30
        targetSdk = 36
        versionCode = 13
        versionName = "5.3.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    val signingEnvironment = listOf(
        "ANDROID_KEYSTORE_FILE",
        "ANDROID_KEYSTORE_PASSWORD",
        "ANDROID_KEY_ALIAS",
        "ANDROID_KEY_PASSWORD",
    ).associateWith { System.getenv(it)?.trim().orEmpty() }
    val missingSigningValues = signingEnvironment.filterValues { it.isEmpty() }.keys
    val requireReleaseSigning = System.getenv("IKEBACK_REQUIRE_RELEASE_SIGNING") == "1"
    if (requireReleaseSigning && missingSigningValues.isNotEmpty()) {
        throw GradleException("Missing required Android signing values: ${missingSigningValues.joinToString()}")
    }
    val hasCompleteSigning = missingSigningValues.isEmpty()
    if (!hasCompleteSigning && signingEnvironment.values.any { it.isNotEmpty() }) {
        throw GradleException("Android signing requires ALL of: ${signingEnvironment.keys.joinToString()}")
    }

    signingConfigs {
        // Local non-CI builds may remain unsigned. CI must provide all four
        // values and can never silently publish an unsigned/debug-signed APK.
        if (hasCompleteSigning) {
            create("release") {
                val keystorePath = signingEnvironment.getValue("ANDROID_KEYSTORE_FILE")
                this.storeFile = file(keystorePath)
                storePassword = signingEnvironment.getValue("ANDROID_KEYSTORE_PASSWORD")
                keyAlias = signingEnvironment.getValue("ANDROID_KEY_ALIAS")
                keyPassword = signingEnvironment.getValue("ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            if (hasCompleteSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.activity)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
}