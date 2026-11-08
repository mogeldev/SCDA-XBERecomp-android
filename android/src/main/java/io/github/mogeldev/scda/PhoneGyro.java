package io.github.mogeldev.scda;

import android.app.Activity;
import android.app.Application;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;

// A zero-size lifecycle observer, independent of touch-overlay visibility. Sensor callbacks run on the UI thread.
public final class PhoneGyro extends View implements SensorEventListener, Application.ActivityLifecycleCallbacks {
    static native boolean nativeWanted();
    static native void nativeReset(boolean active);
    static native void nativeSample(boolean accel, float x, float y, float z, long ns, int rotation);

    private final Activity activity;
    private final SensorManager manager;
    private final Sensor gyro, accel;
    private boolean resumed = true, running, failed;
    private final Runnable poll = this::poll;

    private PhoneGyro(Activity activity) {
        super(activity);
        this.activity = activity;
        manager = (SensorManager) activity.getSystemService(Activity.SENSOR_SERVICE);
        gyro = manager == null ? null : manager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        accel = manager == null ? null : manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
    }

    public static void attach(Activity activity) {
        activity.runOnUiThread(() -> {
            PhoneGyro view = new PhoneGyro(activity);
            activity.getApplication().registerActivityLifecycleCallbacks(view);
            activity.addContentView(view, new ViewGroup.LayoutParams(0, 0));
        });
    }

    private void stop() {
        if (manager != null) manager.unregisterListener(this);
        running = false;
        nativeReset(false);
    }

    private void poll() {
        boolean wanted = nativeWanted() && resumed && hasWindowFocus() && isAttachedToWindow();
        if (!wanted) {
            if (running) stop();
            failed = false;
        } else if (!running && !failed) {
            // 100 Hz, no batching and no high-rate sensor permission required.
            nativeReset(true);
            running = gyro != null && accel != null
                && manager.registerListener(this, gyro, 10000)
                && manager.registerListener(this, accel, 10000);
            if (!running) {
                stop();
                failed = true;
                Log.w("SCDA", "Phone gyro unavailable: gyroscope and accelerometer are required");
            }
        }
        removeCallbacks(poll);
        if (isAttachedToWindow()) postDelayed(poll, 100);
    }

    @Override public void onSensorChanged(SensorEvent event) {
        if (!running) return;
        nativeSample(event.sensor.getType() == Sensor.TYPE_ACCELEROMETER,
            event.values[0], event.values[1], event.values[2], event.timestamp,
            activity.getWindowManager().getDefaultDisplay().getRotation());
    }
    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) stop();
        poll();
    }
    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        poll();
    }
    @Override protected void onDetachedFromWindow() {
        removeCallbacks(poll);
        stop();
        super.onDetachedFromWindow();
    }
    @Override public void onActivityPaused(Activity a) {
        if (a == activity) { resumed = false; stop(); }
    }
    @Override public void onActivityResumed(Activity a) {
        if (a == activity) { resumed = true; poll(); }
    }
    @Override public void onActivityDestroyed(Activity a) {
        if (a == activity) {
            removeCallbacks(poll);
            stop();
            activity.getApplication().unregisterActivityLifecycleCallbacks(this);
        }
    }
    @Override public void onActivityCreated(Activity a, Bundle state) {}
    @Override public void onActivityStarted(Activity a) {}
    @Override public void onActivityStopped(Activity a) {}
    @Override public void onActivitySaveInstanceState(Activity a, Bundle state) {}
}
