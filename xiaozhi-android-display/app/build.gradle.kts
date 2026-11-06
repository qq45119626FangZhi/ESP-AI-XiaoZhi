plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.xiaozhi.display"

    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.xiaozhi.display"
        minSdk = 26
        targetSdk = 33
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            optimization {
                enable = false
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    // MVVM：启用 ViewBinding，布局通过生成的 *Binding 访问，全项目不用 findViewById
    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.recyclerview)
    // MVVM：ViewModel + LiveData
    implementation("androidx.lifecycle:lifecycle-viewmodel:2.6.1")
    implementation("androidx.lifecycle:lifecycle-livedata:2.6.1")
    // 与离线缓存中已有的 AndroidX 版本对齐，避免拉取不存在的传递依赖
    implementation("androidx.core:core:1.10.1")
    implementation("androidx.annotation:annotation:1.6.0")
    implementation("androidx.customview:customview:1.1.0")
    implementation("androidx.collection:collection:1.1.0")
    implementation("androidx.lifecycle:lifecycle-runtime:2.6.1")
    implementation("androidx.lifecycle:lifecycle-common:2.6.1")
}
