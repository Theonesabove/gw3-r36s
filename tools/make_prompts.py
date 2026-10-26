#!/usr/bin/env python3
"""Draw R36S-style shoulder prompts (L1, R1, L2, R2) and an original handheld diagram for the Controller
page, in the game's outline style, and embed them in src/prompts.inc. Our own drawings (DejaVu Sans Bold);
they replace the Xbox LB/RB/LT/RT images and the game's controller drawing.
usage: tools/make_prompts.py   (needs Pillow)"""
import io, math, os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
S = 4  # supersampling

def glyph(text, trigger, flip):
    big = Image.new("L", (70 * S, 70 * S), 0)   # drawn on a 70 px grid, scaled to the target size
    d = ImageDraw.Draw(big)
    if trigger:   # L2/R2: taller body with a sloped top edge
        top, bot, l, r = 12 * S, 58 * S, 11 * S, 59 * S
        slope = 8 * S
        pts = [(l, top + slope), (r, top), (r, bot), (l, bot)] if not flip else [(l, top), (r, top + slope), (r, bot), (l, bot)]
        d.line(pts + [pts[0]], fill=255, width=3 * S, joint="curve")
        fs, ty = 22 * S, 39 * S
    else:         # L1/R1: rounded bumper
        d.rounded_rectangle((6 * S, 20 * S, 64 * S, 50 * S), radius=7 * S, outline=255, width=3 * S)
        fs, ty = 21 * S, 35 * S
    f = ImageFont.truetype(FONT, fs)
    d.text((35 * S, ty), text, font=f, fill=255, anchor="mm")
    return big

def la(alpha):
    return Image.merge("LA", (Image.new("L", alpha.size, 255), alpha))

def shadow(alpha):  # the game's "_shadow" twin: a soft glow of the same shape
    g = Image.eval(alpha.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.GaussianBlur(4)), lambda v: v * 3 // 4)
    return Image.composite(alpha, g, alpha)

def save(out, name, img):
    buf = io.BytesIO(); img.save(buf, "PNG", optimize=True)
    out[name] = buf.getvalue()
    img.save(os.path.join(ROOT, "src", "prompts", name))

out = {}
for name, text, trig, flip in [("JoypadShoulder_TL", "L1", False, False), ("JoypadShoulder_TR", "R1", False, False),
                               ("JoypadShoulder_BL", "L2", True, False), ("JoypadShoulder_BR", "R2", True, True)]:
    big = glyph(text, trig, flip)
    # full-size prompt (70 px) and the small one used inside text (53 px, "joypadShoulder_XX_TextInline")
    for fname, size in ((name, 70), ("j" + name[1:] + "_TextInline", 53)):
        a = big.resize((size, size), Image.LANCZOS)
        save(out, fname + ".png", la(a))
        save(out, fname + "_shadow.png", la(shadow(a)))

# ---- Controller page diagram (Config_A_B.png, 912x520): an original portrait handheld of our own design,
# with the R36S's control layout (screen on top, d-pad and face buttons under it, two sticks below, SELECT /
# START at the bottom, shoulder buttons on the top edge), faceted to match the game's geometric look. The game
# prints SUPER / SMARTBOMB / MOVE / SHOOT at the outer ends of the four leader lines (x 315 and 596, y 82 and
# 427), so those ends stay put. The R36S shows this image at about a third of its size, so strokes are heavy.
def diagram():
    W, H, S2 = 912, 520, 2
    big = Image.new("L", (W * S2, H * S2), 0)
    d = ImageDraw.Draw(big)
    sc = lambda pts: [(x * S2, y * S2) for x, y in pts]
    line = 150
    L, R, T, B = 350, 561, 120, 418          # body
    c = 16                                   # chamfer
    body = [(L + c, T), (R - c, T), (R, T + c), (R, B - 34), (R - 22, B), (L + 22, B), (L, B - 34), (L, T + c)]
    d.line(sc(body + [body[0]]), fill=line, width=8 * S2, joint="curve")
    # shoulder buttons on the top edge: two stacked tabs per side (L1 in front, L2 behind)
    for x0 in (358, 501):
        d.polygon(sc([(x0, 108), (x0 + 52, 108), (x0 + 55, 120), (x0 - 3, 120)]), outline=line, width=5 * S2)
        d.polygon(sc([(x0 + 5, 98), (x0 + 47, 98), (x0 + 50, 108), (x0 + 2, 108)]), outline=line, width=4 * S2)
    # screen (4:3) with an inner bezel line
    d.rectangle(sc([(373, 135), (538, 259)]), outline=line, width=6 * S2)
    d.rectangle(sc([(382, 144), (529, 250)]), outline=90, width=3 * S2)
    def ring(cx, cy, r, w): d.ellipse(sc([(cx - r, cy - r), (cx + r, cy + r)]), outline=line, width=w * S2)
    # d-pad (left, under the screen)
    cx, cy, a, t = 392, 296, 18, 7
    plus = [(cx - t, cy - a), (cx + t, cy - a), (cx + t, cy - t), (cx + a, cy - t), (cx + a, cy + t), (cx + t, cy + t),
            (cx + t, cy + a), (cx - t, cy + a), (cx - t, cy + t), (cx - a, cy + t), (cx - a, cy - t), (cx - t, cy - t)]
    d.polygon(sc(plus), outline=line, width=5 * S2)
    # face buttons (right), labelled like the R36S: X top, A right, B bottom, Y left
    f = ImageFont.truetype(FONT, 12 * S2)
    cx, cy, gap = 519, 296, 18
    for lab, (dx, dy) in {"X": (0, -1), "A": (1, 0), "B": (0, 1), "Y": (-1, 0)}.items():
        bx, by = cx + dx * gap, cy + dy * gap
        ring(bx, by, 10, 4)
        d.text((bx * S2, by * S2), lab, font=f, fill=line, anchor="mm")
    # sticks (below, slightly inward)
    for cx in (418, 493):
        ring(cx, 356, 19, 6); ring(cx, 356, 8, 5)
    # SELECT / START at the bottom
    for px in (440, 471):
        d.rounded_rectangle(sc([(px - 12, 393), (px + 12, 402)]), radius=4 * S2, outline=line, width=4 * S2)
    a = big.resize((W, H), Image.LANCZOS)
    # leader lines, 5 px, full strength: SUPER (L2) / SMARTBOMB (R2) drop onto the back shoulder tabs, MOVE /
    # SHOOT run along the bottom and up to the sticks.
    lines = Image.new("L", (W, H), 0); dl = ImageDraw.Draw(lines)
    for x0, x1, xv, y, yv in ((315, 386, 384, 82, 98), (525, 596, 527, 82, 98),
                              (315, 420, 418, 427, 375), (491, 596, 493, 427, 375)):
        dl.rectangle((x0, y - 2, x1, y + 2), fill=255)
        if abs(yv - y) > 4: dl.rectangle((xv - 2, min(y, yv) - 2, xv + 2, max(y, yv)), fill=255)
    return Image.composite(lines, a, lines)

def rgba(alpha):
    return Image.merge("RGBA", (Image.new("L", alpha.size, 255),) * 3 + (alpha,))

dg = diagram()
save(out, "Config_A_B.png", rgba(dg))
save(out, "Config_A_B_shadow.png", rgba(shadow(dg)))

with open(os.path.join(ROOT, "src", "prompts.inc"), "w") as f:
    f.write("// generated by tools/make_prompts.py - R36S-style shoulder prompts and controller diagram\n")
    for n, data in out.items():
        f.write('{"%s", %d, (const unsigned char*)"' % (n, len(data)))
        f.write("".join("\\x%02x" % b for b in data))
        f.write('"},\n')
print("wrote", len(out), "images")
