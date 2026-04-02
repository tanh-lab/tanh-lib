# Linux Audio Reference

## Table of Contents
1. [PipeWire Architecture](#pipewire-architecture)
2. [WirePlumber and Routing](#wireplumber-and-routing)
3. [Bluetooth Audio on Linux](#bluetooth-audio-on-linux)
4. [JACK Integration](#jack-integration)
5. [Headless Audio Setups](#headless-audio-setups)
6. [Asahi Linux Specifics](#asahi-linux-specifics)

---

## PipeWire Architecture

PipeWire replaces both PulseAudio and JACK, providing a unified graph for all
audio (and video) streams. On Asahi Linux / Fedora, it's the default.

### Key components:

- **PipeWire daemon** (`pipewire`): Core graph engine
- **PipeWire-pulse** (`pipewire-pulse`): PulseAudio compatibility layer
- **WirePlumber** (`wireplumber`): Session/policy manager (replaces pipewire-media-session)
- **spa plugins**: Backend drivers (ALSA, BlueZ, V4L2)

### Useful commands:

```bash
pw-top                    # Real-time view of nodes, quantum, sample rate
pw-cli ls Node            # List all nodes
pw-cli info <id>          # Detailed info on a node
pactl list short sinks    # List sinks (via PulseAudio compat)
wpctl status              # WirePlumber routing status
wpctl inspect <id>        # Inspect a WirePlumber object
```

### Configuration:

PipeWire config files live in:
- `/usr/share/pipewire/` (defaults, don't edit)
- `/etc/pipewire/` (system overrides)
- `~/.config/pipewire/` (user overrides)

Drop-in fragments go in `pipewire.conf.d/`:
```
~/.config/pipewire/pipewire.conf.d/my-settings.conf
```

**Warning**: A config file containing only comments (no actual configuration) can
cause issues. If you create a debug config, make sure it has real content or
delete it.

### Buffer/quantum settings:

```
# ~/.config/pipewire/pipewire.conf.d/low-latency.conf
context.properties = {
    default.clock.quantum = 256
    default.clock.min-quantum = 64
    default.clock.max-quantum = 1024
    default.clock.rate = 48000
}
```

---

## WirePlumber and Routing

WirePlumber handles automatic routing decisions, including Bluetooth profile
switching between A2DP and HFP.

### Automatic A2DP/HFP switching

WirePlumber automatically switches Bluetooth profiles based on which streams are
active. When a recording stream opens, it may switch to HFP for mic access. When
only playback is active, it stays on A2DP.

### Manual routing:

```bash
# Set default sink
wpctl set-default <sink-id>

# Target a specific sink for playback
pw-play --target <sink-name> file.wav

# Test sound through specific device
speaker-test -D <device> -c 2 -t sine
```

### Graph visualization:

```bash
# GUI tools for viewing/editing the PipeWire graph
qpwgraph    # Qt-based
helvum      # GTK-based
```

---

## Bluetooth Audio on Linux

### BlueZ + PipeWire stack

BlueZ handles the Bluetooth protocol layer. PipeWire's `spa-bluez5` plugin
creates audio nodes for connected devices. WirePlumber manages profile switching.

### Available codecs:

Check what your device negotiated:
```bash
pw-top  # Shows codec in the node info
```

Common codecs: SBC, SBC-XQ, AAC, LDAC, aptX, aptX HD. Availability depends on
both the device and the BlueZ/PipeWire build.

### HFP Gateway retry bug

**Symptom**: Every ~60 seconds, BlueZ logs:
```
Unable to get Hands-Free Voice gateway SDP record: Host is down
```
This causes periodic A2DP glitches as BlueZ disrupts the transport to attempt
HFP Gateway negotiation.

**Fix**: Add to `/etc/bluetooth/main.conf` under `[General]`:
```ini
[General]
Disable=Gateway
```

This disables only the HFP Audio Gateway **role** (phone role). HFP Hands-Free
role (headset role) remains functional — WirePlumber can still switch to HFP when
you manually trigger it or when an app requests mic input.

### PipeWire RAOP sink (AirPlay)

For streaming to AirPort Express or other AirPlay receivers:
```bash
# PipeWire discovers AirPlay devices via Avahi/mDNS automatically
# Check if RAOP module is loaded:
pactl list modules | grep raop
```

---

## JACK Integration

PipeWire provides JACK compatibility, so JACK applications see PipeWire nodes as
JACK ports.

### Using JACK apps with PipeWire:

```bash
# Most JACK apps work transparently via pw-jack wrapper or LD_PRELOAD
pw-jack ardour
pw-jack jack_lsp  # List JACK ports (shows PipeWire nodes)
```

### Shairport Sync with JACK backend:

```bash
./configure --sysconfdir=/etc --with-airplay-2 \
  --with-ssl=openssl --with-avahi --with-soxr \
  --with-systemd --with-jack

# Config:
jack = {
  client_name = "shairport-sync";
  autoconnect_pattern = "system:playback.*";
};
```

### Shairport Sync with PipeWire backend:

```bash
./configure --sysconfdir=/etc --with-airplay-2 \
  --with-ssl=openssl --with-avahi --with-soxr \
  --with-systemd --with-pipe

# Config:
pipe = {
  name = "/tmp/shairport-sync-audio";
};
# Then create a PipeWire source node from the pipe
```

The PipeWire (native) backend is preferred over JACK when routing flexibility is
the goal, since everything lands in the same graph.

---

## Headless Audio Setups

For Raspberry Pi or server-based audio receivers (AirPlay, Bluetooth, Spotify):

### Full headless stack:

```
iPhone  ──AirPlay 2──▶ Shairport Sync ──▶ PipeWire ──▶ DAC/speakers
Android ──Bluetooth──▶ BlueZ + PipeWire ──▶ PipeWire ──▶ DAC/speakers
Spotify ──────────────▶ Raspotify ─────────▶ PipeWire ──▶ DAC/speakers
```

### PipeWire headless requirements:

PipeWire normally runs as a user service. For headless (no login session):

```bash
# Enable lingering so user services start at boot
sudo loginctl enable-linger $USER

# Enable PipeWire as user services
systemctl --user enable pipewire pipewire-pulse wireplumber
```

### Service ordering:

Shairport Sync needs PipeWire running before it starts:
```ini
# In shairport-sync.service override
[Unit]
After=pipewire.service wireplumber.service
```

### Bluetooth auto-pairing (headless):

Configure BlueZ agent as `NoInputNoOutput` so it accepts connections without
confirmation:
```bash
bluetoothctl
agent NoInputNoOutput
default-agent
```

### AirPlay 2 with nqptp:

Shairport Sync 4.x with nqptp provides AirPlay 2 support. nqptp must run as a
system service **before** Shairport Sync starts:
```bash
sudo systemctl enable nqptp
sudo systemctl start nqptp
```

Without nqptp running, Shairport Sync falls back to AirPlay 1 or fails to
advertise as AirPlay 2.

---

## Asahi Linux Specifics

### Audio stack

Asahi Linux on Apple Silicon uses PipeWire with the ALSA backend. The Apple
Silicon audio hardware is supported via the Asahi-specific kernel drivers.

### Common issues:

- **VS Code Insiders**: Needs `--ozone-platform-hint=auto` for proper Wayland/X11
  detection. Configure in `~/.config/Code - Insiders/argv.json` or as a shell alias.
- **ripgrep/jemalloc crash**: VS Code Insiders' bundled ripgrep can crash on 16KB
  page size (aarch64). Known issue.
- **DisplayLink/EVDI**: Needs ARM64-specific kernel module patching for external
  displays.
- **FEX-EMU**: Mesa overlay needs investigation for x86 emulation of graphics apps.

### Testing audio:

```bash
# Quick test
speaker-test -c 2 -t sine

# Through specific device
pw-play --target <sink-name> /usr/share/sounds/freedesktop/stereo/bell.oga

# List sinks
pactl list short sinks
```

### Bluetooth headphones (Bowers & Wilkins Pi5 S2):

These headphones support SBC, SBC-XQ, and AAC — no LDAC or aptX. AAC is the
highest quality available. Verify active codec with `pw-top`.

BLE support is present (for companion app communication) but not for audio
streaming (LE Audio).
