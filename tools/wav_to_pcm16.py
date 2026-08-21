"""Convert a WAV (any bit depth) to 16-bit PCM WAV for ndsp."""
import sys
import wave

def main(src, dst):
    with wave.open(src, "rb") as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        print(f"src: {ch}ch {sw*8}bit {rate}Hz {n} frames")
        raw = w.readframes(n)

    if sw == 2:
        out = raw
    elif sw == 3:
        out = bytearray(len(raw) // 3 * 2)
        j = 0
        for i in range(0, len(raw), 3):
            v = int.from_bytes(raw[i:i+3], "little", signed=True) >> 8
            out[j] = v & 0xFF
            out[j+1] = (v >> 8) & 0xFF
            j += 2
    elif sw == 1:
        out = bytearray()
        for b in raw:
            v = (b - 128) << 8
            out.append(v & 0xFF)
            out.append((v >> 8) & 0xFF)
    else:
        sys.exit(f"unsupported sample width {sw}")

    with wave.open(dst, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(out)
    print(f"dst: {ch}ch 16bit {rate}Hz, {len(out)} bytes -> {dst}")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
