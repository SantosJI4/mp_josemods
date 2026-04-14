LOCAL_PATH := $(call my-dir)

# ============================================================
# MODULE 1: libMEOW.so — OVERLAY EXTERNO (APK)
# Processo separado - NÃO injeta no jogo
# Lê dados do SharedMemory e desenha ImGui
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := MEOW

LOCAL_CFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CPPFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all, -llog
LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/backends
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/ImGui/font
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity

# Sources - overlay + ImGui
FILE_LIST := $(LOCAL_PATH)/main.cpp
FILE_LIST += $(wildcard $(LOCAL_PATH)/src/ImGui/*.cpp*)
FILE_LIST += $(wildcard $(LOCAL_PATH)/src/ImGui/backends/*.cpp*)

LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 2: libHook.so — INJETADO NO JOGO (via script root)
# VMT Hook: troca methodPointer no MethodInfo do il2cpp
# Coleta dados e escreve no SharedMemory
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := Hook

LOCAL_CFLAGS := -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CPPFLAGS := -w -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-debug
LOCAL_LDLIBS := -llog -landroid
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity/ByNameModding

# Sources - GameHook + ByNameModding (Il2Cpp resolver + fake_dlfcn)
HOOK_FILES := $(LOCAL_PATH)/GameHook.cpp
HOOK_FILES += $(LOCAL_PATH)/include/Utils/Unity/ByNameModding/Il2Cpp.cpp
HOOK_FILES += $(LOCAL_PATH)/include/Utils/Unity/ByNameModding/fake_dlfcn.cpp

LOCAL_SRC_FILES := $(HOOK_FILES:$(LOCAL_PATH)/%=%)

include $(BUILD_SHARED_LIBRARY)