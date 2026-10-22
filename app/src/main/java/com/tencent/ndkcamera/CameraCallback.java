package com.tencent.ndkcamera;

import android.hardware.Camera;

/** Java interface declarations only; both callbacks are implemented in C++. */
@SuppressWarnings("deprecation")
final class CameraCallback implements Camera.PreviewCallback, Camera.ErrorCallback
{
    private final long nativeCamera;

    CameraCallback(long nativeCamera) { this.nativeCamera = nativeCamera; }

    @Override public native void onPreviewFrame(byte[] data, Camera camera);
    @Override public native void onError(int error, Camera camera);
}
