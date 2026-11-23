package tanhlab.tanhlib.miditest;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;

import java.util.ArrayList;
import java.util.List;

/**
 * Drives thl::midi::DeviceManager on a device or emulator, without MIDI hardware: the
 * inputs are the app's own virtual devices (Midi1Service, UmpService).
 *
 * adb shell am instrument -w tanhlab.tanhlib.miditest/.MidiTest
 *
 * With "-e mode hotplug" it instead waits for a device to arrive and to leave again: the
 * plug app (plug/), which run.sh installs and uninstalls meanwhile.
 */
public final class MidiTest extends Instrumentation {
    private static final String MIDI1_NAME = "tanh-virtual-midi1";
    private static final String UMP_NAME = "tanh-virtual-ump";
    private static final String PLUG_NAME = "tanh-virtual-plug";
    private static final long PLUG_TIMEOUT_MS = 60000;
    private static final long TIMEOUT_MS = 5000;
    private static final long QUIET_MS = 300;

    private final StringBuilder mLog = new StringBuilder();
    private int mFailures = 0;
    private int mChecks = 0;
    private boolean mHotPlugMode = false;

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        mHotPlugMode = arguments != null && "hotplug".equals(arguments.getString("mode"));
        start();
    }

    @Override
    public void onStart() {
        try {
            run();
        } catch (Throwable t) {
            ++mFailures;
            mLog.append("EXCEPTION ").append(android.util.Log.getStackTraceString(t)).append('\n');
        }
        mLog.append(mFailures == 0 ? "OK: " : "FAILED: ").append(mChecks).append(" checks, ")
                .append(mFailures).append(" failed\n");
        Bundle result = new Bundle();
        result.putString(Instrumentation.REPORT_KEY_STREAMRESULT, mLog.toString());
        finish(mFailures == 0 ? Activity.RESULT_OK : Activity.RESULT_CANCELED, result);
    }

    // ── A device as Native.devices() prints it ──────────────────────────────

    private static final class Device {
        int id;
        String name;
        String manufacturer;
        int transport;
        int protocol;
        boolean open;
        int source;
        String key;
    }

    private static List<Device> devices() {
        List<Device> list = new ArrayList<Device>();
        for (String line : Native.devices().split("\n")) {
            if (line.isEmpty()) {
                continue;
            }
            String[] f = line.split("\t", -1);
            Device d = new Device();
            d.id = Integer.parseInt(f[0]);
            d.name = f[1];
            d.manufacturer = f[2];
            d.transport = Integer.parseInt(f[3]);
            d.protocol = Integer.parseInt(f[4]);
            d.open = f[5].equals("1");
            d.source = Integer.parseInt(f[6]);
            d.key = f[7];
            list.add(d);
        }
        return list;
    }

    private static Device find(String product) {
        for (Device d : devices()) {
            if (d.name.contains(product) || d.key.contains(product)) {
                return d;
            }
        }
        return null;
    }

    // ── Small test harness ──────────────────────────────────────────────────

    private interface Condition {
        boolean met();
    }

    private static boolean waitFor(Condition condition, long timeoutMs) {
        long deadline = SystemClock.uptimeMillis() + timeoutMs;
        while (SystemClock.uptimeMillis() < deadline) {
            if (condition.met()) {
                return true;
            }
            SystemClock.sleep(10);
        }
        return condition.met();
    }

    private void check(boolean ok, String what) {
        ++mChecks;
        if (!ok) {
            ++mFailures;
        }
        mLog.append(ok ? "  ok   " : "  FAIL ").append(what).append('\n');
    }

    private void section(String name) {
        mLog.append(name).append('\n');
    }

    /** Packets from `source` that arrive within the timeout, at least `count` of them. */
    private static List<long[]> receive(final int source, final int count, long timeoutMs) {
        final List<long[]> packets = new ArrayList<long[]>();
        waitFor(new Condition() {
            @Override
            public boolean met() {
                long[] values = Native.drain();
                for (int i = 0; i + 6 < values.length; i += 7) {
                    if (values[i + 1] == source) {
                        long[] packet = new long[7];
                        System.arraycopy(values, i, packet, 0, 7);
                        packets.add(packet);
                    }
                }
                return packets.size() >= count;
            }
        }, timeoutMs);
        return packets;
    }

    private static byte[] bytes(int... values) {
        byte[] data = new byte[values.length];
        for (int i = 0; i < values.length; ++i) {
            data[i] = (byte) values[i];
        }
        return data;
    }

    private static String hex(long word) {
        return String.format("0x%08X", word);
    }

    // ── The tests ───────────────────────────────────────────────────────────

    private void run() throws Exception {
        mLog.append("Android API ").append(Build.VERSION.SDK_INT).append('\n');
        boolean hasMidi = getContext().getPackageManager()
                .hasSystemFeature(PackageManager.FEATURE_MIDI);
        check(hasMidi, "the system reports MIDI support");
        if (!hasMidi) {
            return;
        }

        if (mHotPlugMode) {
            hotPlug();
            return;
        }
        midi1();
        withoutAutoOpen();
        if (Build.VERSION.SDK_INT >= 35) {
            ump();
        } else {
            mLog.append("UMP virtual devices need Android 15: skipped\n");
        }
    }

    private Device waitOpen(final String product) {
        waitFor(new Condition() {
            @Override
            public boolean met() {
                Device d = find(product);
                return d != null && d.open;
            }
        }, TIMEOUT_MS);
        return find(product);
    }

    private void midi1() throws Exception {
        section("MIDI 1.0 device, auto-open");
        check(Native.start(false, true), "DeviceManager is running (JavaVM set, MidiBridge bound)");

        Device device = waitOpen(MIDI1_NAME);
        check(device != null, "the virtual device is listed");
        if (device == null) {
            mLog.append("    devices: ").append(Native.devices().replace('\n', ';')).append('\n');
            Native.stop();
            return;
        }
        check(device.open, "it was opened automatically");
        check(device.transport == 4, "transport is Virtual (got " + device.transport + ")");
        check(device.protocol == 0, "protocol is Midi1 (got " + device.protocol + ")");
        check(device.manufacturer.equals("tanh-lab"),
                "manufacturer '" + device.manufacturer + "'");
        check(!device.key.isEmpty(), "persistent key '" + device.key + "'");
        check(find(UMP_NAME) == null, "a Midi1 manager does not list the UMP device");

        check(waitFor(new Condition() {
            @Override
            public boolean met() {
                return Midi1Service.sInstance != null;
            }
        }, TIMEOUT_MS), "opening started the virtual device's service");

        // A note-on with Android's own timestamp.
        long stamp = System.nanoTime();
        Midi1Service.send(bytes(0x90, 60, 100), stamp);
        List<long[]> packets = receive(device.source, 1, TIMEOUT_MS);
        check(packets.size() == 1, "one packet for one note-on (got " + packets.size() + ")");
        if (packets.size() == 1) {
            long[] p = packets.get(0);
            check(p[2] == 1 && p[3] == 0x20903C64L, "it is a type 2 note-on: " + hex(p[3]));
            check(p[0] == stamp, "it carries the sender's timestamp");
        }

        // Running status, and a message cut in two by the transport.
        Midi1Service.send(bytes(0x90, 61), System.nanoTime());
        Midi1Service.send(bytes(101, 62, 102, 0xF8), System.nanoTime());
        packets = receive(device.source, 3, TIMEOUT_MS);
        check(packets.size() == 3, "split message + running status + clock = 3 packets (got "
                + packets.size() + ")");
        if (packets.size() == 3) {
            check(packets.get(0)[3] == 0x20903D65L, "first: " + hex(packets.get(0)[3]));
            check(packets.get(1)[3] == 0x20903E66L, "second: " + hex(packets.get(1)[3]));
            check(packets.get(2)[3] == 0x10F80000L, "third: " + hex(packets.get(2)[3]));
        }

        // Without a timestamp the arrival time is "now" on the clock of uptime_ns().
        long before = Native.uptimeNs();
        Midi1Service.send(bytes(0x80, 60, 0), 0);
        packets = receive(device.source, 1, TIMEOUT_MS);
        long after = Native.uptimeNs();
        check(packets.size() == 1 && packets.get(0)[0] >= before && packets.get(0)[0] <= after,
                "an unstamped message is stamped on arrival");
        check(Math.abs(System.nanoTime() - Native.uptimeNs()) < 50000000L,
                "uptime_ns() and System.nanoTime() are the same clock");

        // System Exclusive stays out by default; the note after it arrives.
        Midi1Service.send(bytes(0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7, 0x90, 64, 1), System.nanoTime());
        packets = receive(device.source, 1, TIMEOUT_MS);
        check(packets.size() == 1 && packets.get(0)[3] == 0x20904001L,
                "SysEx is filtered, the note after it arrives");

        check(Native.dropped() == 0, "nothing was dropped");
        Native.stop();
        check(Native.devices().isEmpty(), "a stopped manager lists nothing");
    }

    private void hotPlug() throws Exception {
        section("Hot-plug (another app with a virtual device is installed, then removed)");
        check(Native.start(false, true), "DeviceManager is running");
        check(find(PLUG_NAME) == null, "the plug device is not there at the start");
        final int changes = Native.changes();

        boolean arrived = waitFor(new Condition() {
            @Override
            public boolean met() {
                return find(PLUG_NAME) != null;
            }
        }, PLUG_TIMEOUT_MS);
        check(arrived, "a device that is plugged in gets listed");
        if (!arrived) {
            Native.stop();
            return;
        }
        check(Native.changes() > changes, "the change callback fired for the arrival");
        Device device = waitOpen(PLUG_NAME);
        check(device != null && device.open, "it is opened automatically");
        if (device != null) {
            List<long[]> packets = receive(device.source, 1, TIMEOUT_MS);
            check(!packets.isEmpty() && packets.get(0)[3] == 0x20904A64L,
                    "and delivers what it plays");
        }

        final int afterArrival = Native.changes();
        boolean left = waitFor(new Condition() {
            @Override
            public boolean met() {
                return find(PLUG_NAME) == null;
            }
        }, PLUG_TIMEOUT_MS);
        check(left, "a device that is pulled out leaves the list");
        check(Native.changes() > afterArrival, "the change callback fired for the removal");

        // The manager is still in working order afterwards.
        Device own = waitOpen(MIDI1_NAME);
        check(own != null && own.open, "the other device is unaffected");
        Native.stop();
    }

    private void withoutAutoOpen() throws Exception {
        section("Without auto-open");
        Native.start(false, false);
        waitFor(new Condition() {
            @Override
            public boolean met() {
                return find(MIDI1_NAME) != null;
            }
        }, TIMEOUT_MS);
        final Device device = find(MIDI1_NAME);
        check(device != null && !device.open, "listed but not open");
        if (device == null) {
            Native.stop();
            return;
        }

        check(Native.open(device.id), "open_input() accepts the request");
        Device opened = waitOpen(MIDI1_NAME);
        check(opened != null && opened.open, "the device opens asynchronously");
        waitFor(new Condition() {
            @Override
            public boolean met() {
                return Midi1Service.sInstance != null;
            }
        }, TIMEOUT_MS);
        Midi1Service.send(bytes(0x90, 72, 100), System.nanoTime());
        check(receive(device.source, 1, TIMEOUT_MS).size() == 1, "an open device delivers");

        check(Native.close(device.id), "close_input() finds the device");
        Device closed = find(MIDI1_NAME);
        check(closed != null && !closed.open, "it is listed as closed");
        // Give the close time to reach the system, then send into the void.
        SystemClock.sleep(QUIET_MS);
        if (Midi1Service.sInstance != null) {
            Midi1Service.send(bytes(0x90, 73, 100), System.nanoTime());
        }
        check(receive(device.source, 1, QUIET_MS).isEmpty(), "a closed device delivers nothing");
        check(!Native.open(0x7FFFFFF0), "open_input() rejects an unknown id");

        // Closed and requested again while the system is still opening it: the answers to
        // the abandoned requests must not be taken for the answer to the last one.
        for (int i = 0; i < 8; ++i) {
            Native.open(device.id);
            Native.close(device.id);
        }
        check(Native.open(device.id), "open_input() accepts a request after open/close in a row");
        Device reopened = waitOpen(MIDI1_NAME);
        check(reopened != null && reopened.open, "the last request opens the device");
        SystemClock.sleep(QUIET_MS);
        Midi1Service.send(bytes(0x90, 74, 100), System.nanoTime());
        check(receive(device.source, 2, 3 * QUIET_MS).size() == 1,
                "and it delivers every message once");
        Device settled = find(MIDI1_NAME);
        check(settled != null && settled.open, "it stays open");
        Native.stop();
    }

    private void ump() throws Exception {
        section("Universal MIDI Packet device (Protocol::Midi2)");
        check(Native.start(true, true), "DeviceManager is running");
        Device device = waitOpen(UMP_NAME);
        check(device != null, "the UMP virtual device is listed");
        if (device == null) {
            mLog.append("    devices: ").append(Native.devices().replace('\n', ';')).append('\n');
            Native.stop();
            return;
        }
        check(device.open, "it was opened automatically");
        check(device.protocol == 1, "protocol is Midi2 (got " + device.protocol + ")");
        Device midi1 = find(MIDI1_NAME);
        check(midi1 != null && midi1.protocol == 0, "the MIDI 1.0 device is listed beside it");

        int listed = 0;
        for (Device d : devices()) {
            if (d.name.contains(UMP_NAME) || d.key.contains(UMP_NAME)) {
                ++listed;
            }
        }
        check(listed == 1, "the UMP device is listed once, not also as a MIDI 1.0 device (got "
                + listed + ")");

        check(waitFor(new Condition() {
            @Override
            public boolean met() {
                return UmpService.sInstance != null;
            }
        }, TIMEOUT_MS), "opening started the UMP service");

        // MIDI 2.0 note-on (two words) and a MIDI 1.0 note-on in UMP (one word), big-endian.
        UmpService.send(bytes(0x40, 0x90, 0x3C, 0x00, 0xC0, 0x00, 0x00, 0x00,
                0x20, 0x91, 0x3E, 0x64), System.nanoTime());
        List<long[]> packets = receive(device.source, 2, TIMEOUT_MS);
        check(packets.size() == 2, "two packets (got " + packets.size() + ")");
        if (packets.size() == 2) {
            long[] first = packets.get(0);
            check(first[2] == 2 && first[3] == 0x40903C00L && first[4] == 0xC0000000L,
                    "MIDI 2.0 note-on: " + hex(first[3]) + " " + hex(first[4]));
            check(packets.get(1)[2] == 1 && packets.get(1)[3] == 0x20913E64L,
                    "MIDI 1.0 note-on in UMP: " + hex(packets.get(1)[3]));
        }
        check(Native.dropped() == 0, "nothing was dropped");
        Native.stop();
    }
}
