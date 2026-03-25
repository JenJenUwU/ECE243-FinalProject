#!/usr/bin/env python3
import sys

try:
    import mido
except ImportError:
    sys.stderr.write(
        "Error: this script requires mido.\n"
        "Install it with:\n"
        "    pip install mido\n"
    )
    sys.exit(1)

# Force UTF-8 stdout/stderr on Windows when possible
try:
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    sys.stderr.reconfigure(encoding="utf-8", newline="\n")
except Exception:
    pass


NOTE_FREQS = [
    8.1758, 8.6620, 9.1770, 9.7227, 10.3009, 10.9134, 11.5623, 12.2499,
    12.9783, 13.7500, 14.5676, 15.4339, 16.3516, 17.3239, 18.3540, 19.4454,
    20.6017, 21.8268, 23.1247, 24.4997, 25.9565, 27.5000, 29.1352, 30.8677,
    32.7032, 34.6478, 36.7081, 38.8909, 41.2034, 43.6535, 46.2493, 48.9994,
    51.9131, 55.0000, 58.2705, 61.7354, 65.4064, 69.2957, 73.4162, 77.7817,
    82.4069, 87.3071, 92.4986, 97.9989, 103.8262, 110.0000, 116.5409, 123.4708,
    130.8128, 138.5913, 146.8324, 155.5635, 164.8138, 174.6141, 184.9972, 195.9977,
    207.6523, 220.0000, 233.0819, 246.9417, 261.6256, 277.1826, 293.6648, 311.1270,
    329.6276, 349.2282, 369.9944, 391.9954, 415.3047, 440.0000, 466.1638, 493.8833,
    523.2511, 554.3653, 587.3295, 622.2540, 659.2551, 698.4565, 739.9888, 783.9909,
    830.6094, 880.0000, 932.3275, 987.7666, 1046.5023, 1108.7305, 1174.6591, 1244.5079,
    1318.5102, 1396.9129, 1479.9777, 1567.9817, 1661.2188, 1760.0000, 1864.6550, 1975.5332,
    2093.0045, 2217.4610, 2349.3181, 2489.0159, 2637.0205, 2793.8259, 2959.9554, 3135.9635,
    3322.4376, 3520.0000, 3729.3101, 3951.0664, 4186.0090, 4434.9221, 4698.6363, 4978.0317,
    5274.0409, 5587.6517, 5919.9108, 6271.9269, 6644.8752, 7040.0000, 7458.6202, 7902.1328,
    8372.0181, 8869.8442, 9397.2726, 9956.0635, 10548.0818, 11175.3034, 11839.8215, 12543.8539
]


def usage():
    sys.stderr.write("Usage:\n")
    sys.stderr.write("    python tools/midi_to_c.py song.mid > midi_data.h\n")


def collect_note_events(mid):
    merged = mido.merge_tracks(mid.tracks)
    ticks_per_beat = mid.ticks_per_beat
    tempo = 500000  # default 120 BPM

    abs_us = 0.0
    active = {}
    out_events = []

    for msg in merged:
        if msg.time:
            abs_us += mido.tick2second(msg.time, ticks_per_beat, tempo) * 1000000.0

        if msg.is_meta:
            if msg.type == "set_tempo":
                tempo = msg.tempo
            continue

        if not hasattr(msg, "channel"):
            continue

        if hasattr(msg, "note"):
            key = (msg.channel, msg.note)
        else:
            continue

        if msg.type == "note_on" and msg.velocity > 0:
            active[key] = {
                "start_us": abs_us,
                "velocity": int(msg.velocity),
                "note": int(msg.note),
            }

        elif msg.type == "note_off" or (msg.type == "note_on" and msg.velocity == 0):
            if key in active:
                note_on = active.pop(key)
                start_ms = int(round(note_on["start_us"] / 1000.0))
                duration_ms = int(round((abs_us - note_on["start_us"]) / 1000.0))
                if duration_ms <= 0:
                    duration_ms = 1

                out_events.append({
                    "start_ms": start_ms,
                    "duration_ms": duration_ms,
                    "note": note_on["note"],
                    "velocity": note_on["velocity"],
                })

    out_events.sort(key=lambda e: (e["start_ms"], e["note"]))
    return out_events


def emit_header(events):
    print("#ifndef MIDI_DATA_H")
    print("#define MIDI_DATA_H")
    print()
    print("#include <stdint.h>")
    print()
    print("typedef struct {")
    print("    uint32_t start_ms;")
    print("    uint32_t duration_ms;")
    print("    uint8_t note;")
    print("    uint8_t velocity;")
    print("} midi_note_event_t;")
    print()
    print("static const float note_freqs[128] = {")
    for i in range(0, 128, 8):
        row = ", ".join("{:.4f}f".format(x) for x in NOTE_FREQS[i:i + 8])
        comma = "," if i + 8 < 128 else ""
        print("    " + row + comma)
    print("};")
    print()
    print("static const midi_note_event_t MIDI_NOTES[] = {")
    if not events:
        print("    {0, 500, 60, 100},")
    else:
        for e in events:
            print("    {{{}, {}, {}, {}}},".format(
                e["start_ms"], e["duration_ms"], e["note"], e["velocity"]
            ))
    print("};")
    print()
    print("static const uint32_t MIDI_NOTE_COUNT = sizeof(MIDI_NOTES) / sizeof(MIDI_NOTES[0]);")
    print()
    print("#endif")


def main():
    if len(sys.argv) != 2:
        usage()
        sys.exit(1)

    midi_path = sys.argv[1]

    try:
        mid = mido.MidiFile(midi_path)
    except Exception as e:
        sys.stderr.write("Failed to read MIDI file '{}': {}\n".format(midi_path, e))
        sys.exit(1)

    events = collect_note_events(mid)
    emit_header(events)


if __name__ == "__main__":
    main()