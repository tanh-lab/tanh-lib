package io.github.tanhlab.miditest;

import android.media.midi.MidiDeviceService;
import android.media.midi.MidiReceiver;

import java.io.IOException;

/** A virtual MIDI 1.0 device: the test sends through it what a keyboard would send. */
public final class Midi1Service extends MidiDeviceService {
    static volatile Midi1Service sInstance;

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
    public MidiReceiver[] onGetInputPortReceivers() {
        return new MidiReceiver[0];
    }

    /** Sends on the output port; timestamp 0 sends without one. */
    static void send(byte[] data, long timestamp) throws IOException {
        MidiReceiver out = sInstance.getOutputPortReceivers()[0];
        if (timestamp == 0) {
            out.send(data, 0, data.length);
        } else {
            out.send(data, 0, data.length, timestamp);
        }
    }
}
