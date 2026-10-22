// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

// Expose API 24 declarations for dlsym function-pointer types on API 21.
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ 1
#include "ndkcamera.h"

#include <string>
#include <pthread.h>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <condition_variable>

#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>

#include <opencv2/core/core.hpp>

#include "mat.h"

// Keep the API 24 symbols out of the ELF imports so the same library loads on API 21.
#define NDK_CAMERA_FUNCTIONS(F) \
    F(camera, ACameraCaptureSession_close) \
    F(camera, ACameraCaptureSession_setRepeatingRequest) \
    F(camera, ACameraCaptureSession_stopRepeating) \
    F(camera, ACameraDevice_close) \
    F(camera, ACameraDevice_createCaptureRequest) \
    F(camera, ACameraDevice_createCaptureSession) \
    F(camera, ACameraManager_create) \
    F(camera, ACameraManager_delete) \
    F(camera, ACameraManager_deleteCameraIdList) \
    F(camera, ACameraManager_getCameraCharacteristics) \
    F(camera, ACameraManager_getCameraIdList) \
    F(camera, ACameraManager_openCamera) \
    F(camera, ACameraMetadata_free) \
    F(camera, ACameraMetadata_getConstEntry) \
    F(camera, ACameraOutputTarget_create) \
    F(camera, ACameraOutputTarget_free) \
    F(camera, ACaptureRequest_addTarget) \
    F(camera, ACaptureRequest_free) \
    F(camera, ACaptureSessionOutputContainer_add) \
    F(camera, ACaptureSessionOutputContainer_create) \
    F(camera, ACaptureSessionOutputContainer_free) \
    F(camera, ACaptureSessionOutput_create) \
    F(camera, ACaptureSessionOutput_free) \
    F(media, AImageReader_acquireLatestImage) \
    F(media, AImageReader_delete) \
    F(media, AImageReader_getWindow) \
    F(media, AImageReader_new) \
    F(media, AImageReader_setImageListener) \
    F(media, AImage_delete) \
    F(media, AImage_getFormat) \
    F(media, AImage_getHeight) \
    F(media, AImage_getPlaneData) \
    F(media, AImage_getPlanePixelStride) \
    F(media, AImage_getPlaneRowStride) \
    F(media, AImage_getWidth)

// These declarations are used only for function-pointer types, never direct calls.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
struct NdkCameraApi
{
#define DECLARE_FUNCTION(library, name) decltype(&::name) name;
    NDK_CAMERA_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION

    void* camera;
    void* media;

    void load()
    {
        if (camera) return;
        camera = dlopen("libcamera2ndk.so", RTLD_NOW | RTLD_LOCAL);
        media = dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
        if (!camera || !media)
        {
            unload();
            return;
        }
#define LOAD_FUNCTION(library, name) \
        name = reinterpret_cast<decltype(name)>(dlsym(library, #name)); \
        if (!name) { unload(); return; }
        NDK_CAMERA_FUNCTIONS(LOAD_FUNCTION)
#undef LOAD_FUNCTION
    }

    void unload()
    {
        if (camera) dlclose(camera);
        if (media) dlclose(media);
        camera = 0;
        media = 0;
    }
};
#pragma clang diagnostic pop
#undef NDK_CAMERA_FUNCTIONS

static NdkCameraApi camera_api = {};

// Error callbacks only enqueue work. Closing a backend may wait for those callbacks.
class CameraRecovery
{
public:
    explicit CameraRecovery(NdkCamera* camera)
        : owner(camera), stopping(false), pending(false), generation(0), retry(false)
    {
        started = pthread_create(&thread, 0, thread_entry, this) == 0;
        if (!started)
            __android_log_print(ANDROID_LOG_ERROR, "NdkCamera", "Recovery pthread_create failed");
    }

    ~CameraRecovery()
    {
        {
            std::lock_guard<std::mutex> guard(mutex);
            stopping = true;
            condition.notify_one();
        }
        if (started) pthread_join(thread, 0);
    }

    bool is_started() const { return started; }

    void notify(unsigned long long session, bool try_legacy)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (stopping) return;
        if (!pending || session > generation)
        {
            generation = session;
            retry = try_legacy;
        }
        else if (session == generation)
        {
            retry = retry && try_legacy;
        }
        pending = true;
        condition.notify_one();
    }

private:
    static void* thread_entry(void* context)
    {
        static_cast<CameraRecovery*>(context)->run();
        return 0;
    }

    void run()
    {
        for (;;)
        {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [this] { return stopping || pending; });
            if (stopping) return;
            lock.unlock();

            std::lock_guard<std::mutex> guard(owner->camera_mutex);
            lock.lock();
            if (stopping) return;
            const unsigned long long session = generation;
            const bool try_legacy = retry;
            pending = false;
            lock.unlock();

            // A close, switch, or synchronous fallback invalidates earlier errors.
            if (session != owner->camera_generation || !owner->camera_open) continue;
            const bool fallback = try_legacy && !owner->legacy_camera;
            owner->close_locked();
            __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "Camera closed after asynchronous error");
            if (fallback && (!owner->camera_managed || (owner->camera_active
                && owner->camera_permission && owner->camera_output_ready)))
            {
                __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "NDK device error; trying Camera1 once");
                owner->open_legacy();
            }
        }
    }

    NdkCamera* owner;
    pthread_t thread;
    bool started;
    std::mutex mutex;
    std::condition_variable condition;
    bool stopping;
    bool pending;
    unsigned long long generation;
    bool retry;
};

// Camera1 is a public Java API. Drive it from a native, JVM-attached Looper thread.
// Never call open/close from an image callback; close joins the capture thread.
class LegacyCamera
{
public:
    static int initialize(JNIEnv* env);
    static void uninitialize(JNIEnv* env);
    explicit LegacyCamera(NdkCamera* owner);
    ~LegacyCamera();
    int open();
    void close();

private:
    static void* thread_entry(void* context);
    void run();
    void signal_ready(int status);
    static void JNICALL preview(JNIEnv* env, jobject callback, jbyteArray data, jobject camera);
    static void JNICALL error(JNIEnv* env, jobject callback, jint code, jobject camera);

    NdkCamera* owner;
    unsigned long long generation;
    pthread_t thread;
    pthread_mutex_t ready_mutex;
    pthread_cond_t ready_condition;
    bool thread_started;
    bool ready;
    int open_status;
    jobject looper;
    jmethodID quit;
    jmethodID add_buffer;
    int width;
    int height;
    unsigned int borrowed_frames;
    unsigned int copied_frames;
    // Allocated only if the VM cannot lend its byte[] storage without a copy.
    std::vector<unsigned char> nv21;
};

namespace {
JavaVM* java_vm = 0;
jclass callback_class = 0;
jfieldID callback_context = 0;

// A failed JNI call must not leave an exception pending while cleanup calls Java.
bool clear_exception(JNIEnv* env)
{
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }
    return false;
}

// Cleanup must continue even if a disconnected vendor camera throws on stopPreview.
void cleanup_call(JNIEnv* env, jobject object, jmethodID m)
{
    if (object && m)
    {
        env->CallVoidMethod(object, m);
        clear_exception(env);
    }
}
}

int LegacyCamera::initialize(JNIEnv* env)
{
    if (callback_class) return 0;
    if (env->GetJavaVM(&java_vm) != JNI_OK) return -1;
    jclass c = env->FindClass("com/tencent/ndkcamera/CameraCallback");
    if (!c) return -1;
    callback_context = env->GetFieldID(c, "nativeCamera", "J");
    if (!callback_context)
    {
        env->DeleteLocalRef(c);
        return -1;
    }
    const JNINativeMethod methods[] = {
        {const_cast<char*>("onPreviewFrame"), const_cast<char*>("([BLandroid/hardware/Camera;)V"), reinterpret_cast<void*>(preview)},
        {const_cast<char*>("onError"), const_cast<char*>("(ILandroid/hardware/Camera;)V"), reinterpret_cast<void*>(error)}
    };
    if (env->RegisterNatives(c, methods, 2) != JNI_OK)
    {
        env->DeleteLocalRef(c);
        return -1;
    }
    callback_class = static_cast<jclass>(env->NewGlobalRef(c));
    env->DeleteLocalRef(c);
    return callback_class ? 0 : -1;
}

void LegacyCamera::uninitialize(JNIEnv* env)
{
    if (callback_class) env->DeleteGlobalRef(callback_class);
    callback_class = 0;
    callback_context = 0;
    java_vm = 0;
}

LegacyCamera::LegacyCamera(NdkCamera* camera)
    : owner(camera), generation(camera->camera_generation), thread_started(false), ready(false), open_status(-1), looper(0), quit(0), add_buffer(0), width(0), height(0), borrowed_frames(0), copied_frames(0)
{
    pthread_mutex_init(&ready_mutex, 0);
    pthread_cond_init(&ready_condition, 0);
}

LegacyCamera::~LegacyCamera()
{
    close();
    pthread_cond_destroy(&ready_condition);
    pthread_mutex_destroy(&ready_mutex);
}

int LegacyCamera::open()
{
    if (!java_vm || !callback_class) return -1;
    pthread_mutex_lock(&ready_mutex);
    ready = false;
    int error = pthread_create(&thread, 0, thread_entry, this);
    if (error != 0)
    {
        pthread_mutex_unlock(&ready_mutex);
        __android_log_print(ANDROID_LOG_ERROR, "NdkCamera", "Camera1 pthread_create failed: %d", error);
        return -1;
    }
    thread_started = true;
    while (!ready) pthread_cond_wait(&ready_condition, &ready_mutex);
    int status = open_status;
    pthread_mutex_unlock(&ready_mutex);
    if (status != 0) close();
    return status;
}

void LegacyCamera::close()
{
    if (!thread_started) return;
    JNIEnv* env = 0;
    bool attached = java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK;
    if (attached && java_vm->AttachCurrentThread(&env, 0) != JNI_OK)
        std::abort(); // Cannot safely destroy a camera while its thread is alive.
    cleanup_call(env, looper, quit);
    pthread_join(thread, 0); // No callback can use this/owner after the join.
    thread_started = false;
    if (looper) env->DeleteGlobalRef(looper);
    looper = 0;
    if (attached) java_vm->DetachCurrentThread();
}

void* LegacyCamera::thread_entry(void* context)
{
    static_cast<LegacyCamera*>(context)->run();
    return 0;
}

void LegacyCamera::signal_ready(int status)
{
    pthread_mutex_lock(&ready_mutex);
    open_status = status;
    ready = true;
    pthread_cond_signal(&ready_condition);
    pthread_mutex_unlock(&ready_mutex);
}

void LegacyCamera::run()
{
    JNIEnv* env = 0;
    if (java_vm->AttachCurrentThread(&env, 0) != JNI_OK)
    {
        signal_ready(-1);
        return;
    }
    jobject camera = 0;
    jobject texture = 0;
    jmethodID stop = 0, release = 0, release_texture = 0;
    bool started = false;
    {
        // Camera delivers its Java callbacks on the Looper of the opening thread.
        jclass lc = env->FindClass("android/os/Looper");
        env->CallStaticVoidMethod(lc, env->GetStaticMethodID(lc, "prepare", "()V"));
        if (env->ExceptionCheck()) goto cleanup;
        jobject local_looper = env->CallStaticObjectMethod(lc, env->GetStaticMethodID(lc, "myLooper", "()Landroid/os/Looper;"));
        looper = env->NewGlobalRef(local_looper);
        if (!looper) goto cleanup;
        quit = env->GetMethodID(lc, "quit", "()V");
        jmethodID loop = env->GetStaticMethodID(lc, "loop", "()V");

        jclass cc = env->FindClass("android/hardware/Camera");
        jclass ic = env->FindClass("android/hardware/Camera$CameraInfo");
        jmethodID info_method = env->GetStaticMethodID(cc, "getCameraInfo", "(ILandroid/hardware/Camera$CameraInfo;)V");
        jobject info = env->NewObject(ic, env->GetMethodID(ic, "<init>", "()V"));
        if (env->ExceptionCheck()) goto cleanup;
        jfieldID facing_field = env->GetFieldID(ic, "facing", "I");
        jfieldID orientation_field = env->GetFieldID(ic, "orientation", "I");

        int count = env->CallStaticIntMethod(cc, env->GetStaticMethodID(cc, "getNumberOfCameras", "()I"));
        if (env->ExceptionCheck()) goto cleanup;
        int id = -1;
        for (int i = 0; i < count; i++)
        {
            env->CallStaticVoidMethod(cc, info_method, i, info);
            if (env->ExceptionCheck()) goto cleanup;
            // Camera1: BACK=0 FRONT=1; NdkCamera's public convention is reversed.
            if (env->GetIntField(info, facing_field) == (owner->camera_facing == 0 ? 1 : 0))
            {
                id = i;
                owner->camera_orientation = env->GetIntField(info, orientation_field);
                break;
            }
        }
        if (id < 0) goto cleanup;
        stop = env->GetMethodID(cc, "stopPreview", "()V");
        release = env->GetMethodID(cc, "release", "()V");
        camera = env->CallStaticObjectMethod(cc, env->GetStaticMethodID(cc, "open", "(I)Landroid/hardware/Camera;"), id);
        if (env->ExceptionCheck()) goto cleanup;

        jclass pc = env->FindClass("android/hardware/Camera$Parameters");
        jobject parameters = env->CallObjectMethod(camera, env->GetMethodID(cc, "getParameters", "()Landroid/hardware/Camera$Parameters;"));
        if (env->ExceptionCheck()) goto cleanup;
        jobject sizes = env->CallObjectMethod(parameters, env->GetMethodID(pc, "getSupportedPreviewSizes", "()Ljava/util/List;"));
        if (env->ExceptionCheck()) goto cleanup;
        jclass list = env->FindClass("java/util/List");
        jmethodID size = env->GetMethodID(list, "size", "()I");
        jmethodID get = env->GetMethodID(list, "get", "(I)Ljava/lang/Object;");
        jclass sc = env->FindClass("android/hardware/Camera$Size");
        jfieldID wf = env->GetFieldID(sc, "width", "I");
        jfieldID hf = env->GetFieldID(sc, "height", "I");

        int n = env->CallIntMethod(sizes, size);
        double best = std::numeric_limits<double>::max();
        for (int i = 0; i < n; i++)
        {
            jobject s = env->CallObjectMethod(sizes, get, i);
            int w = env->GetIntField(s, wf), h = env->GetIntField(s, hf);
            env->DeleteLocalRef(s);
            double score = std::abs(std::log((double)w / h / (640.0 / 480))) * 4
                         + std::abs(std::log((double)w * h / (640.0 * 480)));
            if (score < best) { best = score; width = w; height = h; }
        }
        env->CallVoidMethod(parameters, env->GetMethodID(pc, "setPreviewSize", "(II)V"), width, height);
        // NV21 is required for the Camera1 default preview callback format.
        env->CallVoidMethod(parameters, env->GetMethodID(pc, "setPreviewFormat", "(I)V"), 17);

        jobject modes = env->CallObjectMethod(parameters, env->GetMethodID(pc, "getSupportedFocusModes", "()Ljava/util/List;"));
        if (env->ExceptionCheck()) goto cleanup;
        if (modes)
        {
            jstring continuous = env->NewStringUTF("continuous-video");
            if (!continuous) goto cleanup;
            jboolean supported = env->CallBooleanMethod(modes, env->GetMethodID(list, "contains", "(Ljava/lang/Object;)Z"), continuous);
            if (supported)
            {
                env->CallVoidMethod(parameters, env->GetMethodID(pc, "setFocusMode", "(Ljava/lang/String;)V"), continuous);
            }
        }
        env->CallVoidMethod(camera, env->GetMethodID(cc, "setParameters", "(Landroid/hardware/Camera$Parameters;)V"), parameters);
        if (env->ExceptionCheck()) goto cleanup;
        // Read back negotiated parameters before allocating buffers. Some old HALs
        // round the requested size even when setParameters succeeds.
        parameters = env->CallObjectMethod(camera, env->GetMethodID(cc, "getParameters", "()Landroid/hardware/Camera$Parameters;"));
        if (env->ExceptionCheck()) goto cleanup;
        jobject actual_size = env->CallObjectMethod(parameters, env->GetMethodID(pc, "getPreviewSize", "()Landroid/hardware/Camera$Size;"));
        if (env->ExceptionCheck()) goto cleanup;
        width = env->GetIntField(actual_size, wf);
        height = env->GetIntField(actual_size, hf);

        // The HAL needs a preview sink even though all displayed pixels come from NV21.
        // The multi-buffer SurfaceTexture drops old preview buffers; no GL rendering needed.
        jclass tc = env->FindClass("android/graphics/SurfaceTexture");
        release_texture = env->GetMethodID(tc, "release", "()V");
        texture = env->NewObject(tc, env->GetMethodID(tc, "<init>", "(I)V"), 0);
        if (env->ExceptionCheck()) goto cleanup;
        env->CallVoidMethod(camera, env->GetMethodID(cc, "setPreviewTexture", "(Landroid/graphics/SurfaceTexture;)V"), texture);
        if (env->ExceptionCheck()) goto cleanup;
        jobject callback = env->NewObject(callback_class, env->GetMethodID(callback_class, "<init>", "(J)V"), reinterpret_cast<jlong>(this));
        if (env->ExceptionCheck()) goto cleanup;
        env->CallVoidMethod(camera, env->GetMethodID(cc, "setPreviewCallbackWithBuffer", "(Landroid/hardware/Camera$PreviewCallback;)V"), callback);
        if (env->ExceptionCheck()) goto cleanup;
        env->CallVoidMethod(camera, env->GetMethodID(cc, "setErrorCallback", "(Landroid/hardware/Camera$ErrorCallback;)V"), callback);
        if (env->ExceptionCheck()) goto cleanup;
        add_buffer = env->GetMethodID(cc, "addCallbackBuffer", "([B)V");
        const jsize buffer_size = (jsize)((int64_t)width * height * 3 / 2);
        for (int i = 0; i < 3; i++)
        {
            jbyteArray buffer = env->NewByteArray(buffer_size);
            if (!buffer) goto cleanup;
            env->CallVoidMethod(camera, add_buffer, buffer);
            if (env->ExceptionCheck()) goto cleanup;
            env->DeleteLocalRef(buffer);
        }
        env->CallVoidMethod(camera, env->GetMethodID(cc, "startPreview", "()V"));
        if (env->ExceptionCheck()) goto cleanup;
        __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "Camera1 opened id=%d facing=%d orientation=%d %dx%d", id, owner->camera_facing, owner->camera_orientation, width, height);
        started = true;
        signal_ready(0);
        env->CallStaticVoidMethod(lc, loop);
    }
cleanup:
    clear_exception(env);
    cleanup_call(env, camera, stop);
    cleanup_call(env, camera, release);
    cleanup_call(env, texture, release_texture);
    if (!started)
    {
        __android_log_print(ANDROID_LOG_ERROR, "NdkCamera", "Camera1 open failed");
        signal_ready(-1);
    }
    __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "Camera1 closed borrowed=%u copied=%u", borrowed_frames, copied_frames);
    // Also report an unexpected Looper exit or a failed preview callback.
    // Normal close has already invalidated this generation before joining us.
    if (started) owner->recovery->notify(generation, false);
    // All thread-local JNI references are released when detaching.
    java_vm->DetachCurrentThread();
}

void JNICALL LegacyCamera::preview(JNIEnv* env, jobject callback, jbyteArray data, jobject camera)
{
    LegacyCamera* self = reinterpret_cast<LegacyCamera*>(env->GetLongField(callback, callback_context));
    const jsize buffer_size = (jsize)((int64_t)self->width * self->height * 3 / 2);
    if (!data) goto cleanup;
    if (self->nv21.empty())
    {
        jboolean is_copy = JNI_FALSE;
        // Borrow until on_image returns; release before requeueing the buffer.
        jbyte* bytes = env->GetByteArrayElements(data, &is_copy);
        if (!bytes) goto cleanup;
        self->owner->on_image(reinterpret_cast<const unsigned char*>(bytes), self->width, self->height);
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
        if (env->ExceptionCheck()) goto cleanup;
        if (is_copy)
        {
            ++self->copied_frames;
            // Avoid a VM-allocated temporary copy on every subsequent frame.
            self->nv21.resize(buffer_size);
            __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "Camera1 array access isCopy=1; using reusable copy buffer");
        }
        else
        {
            if (++self->borrowed_frames == 1)
                __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "Camera1 array access isCopy=0; borrowing byte[]");
        }
        // The camera may overwrite data as soon as it is queued again.
        env->CallVoidMethod(camera, self->add_buffer, data);
        if (env->ExceptionCheck()) goto cleanup;
    }
    else
    {
        env->GetByteArrayRegion(data, 0, buffer_size, reinterpret_cast<jbyte*>(self->nv21.data()));
        if (env->ExceptionCheck()) goto cleanup;
        env->CallVoidMethod(camera, self->add_buffer, data);
        if (env->ExceptionCheck()) goto cleanup;
        self->owner->on_image(self->nv21.data(), self->width, self->height);
        if (env->ExceptionCheck()) goto cleanup;
        ++self->copied_frames;
    }
    return;
cleanup:
    clear_exception(env);
    cleanup_call(env, self->looper, self->quit);
}

void JNICALL LegacyCamera::error(JNIEnv* env, jobject callback, jint code, jobject)
{
    LegacyCamera* self = reinterpret_cast<LegacyCamera*>(env->GetLongField(callback, callback_context));
    __android_log_print(ANDROID_LOG_ERROR, "NdkCamera", "Camera1 error=%d", code);
    cleanup_call(env, self->looper, self->quit);
}

void NdkCamera::onDisconnected(void* context, ACameraDevice* device)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onDisconnected %p", device);
    DeviceCallbackContext* callback = static_cast<DeviceCallbackContext*>(context);
    callback->camera->recovery->notify(callback->generation, false);
}

void NdkCamera::onError(void* context, ACameraDevice* device, int error)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onError %p %d", device, error);
    DeviceCallbackContext* callback = static_cast<DeviceCallbackContext*>(context);
    // Occupancy, policy, disconnection and service failures need external recovery.
    callback->camera->recovery->notify(callback->generation, error == ERROR_CAMERA_DEVICE);
}

void NdkCamera::onImageAvailable(void* context, AImageReader* reader)
{
//     __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onImageAvailable %p", reader);

    NdkCamera* self = (NdkCamera*)context;
    std::lock_guard<std::mutex> guard(self->image_mutex);
    if (!self->accepting_images) return;

    AImage* image = 0;
    media_status_t status = camera_api.AImageReader_acquireLatestImage(reader, &image);

    if (status != AMEDIA_OK)
    {
        // error
        return;
    }

    int32_t format;
    camera_api.AImage_getFormat(image, &format);

    // assert format == AIMAGE_FORMAT_YUV_420_888

    int32_t width = 0;
    int32_t height = 0;
    camera_api.AImage_getWidth(image, &width);
    camera_api.AImage_getHeight(image, &height);

    int32_t y_pixelStride = 0;
    int32_t u_pixelStride = 0;
    int32_t v_pixelStride = 0;
    camera_api.AImage_getPlanePixelStride(image, 0, &y_pixelStride);
    camera_api.AImage_getPlanePixelStride(image, 1, &u_pixelStride);
    camera_api.AImage_getPlanePixelStride(image, 2, &v_pixelStride);

    int32_t y_rowStride = 0;
    int32_t u_rowStride = 0;
    int32_t v_rowStride = 0;
    camera_api.AImage_getPlaneRowStride(image, 0, &y_rowStride);
    camera_api.AImage_getPlaneRowStride(image, 1, &u_rowStride);
    camera_api.AImage_getPlaneRowStride(image, 2, &v_rowStride);

    uint8_t* y_data = 0;
    uint8_t* u_data = 0;
    uint8_t* v_data = 0;
    int y_len = 0;
    int u_len = 0;
    int v_len = 0;
    camera_api.AImage_getPlaneData(image, 0, &y_data, &y_len);
    camera_api.AImage_getPlaneData(image, 1, &u_data, &u_len);
    camera_api.AImage_getPlaneData(image, 2, &v_data, &v_len);

    if (u_data == v_data + 1 && v_data == y_data + width * height && y_pixelStride == 1 && u_pixelStride == 2 && v_pixelStride == 2 && y_rowStride == width && u_rowStride == width && v_rowStride == width)
    {
        // already nv21  :)
        ((NdkCamera*)context)->on_image((unsigned char*)y_data, (int)width, (int)height);
    }
    else
    {
        // construct nv21
        unsigned char* nv21 = new unsigned char[width * height + width * height / 2];
        {
            // Y
            unsigned char* yptr = nv21;
            for (int y=0; y<height; y++)
            {
                const unsigned char* y_data_ptr = y_data + y_rowStride * y;
                for (int x=0; x<width; x++)
                {
                    yptr[0] = y_data_ptr[0];
                    yptr++;
                    y_data_ptr += y_pixelStride;
                }
            }

            // UV
            unsigned char* uvptr = nv21 + width * height;
            for (int y=0; y<height/2; y++)
            {
                const unsigned char* v_data_ptr = v_data + v_rowStride * y;
                const unsigned char* u_data_ptr = u_data + u_rowStride * y;
                for (int x=0; x<width/2; x++)
                {
                    uvptr[0] = v_data_ptr[0];
                    uvptr[1] = u_data_ptr[0];
                    uvptr += 2;
                    v_data_ptr += v_pixelStride;
                    u_data_ptr += u_pixelStride;
                }
            }
        }

        ((NdkCamera*)context)->on_image((unsigned char*)nv21, (int)width, (int)height);

        delete[] nv21;
    }

    camera_api.AImage_delete(image);
}

static void onSessionActive(void* context, ACameraCaptureSession *session)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onSessionActive %p", session);
}

static void onSessionReady(void* context, ACameraCaptureSession *session)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onSessionReady %p", session);
}

static void onSessionClosed(void* context, ACameraCaptureSession *session)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onSessionClosed %p", session);
}

void onCaptureFailed(void* context, ACameraCaptureSession* session, ACaptureRequest* request, ACameraCaptureFailure* failure)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onCaptureFailed %p %p %p", session, request, failure);
}

void onCaptureSequenceCompleted(void* context, ACameraCaptureSession* session, int sequenceId, int64_t frameNumber)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onCaptureSequenceCompleted %p %d %ld", session, sequenceId, frameNumber);
}

void onCaptureSequenceAborted(void* context, ACameraCaptureSession* session, int sequenceId)
{
    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onCaptureSequenceAborted %p %d", session, sequenceId);
}

void onCaptureCompleted(void* context, ACameraCaptureSession* session, ACaptureRequest* request, const ACameraMetadata* result)
{
//     __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "onCaptureCompleted %p %p %p", session, request, result);
}

int NdkCamera::initialize(JavaVM* vm)
{
    JNIEnv* env = 0;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK) return -1;
    jclass version = env->FindClass("android/os/Build$VERSION");
    int sdk = env->GetStaticIntField(version, env->GetStaticFieldID(version, "SDK_INT", "I"));
    env->DeleteLocalRef(version);
    if (sdk >= 24) camera_api.load();
    return LegacyCamera::initialize(env);
}

void NdkCamera::uninitialize(JavaVM* vm)
{
    JNIEnv* env = 0;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) == JNI_OK)
        LegacyCamera::uninitialize(env);
    camera_api.unload();
}

NdkCamera::NdkCamera()
{
    camera_facing = 0;
    camera_orientation = 0;
    camera_managed = false;
    camera_active = false;
    camera_permission = false;
    camera_output_ready = true;
    camera_open = false;
    accepting_images = false;
    legacy_camera = 0;

    camera_manager = 0;
    camera_device = 0;
    image_reader = 0;
    image_reader_surface = 0;
    image_reader_target = 0;
    capture_request = 0;
    capture_session_output_container = 0;
    capture_session_output = 0;
    capture_session = 0;
    camera_generation = 0;
    recovery = new CameraRecovery(this);
}

NdkCamera::~NdkCamera()
{
    close();
    delete recovery;
}

int NdkCamera::open(int facing)
{
    std::lock_guard<std::mutex> guard(camera_mutex);
    camera_managed = false;
    return open_locked(facing == -1 ? camera_facing : facing);
}

int NdkCamera::switch_camera()
{
    std::lock_guard<std::mutex> guard(camera_mutex);
    const int old_facing = camera_facing;
    const bool was_open = camera_open;
    if (camera_managed && (!camera_active || !camera_permission || !camera_output_ready))
    {
        camera_facing = 1 - old_facing;
        return 0;
    }
    if (open_locked(1 - old_facing) == 0) return 0;
    if (was_open) open_locked(old_facing);
    return -1;
}

int NdkCamera::open_locked(int facing)
{
    close_locked();
    camera_facing = facing;
    camera_orientation = 0;
    if (!recovery->is_started()) return -1;
    if (open_ndk() == 0)
    {
        camera_open = true;
        return 0;
    }
    close_ndk();
    __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "NDK unavailable for facing=%d; trying Camera1", facing);
    return open_legacy();
}

int NdkCamera::open_legacy()
{
    ++camera_generation;
    legacy_camera = new LegacyCamera(this);
    if (legacy_camera->open() == 0)
    {
        camera_open = true;
        return 0;
    }
    delete legacy_camera;
    legacy_camera = 0;
    return -1;
}

int NdkCamera::open_ndk()
{
    if (!camera_api.camera) return -1;

    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "open");

    camera_manager = camera_api.ACameraManager_create();

    // find front camera
    std::string camera_id;
    {
        ACameraIdList* camera_id_list = 0;
        if (camera_api.ACameraManager_getCameraIdList(camera_manager, &camera_id_list) != ACAMERA_OK)
            return -1;

        for (int i = 0; i < camera_id_list->numCameras; ++i)
        {
            const char* id = camera_id_list->cameraIds[i];
            ACameraMetadata* camera_metadata = 0;
            if (camera_api.ACameraManager_getCameraCharacteristics(camera_manager, id, &camera_metadata) != ACAMERA_OK)
                continue;

            // query faceing
            acamera_metadata_enum_android_lens_facing_t facing = ACAMERA_LENS_FACING_FRONT;
            {
                ACameraMetadata_const_entry e = { 0 };
                camera_api.ACameraMetadata_getConstEntry(camera_metadata, ACAMERA_LENS_FACING, &e);
                facing = (acamera_metadata_enum_android_lens_facing_t)e.data.u8[0];
            }

            if (camera_facing == 0 && facing != ACAMERA_LENS_FACING_FRONT)
            {
                camera_api.ACameraMetadata_free(camera_metadata);
                continue;
            }

            if (camera_facing == 1 && facing != ACAMERA_LENS_FACING_BACK)
            {
                camera_api.ACameraMetadata_free(camera_metadata);
                continue;
            }

            camera_id = id;

            // query orientation
            int orientation = 0;
            {
                ACameraMetadata_const_entry e = { 0 };
                camera_api.ACameraMetadata_getConstEntry(camera_metadata, ACAMERA_SENSOR_ORIENTATION, &e);

                orientation = (int)e.data.i32[0];
            }

            camera_orientation = orientation;

            camera_api.ACameraMetadata_free(camera_metadata);

            break;
        }

        camera_api.ACameraManager_deleteCameraIdList(camera_id_list);
    }

    __android_log_print(ANDROID_LOG_WARN, "NdkCamera", "open %s %d", camera_id.c_str(), camera_orientation);

    if (camera_id.empty()) return -1;

    // setup imagereader and its surface
    {
        if (camera_api.AImageReader_new(640, 480, AIMAGE_FORMAT_YUV_420_888, /*maxImages*/2, &image_reader) != AMEDIA_OK)
            return -1;

        AImageReader_ImageListener listener;
        listener.context = this;
        listener.onImageAvailable = onImageAvailable;

        camera_api.AImageReader_setImageListener(image_reader, &listener);

        camera_api.AImageReader_getWindow(image_reader, &image_reader_surface);
    }

    // open camera
    {
        device_callback_context.camera = this;
        device_callback_context.generation = camera_generation;
        ACameraDevice_StateCallbacks camera_device_state_callbacks = {};
        camera_device_state_callbacks.context = &device_callback_context;
        camera_device_state_callbacks.onDisconnected = onDisconnected;
        camera_device_state_callbacks.onError = onError;

        if (camera_api.ACameraManager_openCamera(camera_manager, camera_id.c_str(), &camera_device_state_callbacks, &camera_device) != ACAMERA_OK)
            return -1;
    }

    // capture request
    {
        if (camera_api.ACameraDevice_createCaptureRequest(camera_device, TEMPLATE_PREVIEW, &capture_request) != ACAMERA_OK)
            return -1;

        camera_api.ACameraOutputTarget_create(image_reader_surface, &image_reader_target);
        camera_api.ACaptureRequest_addTarget(capture_request, image_reader_target);
    }

    // capture session
    {
        ACameraCaptureSession_stateCallbacks camera_capture_session_state_callbacks;
        camera_capture_session_state_callbacks.context = this;
        camera_capture_session_state_callbacks.onActive = onSessionActive;
        camera_capture_session_state_callbacks.onReady = onSessionReady;
        camera_capture_session_state_callbacks.onClosed = onSessionClosed;

        camera_api.ACaptureSessionOutputContainer_create(&capture_session_output_container);

        camera_api.ACaptureSessionOutput_create(image_reader_surface, &capture_session_output);

        camera_api.ACaptureSessionOutputContainer_add(capture_session_output_container, capture_session_output);

        if (camera_api.ACameraDevice_createCaptureSession(camera_device, capture_session_output_container, &camera_capture_session_state_callbacks, &capture_session) != ACAMERA_OK)
            return -1;

        ACameraCaptureSession_captureCallbacks camera_capture_session_capture_callbacks = { 0 };
        camera_capture_session_capture_callbacks.context = this;
        camera_capture_session_capture_callbacks.onCaptureStarted = 0;
        camera_capture_session_capture_callbacks.onCaptureProgressed = 0;
        camera_capture_session_capture_callbacks.onCaptureCompleted = onCaptureCompleted;
        camera_capture_session_capture_callbacks.onCaptureFailed = onCaptureFailed;
        camera_capture_session_capture_callbacks.onCaptureSequenceCompleted = onCaptureSequenceCompleted;
        camera_capture_session_capture_callbacks.onCaptureSequenceAborted = onCaptureSequenceAborted;
        camera_capture_session_capture_callbacks.onCaptureBufferLost = 0;

        accepting_images = true;
        if (camera_api.ACameraCaptureSession_setRepeatingRequest(capture_session, &camera_capture_session_capture_callbacks, 1, &capture_request, nullptr) != ACAMERA_OK)
            return -1;
    }

    __android_log_print(ANDROID_LOG_INFO, "NdkCamera", "NDK opened id=%s facing=%d orientation=%d 640x480", camera_id.c_str(), camera_facing, camera_orientation);
    return 0;
}

void NdkCamera::close()
{
    std::lock_guard<std::mutex> guard(camera_mutex);
    camera_managed = false;
    close_locked();
}

void NdkCamera::close_locked()
{
    ++camera_generation;
    delete legacy_camera;
    legacy_camera = 0;
    close_ndk();
    camera_open = false;
}

int NdkCamera::set_active(bool active)
{
    std::lock_guard<std::mutex> guard(camera_mutex);
    camera_managed = true;
    camera_active = active;
    return update_camera();
}

int NdkCamera::set_permission(bool granted)
{
    std::lock_guard<std::mutex> guard(camera_mutex);
    camera_permission = granted;
    return update_camera();
}

int NdkCamera::set_output_ready(bool ready)
{
    std::lock_guard<std::mutex> guard(camera_mutex);
    camera_output_ready = ready;
    return update_camera();
}

int NdkCamera::update_camera()
{
    // Caller holds camera_mutex; frame callbacks never take this lock.
    if (!camera_managed) return 0;
    if (!camera_active || !camera_permission || !camera_output_ready)
    {
        if (camera_open) close_locked();
        return 0;
    }
    return camera_open ? 0 : open_locked(camera_facing);
}

void NdkCamera::close_ndk()
{
    accepting_images = false;
    // Drain the active frame before releasing its dependencies.
    { std::lock_guard<std::mutex> guard(image_mutex); }

    if (capture_session)
    {
        camera_api.ACameraCaptureSession_stopRepeating(capture_session);
        camera_api.ACameraCaptureSession_close(capture_session);
        capture_session = 0;
    }

    if (camera_device)
    {
        camera_api.ACameraDevice_close(camera_device);
        camera_device = 0;
    }

    if (capture_session_output_container)
    {
        camera_api.ACaptureSessionOutputContainer_free(capture_session_output_container);
        capture_session_output_container = 0;
    }

    if (capture_session_output)
    {
        camera_api.ACaptureSessionOutput_free(capture_session_output);
        capture_session_output = 0;
    }

    if (capture_request)
    {
        camera_api.ACaptureRequest_free(capture_request);
        capture_request = 0;
    }

    if (image_reader_target)
    {
        camera_api.ACameraOutputTarget_free(image_reader_target);
        image_reader_target = 0;
    }

    // Deleting the reader joins its callback thread; its window is borrowed.
    if (image_reader)
    {
        camera_api.AImageReader_delete(image_reader);
        image_reader = 0;
    }
    image_reader_surface = 0;

    if (camera_manager)
    {
        camera_api.ACameraManager_delete(camera_manager);
        camera_manager = 0;
    }
}

void NdkCamera::on_image(const cv::Mat& rgb) const
{
}

void NdkCamera::on_image(const unsigned char* nv21, int nv21_width, int nv21_height) const
{
    // rotate nv21
    int w = 0;
    int h = 0;
    int rotate_type = 0;
    {
        if (camera_orientation == 0)
        {
            w = nv21_width;
            h = nv21_height;
            rotate_type = camera_facing == 0 ? 2 : 1;
        }
        if (camera_orientation == 90)
        {
            w = nv21_height;
            h = nv21_width;
            rotate_type = camera_facing == 0 ? 5 : 6;
        }
        if (camera_orientation == 180)
        {
            w = nv21_width;
            h = nv21_height;
            rotate_type = camera_facing == 0 ? 4 : 3;
        }
        if (camera_orientation == 270)
        {
            w = nv21_height;
            h = nv21_width;
            rotate_type = camera_facing == 0 ? 7 : 8;
        }
    }

    cv::Mat nv21_rotated(h + h / 2, w, CV_8UC1);
    ncnn::kanna_rotate_yuv420sp(nv21, nv21_width, nv21_height, nv21_rotated.data, w, h, rotate_type);

    // nv21_rotated to rgb
    cv::Mat rgb(h, w, CV_8UC3);
    ncnn::yuv420sp2rgb(nv21_rotated.data, w, h, rgb.data);

    on_image(rgb);
}

// Frames pull sensor events directly. A Java Looper requires a callback;
// unregister this notification after its first dispatch so it cannot spin on
// an unread sensor fd between camera frames. The queue remains readable.
static int sensor_notification(int, int, void*)
{
    return 0;
}

NdkCameraWindow::NdkCameraWindow() : NdkCamera()
{
    set_output_ready(false);
    sensor_manager = 0;
    sensor_looper = 0;
    sensor_event_queue = 0;
    accelerometer_sensor = 0;
    win = 0;

    accelerometer_orientation = 0;

    // sensor
    sensor_manager = ASensorManager_getInstance();

    if (sensor_manager)
        accelerometer_sensor = ASensorManager_getDefaultSensor(sensor_manager, ASENSOR_TYPE_ACCELEROMETER);
}

NdkCameraWindow::~NdkCameraWindow()
{
    close();
    set_window(0);
}

void NdkCameraWindow::set_window(ANativeWindow* _win)
{
    std::lock_guard<std::mutex> guard(surface_mutex);
    set_window_locked(_win);
}

int NdkCameraWindow::set_surface(JNIEnv* env, jobject surface)
{
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : 0;
    if (surface && !window) return -1;
    std::lock_guard<std::mutex> guard(surface_mutex);
    int ret = set_window_locked(window);
    if (window) ANativeWindow_release(window);
    return ret;
}

int NdkCameraWindow::set_window_locked(ANativeWindow* _win)
{
    if (!_win) set_output_ready(false);
    {
        std::lock_guard<std::mutex> guard(window_mutex);
        if (_win) ANativeWindow_acquire(_win);
        if (win)
        {
            ANativeWindow_release(win);
        }

        win = _win;
        if (!win && sensor_event_queue)
        {
            if (accelerometer_sensor)
                ASensorEventQueue_disableSensor(sensor_event_queue, accelerometer_sensor);
            ASensorManager_destroyEventQueue(sensor_manager, sensor_event_queue);
            sensor_event_queue = 0;
        }
        if (!win && sensor_looper)
        {
            ALooper_release(sensor_looper);
            sensor_looper = 0;
        }
    }
    return _win ? set_output_ready(true) : 0;
}

void NdkCameraWindow::on_image_render(cv::Mat& rgb) const
{
}

void NdkCameraWindow::on_image(const unsigned char* nv21, int nv21_width, int nv21_height) const
{
    std::lock_guard<std::mutex> guard(window_mutex);
    if (!win) return;
    // Read sensor events directly; polling here would reenter Camera1's Java Looper.
    if (sensor_manager && accelerometer_sensor)
    {
        if (!sensor_event_queue)
        {
            if (!sensor_looper)
            {
                sensor_looper = ALooper_forThread();
                if (!sensor_looper) sensor_looper = ALooper_prepare(0);
                // ASensorEventQueue stores only a borrowed Looper pointer. Keep
                // it alive when a camera switch terminates the opening thread.
                if (sensor_looper) ALooper_acquire(sensor_looper);
            }
            if (sensor_looper)
                sensor_event_queue = ASensorManager_createEventQueue(sensor_manager, sensor_looper,
                    ALOOPER_POLL_CALLBACK, sensor_notification, 0);

            if (sensor_event_queue)
                ASensorEventQueue_enableSensor(sensor_event_queue, accelerometer_sensor);
        }

        if (sensor_event_queue)
        {
            ASensorEvent e[8];
            ssize_t num_event = 0;
            while (ASensorEventQueue_hasEvents(sensor_event_queue) == 1)
            {
                num_event = ASensorEventQueue_getEvents(sensor_event_queue, e, 8);
                if (num_event < 0)
                    break;
            }

            if (num_event > 0)
            {
                float acceleration_x = e[num_event - 1].acceleration.x;
                float acceleration_y = e[num_event - 1].acceleration.y;
                float acceleration_z = e[num_event - 1].acceleration.z;
//                 __android_log_print(ANDROID_LOG_WARN, "NdkCameraWindow", "x = %f, y = %f, z = %f", x, y, z);

                if (acceleration_y > 7)
                {
                    accelerometer_orientation = 0;
                }
                if (acceleration_x < -7)
                {
                    accelerometer_orientation = 90;
                }
                if (acceleration_y < -7)
                {
                    accelerometer_orientation = 180;
                }
                if (acceleration_x > 7)
                {
                    accelerometer_orientation = 270;
                }
            }
        }
    }

    // roi crop and rotate nv21
    int nv21_roi_x = 0;
    int nv21_roi_y = 0;
    int nv21_roi_w = 0;
    int nv21_roi_h = 0;
    int roi_x = 0;
    int roi_y = 0;
    int roi_w = 0;
    int roi_h = 0;
    int rotate_type = 0;
    int render_w = 0;
    int render_h = 0;
    int render_rotate_type = 0;
    {
        int win_w = ANativeWindow_getWidth(win);
        int win_h = ANativeWindow_getHeight(win);
        if (win_w < 2 || win_h < 2) return;

        if (accelerometer_orientation == 90 || accelerometer_orientation == 270)
        {
            std::swap(win_w, win_h);
        }

        const int final_orientation = (camera_orientation + accelerometer_orientation) % 360;

        if (final_orientation == 0 || final_orientation == 180)
        {
            if (win_w * nv21_height > win_h * nv21_width)
            {
                roi_w = nv21_width;
                roi_h = (nv21_width * win_h / win_w) / 2 * 2;
                roi_x = 0;
                roi_y = ((nv21_height - roi_h) / 2) / 2 * 2;
            }
            else
            {
                roi_h = nv21_height;
                roi_w = (nv21_height * win_w / win_h) / 2 * 2;
                roi_x = ((nv21_width - roi_w) / 2) / 2 * 2;
                roi_y = 0;
            }

            nv21_roi_x = roi_x;
            nv21_roi_y = roi_y;
            nv21_roi_w = roi_w;
            nv21_roi_h = roi_h;
        }
        if (final_orientation == 90 || final_orientation == 270)
        {
            if (win_w * nv21_width > win_h * nv21_height)
            {
                roi_w = nv21_height;
                roi_h = (nv21_height * win_h / win_w) / 2 * 2;
                roi_x = 0;
                roi_y = ((nv21_width - roi_h) / 2) / 2 * 2;
            }
            else
            {
                roi_h = nv21_width;
                roi_w = (nv21_width * win_w / win_h) / 2 * 2;
                roi_x = ((nv21_height - roi_w) / 2) / 2 * 2;
                roi_y = 0;
            }

            nv21_roi_x = roi_y;
            nv21_roi_y = roi_x;
            nv21_roi_w = roi_h;
            nv21_roi_h = roi_w;
        }

        if (camera_facing == 0)
        {
            if (camera_orientation == 0 && accelerometer_orientation == 0)
            {
                rotate_type = 2;
            }
            if (camera_orientation == 0 && accelerometer_orientation == 90)
            {
                rotate_type = 7;
            }
            if (camera_orientation == 0 && accelerometer_orientation == 180)
            {
                rotate_type = 4;
            }
            if (camera_orientation == 0 && accelerometer_orientation == 270)
            {
                rotate_type = 5;
            }
            if (camera_orientation == 90 && accelerometer_orientation == 0)
            {
                rotate_type = 5;
            }
            if (camera_orientation == 90 && accelerometer_orientation == 90)
            {
                rotate_type = 2;
            }
            if (camera_orientation == 90 && accelerometer_orientation == 180)
            {
                rotate_type = 7;
            }
            if (camera_orientation == 90 && accelerometer_orientation == 270)
            {
                rotate_type = 4;
            }
            if (camera_orientation == 180 && accelerometer_orientation == 0)
            {
                rotate_type = 4;
            }
            if (camera_orientation == 180 && accelerometer_orientation == 90)
            {
                rotate_type = 5;
            }
            if (camera_orientation == 180 && accelerometer_orientation == 180)
            {
                rotate_type = 2;
            }
            if (camera_orientation == 180 && accelerometer_orientation == 270)
            {
                rotate_type = 7;
            }
            if (camera_orientation == 270 && accelerometer_orientation == 0)
            {
                rotate_type = 7;
            }
            if (camera_orientation == 270 && accelerometer_orientation == 90)
            {
                rotate_type = 4;
            }
            if (camera_orientation == 270 && accelerometer_orientation == 180)
            {
                rotate_type = 5;
            }
            if (camera_orientation == 270 && accelerometer_orientation == 270)
            {
                rotate_type = 2;
            }
        }
        else
        {
            if (final_orientation == 0)
            {
                rotate_type = 1;
            }
            if (final_orientation == 90)
            {
                rotate_type = 6;
            }
            if (final_orientation == 180)
            {
                rotate_type = 3;
            }
            if (final_orientation == 270)
            {
                rotate_type = 8;
            }
        }

        if (accelerometer_orientation == 0)
        {
            render_w = roi_w;
            render_h = roi_h;
            render_rotate_type = 1;
        }
        if (accelerometer_orientation == 90)
        {
            render_w = roi_h;
            render_h = roi_w;
            render_rotate_type = 8;
        }
        if (accelerometer_orientation == 180)
        {
            render_w = roi_w;
            render_h = roi_h;
            render_rotate_type = 3;
        }
        if (accelerometer_orientation == 270)
        {
            render_w = roi_h;
            render_h = roi_w;
            render_rotate_type = 6;
        }
    }

    if (roi_w < 2 || roi_h < 2) return;

    // crop and rotate nv21
    cv::Mat nv21_croprotated(roi_h + roi_h / 2, roi_w, CV_8UC1);
    {
        const unsigned char* srcY = nv21 + nv21_roi_y * nv21_width + nv21_roi_x;
        unsigned char* dstY = nv21_croprotated.data;
        ncnn::kanna_rotate_c1(srcY, nv21_roi_w, nv21_roi_h, nv21_width, dstY, roi_w, roi_h, roi_w, rotate_type);

        const unsigned char* srcUV = nv21 + nv21_width * nv21_height + nv21_roi_y * nv21_width / 2 + nv21_roi_x;
        unsigned char* dstUV = nv21_croprotated.data + roi_w * roi_h;
        ncnn::kanna_rotate_c2(srcUV, nv21_roi_w / 2, nv21_roi_h / 2, nv21_width, dstUV, roi_w / 2, roi_h / 2, roi_w, rotate_type);
    }

    // nv21_croprotated to rgb
    cv::Mat rgb(roi_h, roi_w, CV_8UC3);
    ncnn::yuv420sp2rgb(nv21_croprotated.data, roi_w, roi_h, rgb.data);

    on_image_render(rgb);

    // rotate to native window orientation
    cv::Mat rgb_render(render_h, render_w, CV_8UC3);
    ncnn::kanna_rotate_c3(rgb.data, roi_w, roi_h, rgb_render.data, render_w, render_h, render_rotate_type);

    if (ANativeWindow_setBuffersGeometry(win, render_w, render_h, WINDOW_FORMAT_RGBA_8888) != 0) return;

    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(win, &buf, NULL) != 0) return;

    // scale to target size
    if ((buf.format == WINDOW_FORMAT_RGBA_8888 || buf.format == WINDOW_FORMAT_RGBX_8888)
        && buf.width >= render_w && buf.height >= render_h)
    {
        for (int y = 0; y < render_h; y++)
        {
            const unsigned char* ptr = rgb_render.ptr<const unsigned char>(y);
            unsigned char* outptr = (unsigned char*)buf.bits + buf.stride * 4 * y;

            int x = 0;
#if __ARM_NEON
            for (; x + 7 < render_w; x += 8)
            {
                uint8x8x3_t _rgb = vld3_u8(ptr);
                uint8x8x4_t _rgba;
                _rgba.val[0] = _rgb.val[0];
                _rgba.val[1] = _rgb.val[1];
                _rgba.val[2] = _rgb.val[2];
                _rgba.val[3] = vdup_n_u8(255);
                vst4_u8(outptr, _rgba);

                ptr += 24;
                outptr += 32;
            }
#endif // __ARM_NEON
            for (; x < render_w; x++)
            {
                outptr[0] = ptr[0];
                outptr[1] = ptr[1];
                outptr[2] = ptr[2];
                outptr[3] = 255;

                ptr += 3;
                outptr += 4;
            }
        }
    }

    ANativeWindow_unlockAndPost(win);
}
