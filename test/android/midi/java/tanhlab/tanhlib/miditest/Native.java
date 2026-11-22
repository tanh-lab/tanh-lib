package tanhlab.tanhlib.miditest;

/** The test's window onto thl::midi::DeviceManager (native/midi_test_jni.cpp). */
final class Native {
    static {
        // JNI_OnLoad calls thl::midi::set_android_java_vm(), as an app has to.
        System.loadLibrary("miditest");
    }

    private Native() {
    }

    /** Creates the DeviceManager; returns DeviceManager::running(). */
    static native boolean start(boolean midi2, boolean autoOpen);

    static native void stop();

    /** One line per input: id, name, manufacturer, transport, protocol, open, source, key (tabs). */
    static native String devices();

    static native boolean open(int id);

    static native boolean close(int id);

    /** Drains the queue: 7 values per packet — time, source, size, word 0..3. */
    static native long[] drain();

    /** Number of device change callbacks so far. */
    static native int changes();

    static native long dropped();

    static native long uptimeNs();
}
