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
LOCAL_MODULE := gl2

LOCAL_CFLAGS := -w -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CPPFLAGS := -w -Wno-error=format-security -fvisibility=hidden -Werror -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-debug
LOCAL_LDLIBS := -llog -landroid
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES := $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/Utils/Unity

# Sources - GameHook only (direct offsets, no ByNameModding)
HOOK_FILES := $(LOCAL_PATH)/GameHook.cpp

LOCAL_SRC_FILES := $(HOOK_FILES:$(LOCAL_PATH)/%=%)

include $(BUILD_SHARED_LIBRARY)

# ============================================================
# MODULE 3: libinjector.so — PTRACE INJECTOR (shared library)
# Compilado como .so para o APK empacotar (Android só extrai lib*.so)
# Invocado via LD_PRELOAD em shell root — constructor lê config
# e faz ptrace+dlopen no processo do jogo
# ============================================================

include $(CLEAR_VARS)
LOCAL_MODULE := injector

LOCAL_CFLAGS := -w -fvisibility=hidden -O2 -DNDEBUG -fPIC
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all
LOCAL_LDLIBS := -llog -ldl
LOCAL_ARM_MODE := arm

LOCAL_SRC_FILES := injector.c

include $(BUILD_SHARED_LIBRARY)