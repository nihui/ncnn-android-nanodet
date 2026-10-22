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

#ifndef NDKCAMERA_H
#define NDKCAMERA_H

#include <android/looper.h>
#include <android/native_window.h>
#include <android/sensor.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <jni.h>
#include <mutex>
#include <atomic>

#include <opencv2/core/core.hpp>

class LegacyCamera;
class CameraRecovery;

class NdkCamera
{
public:
    NdkCamera();
    virtual ~NdkCamera();

    // Call from JNI_OnLoad/OnUnload. Include com.tencent.ndkcamera.CameraCallback.
    static int initialize(JavaVM* vm);
    static void uninitialize(JavaVM* vm);

    // facing 0=front 1=back -1=resume the last facing; 0 on success.
    int open(int camera_facing = 0);
    // Close the old camera before opening the other facing. Restore it on failure.
    int switch_camera();
    void close();

    // Optional lifecycle control; existing open/close callers need not use it.
    int set_active(bool active);
    int set_permission(bool granted);

    virtual void on_image(const cv::Mat& rgb) const;

    virtual void on_image(const unsigned char* nv21, int nv21_width, int nv21_height) const;

public:
    int camera_facing;
    int camera_orientation;

protected:
    int set_output_ready(bool ready);

private:
    friend class LegacyCamera;
    friend class CameraRecovery;
    int update_camera();
    int open_locked(int facing);
    int open_ndk();
    int open_legacy();
    void close_locked();
    void close_ndk();
    static void onDisconnected(void* context, ACameraDevice* device);
    static void onError(void* context, ACameraDevice* device, int error);
    static void onImageAvailable(void* context, AImageReader* reader);

    struct DeviceCallbackContext
    {
        NdkCamera* camera;
        unsigned long long generation;
    } device_callback_context;
    CameraRecovery* recovery;
    unsigned long long camera_generation;
    std::mutex camera_mutex;
    bool camera_managed;
    bool camera_active;
    bool camera_permission;
    bool camera_output_ready;
    bool camera_open;
    std::mutex image_mutex;
    std::atomic<bool> accepting_images;
    LegacyCamera* legacy_camera;
    ACameraManager* camera_manager;
    ACameraDevice* camera_device;
    AImageReader* image_reader;
    ANativeWindow* image_reader_surface;
    ACameraOutputTarget* image_reader_target;
    ACaptureRequest* capture_request;
    ACaptureSessionOutputContainer* capture_session_output_container;
    ACaptureSessionOutput* capture_session_output;
    ACameraCaptureSession* capture_session;
};

class NdkCameraWindow : public NdkCamera
{
public:
    NdkCameraWindow();
    virtual ~NdkCameraWindow();

    void set_window(ANativeWindow* win);
    int set_surface(JNIEnv* env, jobject surface);

    virtual void on_image_render(cv::Mat& rgb) const;

    virtual void on_image(const unsigned char* nv21, int nv21_width, int nv21_height) const;

public:
    mutable int accelerometer_orientation;

private:
    int set_window_locked(ANativeWindow* win);
    std::mutex surface_mutex;
    mutable std::mutex window_mutex;
    ASensorManager* sensor_manager;
    mutable ALooper* sensor_looper;
    mutable ASensorEventQueue* sensor_event_queue;
    const ASensor* accelerometer_sensor;
    ANativeWindow* win;
};

#endif // NDKCAMERA_H
