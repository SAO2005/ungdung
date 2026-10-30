#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#define LOG_TAG "MeostarMod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Dobby Hook APIs (statically linked inside libmeostar.so)
extern "C" {
    int DobbyHook(void *function_address, void *replace_call, void **origin_call);
    void *DobbySymbolResolver(const char *image_name, const char *symbol_name);
    int DobbyCodePatch(void *address, uint8_t *buffer, uint32_t buffer_size);
}

// Global state
static JavaVM *g_vm = nullptr;
static bool g_game_lib_loaded = false;
static std::string g_package_name = "";

// --------------------------------------------------------------------------
// 1. String Obfuscation Helper (Thuật toán giải mã chuỗi XOR tương đương)
// --------------------------------------------------------------------------
static std::string DecryptString(const uint8_t *data, size_t len, uint8_t key) {
    std::string result;
    result.resize(len);
    for (size_t i = 0; i < len; ++i) {
        result[i] = data[i] ^ key;
    }
    return result;
}

// --------------------------------------------------------------------------
// 2. Package Name & Environment Detection
// --------------------------------------------------------------------------
static std::string GetCurrentPackageName() {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string pkg;
    if (cmdline.is_open()) {
        std::getline(cmdline, pkg, '\0');
        cmdline.close();
    }
    return pkg;
}

// --------------------------------------------------------------------------
// 3. JNI Implementations: com.gamesafe.ano.Menu
// --------------------------------------------------------------------------
extern "C" {

// Offset: 0x1d095c
jstring Menu_Icon(JNIEnv *env, jobject thiz) {
    // Trả về icon tĩnh hoặc rỗng
    return env->NewStringUTF("");
}

// Offset: 0x1d0a1c
jstring Menu_IconWebViewData(JNIEnv *env, jobject thiz) {
    std::string logo_path = "/data/user/0/" + g_package_name + "/cache/logo.txt";
    std::ifstream file(logo_path);
    if (file.is_open()) {
        std::stringstream buffer;
        buffer << file.rdbuf();
        return env->NewStringUTF(buffer.str().c_str());
    }
    return env->NewStringUTF("");
}

// Offset: 0x24db84
jboolean Menu_IsGameLibLoaded(JNIEnv *env, jobject thiz) {
    return g_game_lib_loaded ? JNI_TRUE : JNI_FALSE;
}

// Offset: 0x1d3204
void Menu_Init(JNIEnv *env, jobject thiz, jobject context, jobject tv_title, jobject tv_sub) {
    std::string name_path = "/data/user/0/" + g_package_name + "/cache/NameHack.txt";
    std::ifstream file(name_path);
    if (file.is_open()) {
        std::string custom_title;
        std::getline(file, custom_title);
        if (!custom_title.empty()) {
            jclass tv_class = env->GetObjectClass(tv_title);
            jmethodID setText = env->GetMethodID(tv_class, "setText", "(Ljava/lang/CharSequence;)V");
            jstring jtitle = env->NewStringUTF(custom_title.c_str());
            env->CallVoidMethod(tv_title, setText, jtitle);
            env->DeleteLocalRef(jtitle);
            env->DeleteLocalRef(tv_class);
        }
    }
}

// Offset: 0x1d0bac
jobjectArray Menu_SettingsList(JNIEnv *env, jobject thiz) {
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray(1, stringClass, nullptr);

    // Chuỗi cấu hình được giải mã từ offset 0x12a45c
    const char *setting_item = "-1_Toggle_True_Tự Động Lưu Và Nạp Cấu Hình (Tắt = Xoá Cấu Hình Đã Lưu)";
    jstring item = env->NewStringUTF(setting_item);
    env->SetObjectArrayElement(array, 0, item);
    env->DeleteLocalRef(item);
    env->DeleteLocalRef(stringClass);

    return array;
}

// Offset: 0x260430
jobjectArray Menu_GetFeatureList(JNIEnv *env, jobject thiz) {
    jclass stringClass = env->FindClass("java/lang/String");

    // Khởi tạo danh sách các mục tính năng mod menu
    std::vector<std::string> features = {
        "0_Category_HỖ TRỢ COMBO TƯỚNG",
        "1_Toggle_Florentino: Tự Động Nhặt Hoa & Combo Chiêu 2",
        "2_Toggle_Zata: Tự Động Tính Stack & Cất Cánh Ulti",
        "3_Toggle_Veres: Tự Động Vành Xích Nội Tại",
        "4_Category_CỬA HÀNG & MUA BÁN ĐỒ",
        "5_Toggle_Tự Động Bán Đồ (Giáp Hộ Mệnh -> Liềm / Cầu Băng Sương)",
        "6_Button_Cấu Hình Thứ Tự Mua/Bán Đồ Trận Đấu",
        "7_Category_NGOẠI HÌNH & HIỆU ỨNG",
        "8_Button_ĐỔI SKIN TRONG TRẬN (Mod Skin)",
        "9_Category_MACRO COMBO",
        "10_Toggle_Phát Macro Đã Ghi",
        "11_Button_GHI / DỪNG GHI MACRO",
        "12_Button_Nhập / Xuất Macro JSON (Clipboard)",
        "13_Category_BẢO VỆ TÀI KHOẢN",
        "14_Toggle_Bảo Vệ Tố Cáo (Chặn Tố Cáo Phá Hoại / AFK)"
    };

    jobjectArray array = env->NewObjectArray((jsize)features.size(), stringClass, nullptr);
    for (jsize i = 0; i < (jsize)features.size(); ++i) {
        jstring str = env->NewStringUTF(features[i].c_str());
        env->SetObjectArrayElement(array, i, str);
        env->DeleteLocalRef(str);
    }
    env->DeleteLocalRef(stringClass);
    return array;
}

// --------------------------------------------------------------------------
// 4. JNI Implementations: com.gamesafe.ano.Preferences & Main
// --------------------------------------------------------------------------

// Offset: 0x2845f4
void Preferences_Changes(JNIEnv *env, jclass clazz, jobject context, jint feature, 
                         jstring featName, jint value, jboolean is_bool, jstring strVal) {
    // feature == -1: Xử lý lưu / xóa cache cấu hình
    if (feature == -1) {
        if (!is_bool) {
            std::string cfg_path = "/data/user/0/" + g_package_name + "/cache/mod_config.json";
            unlink(cfg_path.c_str());
        }
        return;
    }
    // Dispatch xử lý toggle theo feature id
    // Cập nhật trạng thái bộ nhớ cho các luồng hook Il2Cpp
}

// Offset: 0x1d2ab0
void Main_CheckOverlayPermission(JNIEnv *env, jclass clazz, jobject context) {
    // Kiểm tra Build.VERSION.SDK_INT >= 23 (0x17)
    // Gọi Settings.canDrawOverlays(context)
    // Nếu chưa cấp quyền -> Khởi chạy Intent ACTION_MANAGE_OVERLAY_PERMISSION
}

} // extern "C"

// --------------------------------------------------------------------------
// 5. Hooking Engine & Il2Cpp Injection
// --------------------------------------------------------------------------
static void *(*orig_async_execute)(void *) = nullptr;

static void *hooked_async_execute(void *state) {
    // Khi thread của libil2cpp bắt đầu chạy
    if (!g_game_lib_loaded) {
        void *il2cpp_handle = dlopen("libil2cpp.so", RTLD_NOLOAD);
        if (il2cpp_handle) {
            g_game_lib_loaded = true;
            LOGI("libil2cpp.so loaded, applying game memory patches...");
            // Cài đặt hook vào các hàm LSkillComponent, SkillSlot, ActorLinker
        }
    }
    return orig_async_execute ? orig_async_execute(state) : nullptr;
}

static void *HookThread(void *) {
    // Đợi libil2cpp.so được nạp vào memory space
    while (!dlopen("libil2cpp.so", RTLD_NOLOAD)) {
        usleep(50000);
    }

    // Giảm thời gian chờ xuống 0.5 giây để tránh bị game kill
    LOGI("Đã tìm thấy libil2cpp.so, đợi 0.5 giây...");
    usleep(500000); 

    // Hook symbol runtime của libil2cpp
    void *target_sym = DobbySymbolResolver("libil2cpp.so", "il2cpp_init");
    if (target_sym) {
        LOGI("Bắt đầu Hook vào il2cpp_init...");
        DobbyHook(target_sym, (void *)hooked_async_execute, (void **)&orig_async_execute);
        LOGI("Hook thành công!");
    } else {
        LOGE("Không tìm thấy symbol il2cpp_init!");
    }

    LOGI("Đã chạy xong luồng Hook!");
    g_game_lib_loaded = true;
    return nullptr;
}

// --------------------------------------------------------------------------
// 6. JNI_OnLoad Entry Point (Offset: 0x285cdc)
// --------------------------------------------------------------------------
JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_vm = vm;
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    g_package_name = GetCurrentPackageName();

    // 1. Nạp thư viện anogs gốc nếu có
    std::string anogs_path = "/data/user/0/" + g_package_name + "/arm64-v8a/libanogs.so";
    dlopen(anogs_path.c_str(), RTLD_NOW | RTLD_GLOBAL);

    // 2. Đăng ký động các phương thức của com.gamesafe.ano.Menu
    JNINativeMethod menu_methods[] = {
        {"Icon", "()Ljava/lang/String;", (void *)Menu_Icon},
        {"IconWebViewData", "()Ljava/lang/String;", (void *)Menu_IconWebViewData},
        {"IsGameLibLoaded", "()Z", (void *)Menu_IsGameLibLoaded},
        {"Init", "(Landroid/content/Context;Landroid/widget/TextView;Landroid/widget/TextView;)V", (void *)Menu_Init},
        {"SettingsList", "()[Ljava/lang/String;", (void *)Menu_SettingsList},
        {"GetFeatureList", "()[Ljava/lang/String;", (void *)Menu_GetFeatureList}
    };
    jclass menu_cls = env->FindClass("com/gamesafe/ano/Menu");
    if (menu_cls) {
        env->RegisterNatives(menu_cls, menu_methods, sizeof(menu_methods) / sizeof(menu_methods[0]));
        env->DeleteLocalRef(menu_cls);
    }

    // 3. Đăng ký com.gamesafe.ano.Preferences.Changes
    JNINativeMethod pref_methods[] = {
        {"Changes", "(Landroid/content/Context;ILjava/lang/String;IZLjava/lang/String;)V", (void *)Preferences_Changes}
    };
    jclass pref_cls = env->FindClass("com/gamesafe/ano/Preferences");
    if (pref_cls) {
        env->RegisterNatives(pref_cls, pref_methods, sizeof(pref_methods) / sizeof(pref_methods[0]));
        env->DeleteLocalRef(pref_cls);
    }

    // 4. Đăng ký com.gamesafe.ano.Main.CheckOverlayPermission
    JNINativeMethod main_methods[] = {
        {"CheckOverlayPermission", "(Landroid/content/Context;)V", (void *)Main_CheckOverlayPermission}
    };
    jclass main_cls = env->FindClass("com/gamesafe/ano/Main");
    if (main_cls) {
        env->RegisterNatives(main_cls, main_methods, sizeof(main_methods) / sizeof(main_methods[0]));
        env->DeleteLocalRef(main_cls);
    }

    // 5. Khởi tạo thread giám sát và hook libil2cpp
    pthread_t th;
    pthread_create(&th, nullptr, HookThread, nullptr);
    pthread_detach(th);

    return JNI_VERSION_1_6;
}
