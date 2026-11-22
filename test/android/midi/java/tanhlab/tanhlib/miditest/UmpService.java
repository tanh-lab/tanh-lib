package tanhlab.tanhlib.miditest;

import android.annotation.TargetApi;
import android.media.midi.MidiReceiver;
import android.media.midi.MidiUmpDeviceService;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** A virtual Universal MIDI Packet device (Android 15 and later). */
@TargetApi(35)
public final class UmpService extends MidiUmpDeviceService {
    static volatile UmpService sInstance;

    @Override
    public void onCreate() {
        super.onCreate();
        sInstance = this;
    }

    @Override
    public void onDestroy() {
        sInstance = null;
        super.onDestroy();
    }

    @Override
    public List<MidiReceiver> onGetInputPortReceivers() {
        // Ports are bidirectional: one receiver for the port's input side, which the test
        // does not use.
        List<MidiReceiver> receivers = new ArrayList<MidiReceiver>();
        receivers.add(new MidiReceiver() {
            @Override
            public void onSend(byte[] data, int offset, int count, long timestamp) {
            }
        });
        return receivers;
    }

    /** Sends complete packets, each 32-bit word in network byte order. */
    static void send(byte[] data, long timestamp) throws IOException {
        sInstance.getOutputPortReceivers().get(0).send(data, 0, data.length, timestamp);
    }
}
