"""Turns raw 64x32 RGB frames from `sim frames` into an LED-looking GIF."""
import sys
from PIL import Image, ImageDraw

W, H, PX = 64, 32, 8
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
n = len(data) // (W * H * 3)
frames = []
for i in range(0, n, 2):  # 10 fps is plenty for a preview
    raw = data[i * W * H * 3:(i + 1) * W * H * 3]
    img = Image.new("RGB", (W * PX, H * PX), (8, 8, 10))
    d = ImageDraw.Draw(img)
    for y in range(H):
        for x in range(W):
            o = (y * W + x) * 3
            r, g, b = raw[o], raw[o + 1], raw[o + 2]
            c = (max(r, 18), max(g, 18), max(b, 20)) if (r or g or b) else (22, 22, 26)
            d.ellipse([x * PX + 1, y * PX + 1, x * PX + PX - 2, y * PX + PX - 2], fill=c)
    frames.append(img)
frames[0].save(dst, save_all=True, append_images=frames[1:], duration=100, loop=0)
print(f"{len(frames)} frames -> {dst}")
