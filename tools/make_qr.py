"""Printable QR codes for the apron: PNG + SVG, normal and mirrored (iron-on)."""
import qrcode, qrcode.image.svg
from PIL import ImageOps

URL = "https://kevinniechen.github.io/bear-clock/"
q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_H, box_size=40, border=4)
q.add_data(URL)
q.make(fit=True)
img = q.make_image(fill_color="black", back_color="white").convert("RGB")
img.save("qr/guest-qr.png", dpi=(300, 300))
ImageOps.mirror(img).save("qr/guest-qr-MIRRORED-for-iron-on.png", dpi=(300, 300))
q.make_image(image_factory=qrcode.image.svg.SvgPathImage).save("qr/guest-qr.svg")
print(q.version, img.size)
