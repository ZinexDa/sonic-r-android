plugins {
    id("com.android.application") version "8.12.3"
    id("org.jetbrains.kotlin.android") version "2.0.21"
}

android {
    namespace = "org.sonicr.android"
    compileSdk = 34
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = "org.sonicr.android"
        minSdk = 21
        targetSdk = 34
        versionCode = 6
        versionName = "1.2.2"

        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                    "-DSDL_SHARED=ON",
                    "-DSDL_STATIC=OFF",
                    "-DSDL_TEST=OFF"
                )
                abiFilters("arm64-v8a", "x86_64", "armeabi-v7a")
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64", "armeabi-v7a")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    buildFeatures {
        viewBinding = true
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isDebuggable = false
            signingConfig = signingConfigs.getByName("debug")
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
        debug {
            isDebuggable = true
            isJniDebuggable = true
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../sonic-r-main/source/android/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            java.srcDirs(
                "src/main/java",
                "../sonic-r-main/source/android/SDL2/android-project/app/src/main/java"
            )
        }
    }
}

kotlin {
    jvmToolchain(21)
}

dependencies {
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
    testImplementation("junit:junit:4.13.2")
}
