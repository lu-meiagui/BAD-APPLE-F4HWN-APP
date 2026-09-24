#!/usr/bin/env python3
import argparse
import sys

SRC_W, SRC_H = 86, 64
SRC_FPS = 30.0
LOGICAL_W_DEFAULT, LOGICAL_H_DEFAULT = 32, 16
N_CTX = 64

F4HWN_TOTAL_BUDGET = 4096
ESTIMATED_DECODER_OVERHEAD = 800

def parse_codeb(path):
    with open(path, "rb") as f:
        data = f.read()
    mp = [[False] * SRC_H for _ in range(SRC_W)]
    frames = []
    i, n = 0, len(data)
    while i < n:
        b = data[i]
        if b == 1:
            frames.append([row[:] for row in mp])
            i += 1
            continue
        if b == 2:
            break
        x = b - 128 - 21
        yb = data[i + 1]
        if yb >= 128:
            y, state = yb - 128, True
        else:
            y, state = yb, False
        if 0 <= x < SRC_W and 0 <= y < SRC_H:
            mp[x][y] = state
        i += 2
    return frames

def downsample_majority(frame, logical_w, logical_h):
    bits = []
    for by in range(logical_h):
        y0 = by * SRC_H // logical_h
        y1 = max((by + 1) * SRC_H // logical_h, y0 + 1)
        for bx in range(logical_w):
            x0 = bx * SRC_W // logical_w
            x1 = max((bx + 1) * SRC_W // logical_w, x0 + 1)
            total = (x1 - x0) * (y1 - y0)
            on = sum(1 for x in range(x0, x1) for y in range(y0, y1) if frame[x][y])
            bits.append(1 if on * 2 >= total else 0)
    return bits

TOP, BOTTOM, MASK32 = 1 << 24, 1 << 16, 0xFFFFFFFF
PROB_BITS, PROB_MAX, PROB_INIT, MOVE_BITS = 12, 4096, 2048, 5

class RangeEncoder:
    def __init__(self):
        self.low, self.range, self.out = 0, MASK32, bytearray()

    def encode_bit(self, bit, prob):
        bound = (self.range >> PROB_BITS) * prob
        if bit == 0:
            self.range = bound
            prob += (PROB_MAX - prob) >> MOVE_BITS
        else:
            self.low = (self.low + bound) & MASK32
            self.range -= bound
            prob -= prob >> MOVE_BITS
        self._norm()
        return prob

    def _norm(self):
        while True:
            if ((self.low ^ (self.low + self.range)) & MASK32) < TOP:
                pass
            elif self.range < BOTTOM:
                self.range = (-self.low) & (BOTTOM - 1)
            else:
                break
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
            self.range = (self.range << 8) & MASK32

    def finish(self):
        for _ in range(4):
            self.out.append((self.low >> 24) & 0xFF)
            self.low = (self.low << 8) & MASK32
        return bytes(self.out)

class RangeDecoder:
    def __init__(self, data):
        self.data, self.pos = data, 0
        self.low, self.range, self.code = 0, MASK32, 0
        for _ in range(4):
            self.code = ((self.code << 8) | self._byte()) & MASK32

    def _byte(self):
        b = self.data[self.pos] if self.pos < len(self.data) else 0
        self.pos += 1
        return b

    def decode_bit(self, prob):
        bound = (self.range >> PROB_BITS) * prob
        v = (self.code - self.low) & MASK32
        if v < bound:
            bit, self.range = 0, bound
            prob += (PROB_MAX - prob) >> MOVE_BITS
        else:
            bit = 1
            self.low = (self.low + bound) & MASK32
            self.range -= bound
            prob -= prob >> MOVE_BITS
        self._norm()
        return bit, prob

    def _norm(self):
        while True:
            if ((self.low ^ (self.low + self.range)) & MASK32) < TOP:
                pass
            elif self.range < BOTTOM:
                self.range = (-self.low) & (BOTTOM - 1)
            else:
                break
            self.code = ((self.code << 8) | self._byte()) & MASK32
            self.low = (self.low << 8) & MASK32
            self.range = (self.range << 8) & MASK32

def _get(f, w, h, x, y):
    if f is None or x < 0 or x >= w or y < 0 or y >= h:
        return 0
    return f[y * w + x]

def context(diff, prev_diff, w, h, x, y):
    spatial = (_get(diff, w, h, x - 1, y) * 32 + _get(diff, w, h, x, y - 1) * 16 +
               _get(diff, w, h, x - 1, y - 1) * 8 + _get(diff, w, h, x + 1, y - 1) * 4)
    exact, nearby = 0, 0
    if prev_diff is not None:
        exact = _get(prev_diff, w, h, x, y) * 2
        nearby = 1 if (_get(prev_diff, w, h, x - 1, y) or _get(prev_diff, w, h, x + 1, y) or
                        _get(prev_diff, w, h, x, y - 1) or _get(prev_diff, w, h, x, y + 1)) else 0
    return spatial + exact + nearby

def encode_video(diffs, w, h):
    enc = RangeEncoder()
    probs = [PROB_INIT] * N_CTX
    prev_diff = None
    for diff in diffs:
        for y in range(h):
            for x in range(w):
                bit = diff[y * w + x]
                ctx = context(diff, prev_diff, w, h, x, y)
                probs[ctx] = enc.encode_bit(bit, probs[ctx])
        prev_diff = diff
    return enc.finish()

def decode_video(data, n_frames, w, h):
    dec = RangeDecoder(data)
    probs = [PROB_INIT] * N_CTX
    diffs, prev_diff = [], None
    for _ in range(n_frames):
        diff = [0] * (w * h)
        for y in range(h):
            for x in range(w):
                ctx = context(diff, prev_diff, w, h, x, y)
                bit, probs[ctx] = dec.decode_bit(probs[ctx])
                diff[y * w + x] = bit
        diffs.append(diff)
        prev_diff = diff
    return diffs

def write_c_header(path, data, logical_w, logical_h, num_frames, array_name):
    with open(path, "w") as f:
        f.write("#ifndef BAD_APPLE_DATA_H\n#define BAD_APPLE_DATA_H\n#include <stdint.h>\n\n")
        f.write(f"#define BA_SRC_LOGICAL_W {logical_w}\n#define BA_SRC_LOGICAL_H {logical_h}\n")
        f.write(f"#define BA_SRC_NUM_FRAMES {num_frames}\n")
        f.write(f"#define {array_name.upper()}_LEN {len(data)}\n\n")
        f.write(f"static const uint8_t {array_name}[{array_name.upper()}_LEN] = {{\n")
        for i in range(0, len(data), 16):
            f.write(", ".join(str(x) for x in data[i:i + 16]) + ",\n")
        f.write("};\n\n#endif\n")

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("codeb")
    ap.add_argument("--out", default="bad_apple")
    ap.add_argument("--fps", type=float, default=4.0)
    ap.add_argument("--logical-w", type=int, default=LOGICAL_W_DEFAULT)
    ap.add_argument("--logical-h", type=int, default=LOGICAL_H_DEFAULT)
    ap.add_argument("--start-frame", type=int, default=0)
    ap.add_argument("--num-frames", type=int, default=None)
    ap.add_argument("--array-name", default="bad_apple_data")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--budget-kb", type=float, default=None)
    args = ap.parse_args()

    print("Parseando .codeb...")
    src_frames = parse_codeb(args.codeb)
    print(f"Frames en el .codeb: {len(src_frames)} (~{len(src_frames)/SRC_FPS:.1f}s a {SRC_FPS} fps)")

    end = len(src_frames) if args.num_frames is None else min(len(src_frames), args.start_frame + args.num_frames)
    src_frames = src_frames[args.start_frame:end]
    if not src_frames:
        sys.exit("Rango de frames vacío.")

    step = max(1, round(SRC_FPS / args.fps))
    sampled = src_frames[::step]
    logical_frames = [downsample_majority(f, args.logical_w, args.logical_h) for f in sampled]
    w, h = args.logical_w, args.logical_h
    bits_per_frame = w * h

    if logical_frames:
        logical_frames[0] = [1] * bits_per_frame

    prev = [0] * bits_per_frame
    diffs = []
    for f in logical_frames:
        diffs.append([a ^ b for a, b in zip(f, prev)])
        prev = f

    print(f"Codificando {len(diffs)} frames con range coder...")
    encoded = encode_video(diffs, w, h)

    if args.selftest:
        decoded = decode_video(encoded, len(diffs), w, h)
        if decoded != diffs:
            sys.exit("SELFTEST FALLÓ: round-trip no bit-exacto.")
        print("selftest OK: round-trip bit-exacto")

    bin_path, h_path = f"{args.out}.bin", f"{args.out}.h"
    with open(bin_path, "wb") as f:
        f.write(encoded)
    write_c_header(h_path, encoded, w, h, len(logical_frames), args.array_name)

    raw_size = len(diffs) * bits_per_frame // 8
    comp_size = len(encoded)
    print(f"\nFrames:            {len(logical_frames)}  (~{len(logical_frames)/args.fps:.1f}s a {args.fps} fps)")
    print(f"Resolución lógica: {w}x{h} ({bits_per_frame} bits/frame)")
    print(f"Tamaño crudo:      {raw_size} B ({raw_size/1024:.2f} KB)")
    print(f"Tamaño range coder:{comp_size} B ({comp_size/1024:.2f} KB)  ratio {raw_size/comp_size:.2f}x")
    print(f"Bytes/frame prom.: {comp_size/len(logical_frames):.2f}")
    print(f"Salida: {bin_path}, {h_path}")

    if args.budget_kb is not None and comp_size > args.budget_kb * 1024:
        print(f"\n⚠️  {comp_size} B supera tu presupuesto de {args.budget_kb} KB.")

    headroom = F4HWN_TOTAL_BUDGET - ESTIMATED_DECODER_OVERHEAD
    print(f"\n--- Chequeo contra F4HWN (4096 B totales, overhead decoder RC ~{ESTIMATED_DECODER_OVERHEAD}B estimado) ---")
    if comp_size > headroom:
        print(f"❌ NO entra ({comp_size - headroom} B de más, según la estimación).")
    else:
        print(f"✅ Entra según la estimación (sobran ~{headroom - comp_size} B) -- confirmar con build.sh real.")

if __name__ == "__main__":
    main()
