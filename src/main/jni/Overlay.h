#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x0040
#endif
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <mutex>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "Roboto-Regular.h"

class Overlay {
public:
    static Overlay& get() {
        static Overlay instance;
        return instance;
    }

    bool init(JNIEnv* env, jobject surface, int w, int h) {
        std::lock_guard<std::mutex> lock(mtx);
        if (running.load()) return false;

        nativeWindow = ANativeWindow_fromSurface(env, surface);
        if (!nativeWindow) return false;

        screenW = w;
        screenH = h;

        if (!initEGL()) {
            ANativeWindow_release(nativeWindow);
            nativeWindow = nullptr;
            return false;
        }

        setupImGui();
        running.store(true);
        pthread_create(&renderThread, nullptr, renderThreadEntry, this);
        return true;
    }

    void destroy() {
        running.store(false);
        if (renderThread) {
            pthread_join(renderThread, nullptr);
            renderThread = 0;
        }

        std::lock_guard<std::mutex> lock(mtx);

        if (imguiInitialized) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui::DestroyContext();
            imguiInitialized = false;
        }

        if (eglDisplay != EGL_NO_DISPLAY) {
            eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, eglSurface);
            if (eglContext != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay, eglContext);
            eglTerminate(eglDisplay);
        }

        eglDisplay = EGL_NO_DISPLAY;
        eglSurface = EGL_NO_SURFACE;
        eglContext = EGL_NO_CONTEXT;

        if (nativeWindow) {
            ANativeWindow_release(nativeWindow);
            nativeWindow = nullptr;
        }
    }

    void setScreenSize(int w, int h) {
        screenW = w;
        screenH = h;
    }

    void handleTouch(int action, float x, float y) {
        std::lock_guard<std::mutex> lock(touchMtx);
        touchX = x;
        touchY = y;
        if (action == 0) { // ACTION_DOWN
            pendingDown = true;
        } else if (action == 1 || action == 3) { // ACTION_UP / CANCEL
            pendingUp = true;
        }
        // ACTION_MOVE: só atualiza posição
    }

    int getScreenW() const { return screenW; }
    int getScreenH() const { return screenH; }
    bool isRunning() const { return running.load(); }

    // Callback de draw externo - setado pelo main.cpp
    void (*onDraw)(int screenW, int screenH) = nullptr;

private:
    Overlay() = default;
    ~Overlay() { destroy(); }
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool initEGL() {
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay == EGL_NO_DISPLAY) return false;

        EGLint major, minor;
        if (!eglInitialize(eglDisplay, &major, &minor)) return false;

        const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 0,
            EGL_STENCIL_SIZE, 0,
            EGL_NONE
        };

        EGLConfig config;
        EGLint numConfigs;
        if (!eglChooseConfig(eglDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0)
            return false;

        ANativeWindow_setBuffersGeometry(nativeWindow, 0, 0, EGL_NATIVE_VISUAL_ID);

        // Obtem o formato nativo
        EGLint format;
        eglGetConfigAttrib(eglDisplay, config, EGL_NATIVE_VISUAL_ID, &format);
        ANativeWindow_setBuffersGeometry(nativeWindow, 0, 0, format);

        eglSurface = eglCreateWindowSurface(eglDisplay, config, nativeWindow, nullptr);
        if (eglSurface == EGL_NO_SURFACE) return false;

        const EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE
        };

        eglContext = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
        if (eglContext == EGL_NO_CONTEXT) return false;

        if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext))
            return false;

        return true;
    }

    void setupImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        io.DisplaySize = ImVec2((float)screenW, (float)screenH);
        ImGui::StyleColorsDark();

        ImGuiStyle* style = &ImGui::GetStyle();
        style->Alpha = 1.0f;
        style->WindowTitleAlign = ImVec2(0.5f, 0.5f);
        style->PopupRounding = 3;
        style->WindowPadding = ImVec2(4, 4);
        style->FramePadding = ImVec2(2, 2);
        style->ItemSpacing = ImVec2(2, 2);
        style->ScrollbarSize = 17;
        style->WindowBorderSize = 1;
        style->ChildBorderSize = 1;
        style->PopupBorderSize = 3;
        style->FrameBorderSize = 1;
        style->WindowRounding = 3;
        style->ChildRounding = 3;
        style->FrameRounding = 3;
        style->ScrollbarRounding = 2;
        style->GrabRounding = 3;

        ImGui_ImplOpenGL3_Init("#version 300 es");

        ImFontConfig fontCfg;
        io.Fonts->AddFontFromMemoryTTF(&Roboto_Regular, sizeof(Roboto_Regular),
                                        40.0f, &fontCfg, io.Fonts->GetGlyphRangesCyrillic());

        ImGui::GetStyle().ScaleAllSizes(3.0f);
        imguiInitialized = true;
    }

    void processTouch() {
        std::lock_guard<std::mutex> lock(touchMtx);

        ImGuiIO& io = ImGui::GetIO();

        // Sempre atualizar posição do mouse
        if (touchX >= 0.0f && touchY >= 0.0f) {
            io.MousePos = ImVec2(touchX, touchY);
        }

        // DOWN tem prioridade: segura por 1 frame antes de processar UP
        if (pendingDown) {
            io.MouseDown[0] = true;
            pendingDown = false;
            // Não processar UP este frame — garante que ImGui vê o click
            return;
        }

        if (pendingUp) {
            io.MouseDown[0] = false;
            pendingUp = false;
            clearMousePosNextFrame = true;
        }
    }

    void renderFrame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)screenW, (float)screenH);
        io.DeltaTime = 1.0f / 60.0f;

        processTouch();

        // Limpa com fundo totalmente transparente
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Habilita blending para transparência
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // Callback de draw externo
        if (onDraw) {
            onDraw(screenW, screenH);
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        eglSwapBuffers(eglDisplay, eglSurface);

        if (clearMousePosNextFrame) {
            io.MousePos = ImVec2(-1, -1);
            clearMousePosNextFrame = false;
        }
    }

    static void* renderThreadEntry(void* arg) {
        Overlay* self = static_cast<Overlay*>(arg);

        // Re-bind EGL context nesta thread
        eglMakeCurrent(self->eglDisplay, self->eglSurface,
                        self->eglSurface, self->eglContext);

        while (self->running.load()) {
            self->renderFrame();
            usleep(16000); // ~60 FPS
        }

        eglMakeCurrent(self->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return nullptr;
    }

    // EGL
    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLSurface eglSurface = EGL_NO_SURFACE;
    EGLContext eglContext = EGL_NO_CONTEXT;
    ANativeWindow* nativeWindow = nullptr;

    // Screen
    std::atomic<int> screenW{0};
    std::atomic<int> screenH{0};

    // State
    std::atomic<bool> running{false};
    pthread_t renderThread = 0;
    bool imguiInitialized = false;
    bool clearMousePosNextFrame = false;
    std::mutex mtx;

    // Touch
    std::mutex touchMtx;
    float touchX = -1.0f;
    float touchY = -1.0f;
    bool pendingDown = false;
    bool pendingUp = false;
};
