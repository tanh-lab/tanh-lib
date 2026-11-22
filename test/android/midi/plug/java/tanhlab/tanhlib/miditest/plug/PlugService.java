package tanhlab.tanhlib.miditest.plug;

import android.media.midi.MidiDeviceService;
import android.media.midi.MidiDeviceStatus;
import android.media.midi.MidiReceiver;
import android.os.Handler;
import android.os.Looper;

import java.io.IOException;

/** A virtual device that plays one note, a few times over, once somebody listens. */
public final class PlugService extends MidiDeviceService {
    private static final int REPEATS = 10;
    private static final long INTERVAL_MS = 200;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mSending = false;

    @Override
    public MidiReceiver[] onGetInputPortReceivers() {
        return new MidiReceiver[0];
    }

    @Override
    public void onDeviceStatusChanged(MidiDeviceStatus status) {
        if (status.getOutputPortOpenCount(0) > 0 && !mSending) {
            mSending = true;
            send(REPEATS);
        }
    }

    private void send(final int remaining) {
        if (remaining == 0) {
            mSending = false;
            return;
        }
        try {
            getOutputPortReceivers()[0].send(new byte[] {(byte) 0x90, 74, 100}, 0, 3);
        } catch (IOException ignored) {
            // The listener went away.
        }
        mHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                send(remaining - 1);
            }
        }, INTERVAL_MS);
    }
}
