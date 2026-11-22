package tanhlab.tanhlib.midi;

import android.annotation.TargetApi;
import android.content.Context;
import android.content.pm.PackageManager;
import android.media.midi.MidiDevice;
import android.media.midi.MidiDeviceInfo;
import android.media.midi.MidiManager;
import android.media.midi.MidiOutputPort;
import android.media.midi.MidiReceiver;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.concurrent.Executor;

/**
 * The Java half of tanh-lib's Android MIDI input (thl::midi::DeviceManager).
 *
 * android.media.midi can only be received from Java: MidiReceiver and
 * MidiManager.DeviceCallback are abstract classes and openDevice() answers through a
 * listener. This class does that and nothing else — every decision is taken in C++
 * (src/midi/DeviceBackend_android.cpp), which creates one instance per DeviceManager and
 * is called back through the native methods below.
 *
 * Add this folder to the app's Java sources, keep the class from R8 (see
 * android/consumer-rules.pro) and call thl::midi::set_android_java_vm() in JNI_OnLoad.
 *
 * Threads: device callbacks and opening run on this object's own HandlerThread; received
 * data arrives on the thread android.media.midi runs per port. Lock order is this
 * object's monitor first, then the C++ side's mutex; the methods C++ calls while holding
 * its mutex (open, close) therefore only post to the handler.
 */
public final class MidiBridge {
    private static final class OpenDevice {
        MidiDevice device;
        final ArrayList<MidiOutputPort> ports = new ArrayList<MidiOutputPort>();
    }

    private final long mHandle;
    private final boolean mWantUmp;
    private final MidiManager mManager;
    private final HandlerThread mThread = new HandlerThread("thl.midi");
    private final Handler mHandler;

    private final HashMap<Integer, MidiDeviceInfo> mDevices = new HashMap<Integer, MidiDeviceInfo>();
    private final HashMap<Integer, Boolean> mIsUmp = new HashMap<Integer, Boolean>();
    private final HashMap<Integer, OpenDevice> mOpen = new HashMap<Integer, OpenDevice>();
    private final ArrayList<MidiManager.DeviceCallback> mCallbacks =
            new ArrayList<MidiManager.DeviceCallback>();
    private boolean mStopped = false;

    /**
     * Returns null when the device has no MIDI support. wantUmp also lists devices of the
     * Universal MIDI Packet transport (API 33 and later).
     */
    public static MidiBridge create(Context context, long handle, boolean wantUmp) {
        if (!context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_MIDI)) {
            return null;
        }
        MidiManager manager = (MidiManager) context.getSystemService(Context.MIDI_SERVICE);
        if (manager == null) {
            return null;
        }
        return new MidiBridge(manager, handle, wantUmp);
    }

    private MidiBridge(MidiManager manager, long handle, boolean wantUmp) {
        mManager = manager;
        mHandle = handle;
        mWantUmp = wantUmp;
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
    }

    /** Reports the devices that are present, on the calling thread, and follows changes. */
    public void start() {
        // Register before listing: a device that appears in between is reported twice and
        // the second report is ignored, rather than missed.
        if (Build.VERSION.SDK_INT >= 33) {
            startWithTransports();
        } else {
            startLegacy();
        }
    }

    @SuppressWarnings("deprecation")
    private void startLegacy() {
        Callback callback = new Callback(false);
        synchronized (this) {
            mCallbacks.add(callback);
        }
        mManager.registerDeviceCallback(callback, mHandler);
        for (MidiDeviceInfo info : mManager.getDevices()) {
            added(info, false);
        }
    }

    @TargetApi(33)
    private void startWithTransports() {
        Executor executor = new Executor() {
            @Override
            public void execute(Runnable command) {
                mHandler.post(command);
            }
        };
        registerTransport(MidiManager.TRANSPORT_MIDI_BYTE_STREAM, false, executor);
        if (mWantUmp) {
            registerTransport(MidiManager.TRANSPORT_UNIVERSAL_MIDI_PACKETS, true, executor);
        }
    }

    @TargetApi(33)
    private void registerTransport(int transport, boolean ump, Executor executor) {
        Callback callback = new Callback(ump);
        synchronized (this) {
            mCallbacks.add(callback);
        }
        mManager.registerDeviceCallback(transport, executor, callback);
        for (MidiDeviceInfo info : mManager.getDevicesForTransport(transport)) {
            added(info, ump);
        }
    }

    /** Opens every output port of the device and delivers what it sends as `slot`. */
    public void open(final int deviceId, final int slot) {
        mHandler.post(new Runnable() {
            @Override
            public void run() {
                doOpen(deviceId, slot);
            }
        });
    }

    public void close(final int deviceId) {
        mHandler.post(new Runnable() {
            @Override
            public void run() {
                synchronized (MidiBridge.this) {
                    closeLocked(deviceId);
                }
            }
        });
    }

    /** Closes everything. Nothing is reported to C++ afterwards. */
    public void stop() {
        synchronized (this) {
            mStopped = true;
            for (MidiManager.DeviceCallback callback : mCallbacks) {
                mManager.unregisterDeviceCallback(callback);
            }
            mCallbacks.clear();
            for (Integer id : new ArrayList<Integer>(mOpen.keySet())) {
                closeLocked(id);
            }
            mDevices.clear();
            mIsUmp.clear();
        }
        mThread.quitSafely();
    }

    private final class Callback extends MidiManager.DeviceCallback {
        private final boolean mUmp;

        Callback(boolean ump) {
            mUmp = ump;
        }

        @Override
        public void onDeviceAdded(MidiDeviceInfo info) {
            added(info, mUmp);
        }

        @Override
        public void onDeviceRemoved(MidiDeviceInfo info) {
            removed(info);
        }
    }

    private final class Receiver extends MidiReceiver {
        private final int mSlot;
        private final int mPort;
        private final boolean mUmp;

        Receiver(int slot, int port, boolean ump) {
            mSlot = slot;
            mPort = port;
            mUmp = ump;
        }

        @Override
        public void onSend(byte[] data, int offset, int count, long timestamp) {
            nativeOnData(mHandle, mSlot, mPort, mUmp, data, offset, count, timestamp);
        }
    }

    private static String property(Bundle properties, String key) {
        String value = properties.getString(key);
        return value == null ? "" : value;
    }

    private synchronized void added(MidiDeviceInfo info, boolean ump) {
        // A device without output ports has nothing to receive from. ("Output" is from the
        // device's point of view: its output is our input.)
        if (mStopped || info.getOutputPortCount() == 0) {
            return;
        }
        int id = info.getId();
        if (mDevices.containsKey(id)) {
            return;
        }
        mDevices.put(id, info);
        mIsUmp.put(id, ump);

        Bundle properties = info.getProperties();
        String name = property(properties, MidiDeviceInfo.PROPERTY_NAME);
        String manufacturer = property(properties, MidiDeviceInfo.PROPERTY_MANUFACTURER);
        String product = property(properties, MidiDeviceInfo.PROPERTY_PRODUCT);
        String serial = property(properties, MidiDeviceInfo.PROPERTY_SERIAL_NUMBER);
        if (name.isEmpty()) {
            name = product;
        }
        // The id changes with every reconnect; this stays.
        String key = manufacturer + "|" + product + "|" + serial + (ump ? "|ump" : "");

        nativeOnDeviceAdded(mHandle, id, name, manufacturer, key, info.getType(), ump,
                info.getOutputPortCount());
    }

    private synchronized void removed(MidiDeviceInfo info) {
        int id = info.getId();
        if (mStopped || mDevices.remove(id) == null) {
            return;
        }
        mIsUmp.remove(id);
        closeLocked(id);
        nativeOnDeviceRemoved(mHandle, id);
    }

    private void doOpen(final int deviceId, final int slot) {
        final MidiDeviceInfo info;
        synchronized (this) {
            info = mDevices.get(deviceId);
            if (mStopped || info == null || mOpen.containsKey(deviceId)) {
                return;
            }
            // Reserve the entry so that a second request does not open the device twice.
            mOpen.put(deviceId, new OpenDevice());
        }
        mManager.openDevice(info, new MidiManager.OnDeviceOpenedListener() {
            @Override
            public void onDeviceOpened(MidiDevice device) {
                opened(deviceId, slot, device);
            }
        }, mHandler);
    }

    private synchronized void opened(int deviceId, int slot, MidiDevice device) {
        OpenDevice open = mOpen.get(deviceId);
        MidiDeviceInfo info = mDevices.get(deviceId);
        if (mStopped || open == null || info == null || device == null) {
            // Closed or unplugged while it was opening, or the system refused.
            closeQuietly(device);
            mOpen.remove(deviceId);
            if (!mStopped && info != null) {
                nativeOnOpened(mHandle, deviceId, false);
            }
            return;
        }

        open.device = device;
        boolean ump = Boolean.TRUE.equals(mIsUmp.get(deviceId));
        for (int port = 0; port < info.getOutputPortCount(); ++port) {
            // Null when another client holds the port.
            MidiOutputPort output = device.openOutputPort(port);
            if (output != null) {
                output.connect(new Receiver(slot, port, ump));
                open.ports.add(output);
            }
        }
        if (open.ports.isEmpty()) {
            closeLocked(deviceId);
            nativeOnOpened(mHandle, deviceId, false);
            return;
        }
        nativeOnOpened(mHandle, deviceId, true);
    }

    private void closeLocked(int deviceId) {
        OpenDevice open = mOpen.remove(deviceId);
        if (open == null) {
            return;
        }
        for (MidiOutputPort port : open.ports) {
            closeQuietly(port);
        }
        closeQuietly(open.device);
    }

    private static void closeQuietly(java.io.Closeable closeable) {
        if (closeable == null) {
            return;
        }
        try {
            closeable.close();
        } catch (IOException ignored) {
            // Nothing to do about a port that fails to close.
        }
    }

    private static native void nativeOnDeviceAdded(long handle, int id, String name,
            String manufacturer, String key, int type, boolean ump, int outputPorts);

    private static native void nativeOnDeviceRemoved(long handle, int id);

    private static native void nativeOnOpened(long handle, int id, boolean ok);

    private static native void nativeOnData(long handle, int slot, int port, boolean ump,
            byte[] data, int offset, int count, long timestamp);
}
