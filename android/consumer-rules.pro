# tanh-lib MIDI (tanh::Midi): the native backend finds this class by name and binds its
# native methods with RegisterNatives, so R8 must neither rename nor strip it.
-keep class io.github.tanhlab.midi.MidiBridge { *; }
-keep class io.github.tanhlab.midi.MidiBridge$* { *; }
