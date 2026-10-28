#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Linux fork: Xbox and Steam Deck button pictures for MediEvil (after the Uncharted Linux fork's
button prompts).

The game shows PlayStation buttons only. Its pictures are ordinary textures in its paks: the
painted buttons of the tutorial prompts (UI/Textures/ButtonPrompts/PS4_*, DXT5), the HUD's
button atlas (UI/Sprites/HUD/InventoryButtonAtlas, BGRA) and a few HUD buttons. They are read
from the player's own game at start, repainted and put into the override pak (ue4_override.py):
Cross, Circle, Square and Triangle become A, B, X and Y, Options the Menu button (☰) and the
touchpad the View button (⧉), which is what the Deck's and an Xbox pad's buttons do in this port
(runtime_pad.c: Start is Options, Back the touchpad). The Xbox style also renames the shoulder
buttons (LB RB LT RT); the Steam Deck's are L1 R1 L2 R2 already. Steam Deck: white letters on
dark buttons; Xbox: A B X Y in their colours.

PS4 textures are tiled: 8x8 elements (DXT blocks or pixels) per tile, Morton order inside a tile,
tiles in rows. Nothing of the game is stored in this repository; the result is cached per style.

Usage: python scripts/button_prompts.py <game dir> <xbox|steamdeck> <out dir>   (writes PNGs)
"""
import hashlib
import io
import os
import pickle
import struct
import sys
from pathlib import Path

STYLES = ('playstation', 'xbox', 'steamdeck')
VERSION = 1  # bump when the pictures change (the cache key)

PROMPTS = 'OVERBITE/Content/UI/Textures/ButtonPrompts/'
HUD = 'OVERBITE/Content/UI/Sprites/HUD/'
FACE = {'Cross': 'A', 'Circle': 'B', 'Square': 'X', 'Triangle': 'Y'}
XBOX_COLOURS = {'A': (63, 174, 73), 'B': (217, 55, 47), 'X': (47, 111, 217), 'Y': (229, 181, 33)}
SHOULDERS = {'L1': 'LB', 'R1': 'RB', 'L2': 'LT', 'R2': 'RT'}
# The atlas: face buttons in the top row (64 px discs), then R1 L1 R2 L2, Options and the
# touchpad (x0, y0, x1, y1).
ATLAS_FACE = {'Circle': (0, 0, 64, 64), 'Cross': (64, 0, 128, 64), 'Triangle': (128, 0, 192, 64),
              'Square': (192, 0, 256, 64)}
ATLAS_SHOULDERS = {'R1': (513, 12, 575, 54), 'L1': (577, 12, 639, 54), 'R2': (641, 3, 703, 64),
                   'L2': (704, 3, 767, 64)}
ATLAS_OPTIONS = (783, 0, 818, 52)
ATLAS_OPTIONS_LABEL = (774, 54, 824, 63)
ATLAS_TOUCHPAD = (832, 12, 896, 52)
ATLAS_ROWS = 80  # only the top rows are repainted


# ---- textures --------------------------------------------------------------------------------

def parse(uexp):
    """(width, height, pixel format, data offset, data size) of a cooked Texture2D's first mip."""
    i = uexp.find(b'PF_')
    if i < 16:
        raise ValueError('no pixel format')
    length, = struct.unpack_from('<i', uexp, i - 4)
    width, height = struct.unpack_from('<ii', uexp, i - 16)
    fmt = uexp[i:i + length - 1].decode()
    j = i + length
    size, = struct.unpack_from('<I', uexp, j + 20)
    return width, height, fmt, j + 32, size


def morton8(n):
    x = y = 0
    for b in range(3):
        x |= ((n >> (2 * b)) & 1) << b
        y |= ((n >> (2 * b + 1)) & 1) << b
    return x, y


MORTON = [morton8(n) for n in range(64)]


def detile(data, columns, rows, unit, limit_rows=None):
    """Tiled elements (DXT blocks or pixels of `unit` bytes) -> rows of linear elements."""
    rows = rows if limit_rows is None else min(rows, limit_rows)
    out = bytearray(columns * rows * unit)
    tiles_x = columns // 8
    for n in range((rows + 7) // 8 * tiles_x * 64):
        tile, inner = divmod(n, 64)
        x, y = MORTON[inner]
        x += (tile % tiles_x) * 8
        y += (tile // tiles_x) * 8
        if y < rows:
            out[(y * columns + x) * unit:(y * columns + x + 1) * unit] = data[n * unit:(n + 1) * unit]
    return bytes(out)


def retile(linear, data, columns, rows, unit):
    """Writes linear elements of the first `rows` rows back into the tiled `data` (bytearray)."""
    tiles_x = columns // 8
    for n in range((rows + 7) // 8 * tiles_x * 64):
        tile, inner = divmod(n, 64)
        x, y = MORTON[inner]
        x += (tile % tiles_x) * 8
        y += (tile // tiles_x) * 8
        if y < rows:
            data[n * unit:(n + 1) * unit] = linear[(y * columns + x) * unit:(y * columns + x + 1) * unit]


def dds_header(width, height):
    header = bytearray(128)
    struct.pack_into('<4sIIIIIII', header, 0, b'DDS ', 124, 0x81007, height, width, width * height, 0, 1)
    struct.pack_into('<II4s', header, 76, 32, 4, b'DXT5')
    struct.pack_into('<I', header, 108, 0x1000)
    return bytes(header)


def load_texture(uexp, rows=None):
    """(RGBA image of the padded texture, how to write it back)."""
    from PIL import Image
    width, height, fmt, at, size = parse(uexp)
    data = uexp[at:at + size]
    if fmt == 'PF_DXT5':
        columns = (width + 3) // 4 + 7 & ~7
        lines = size // 16 // columns
        linear = detile(data, columns, lines, 16)
        image = Image.open(io.BytesIO(dds_header(columns * 4, lines * 4) + linear)).convert('RGBA')
        return image, ('dxt5', at, size, columns, lines)
    if fmt == 'PF_B8G8R8A8':
        columns = width + 7 & ~7
        lines = size // 4 // columns
        lines = lines if rows is None else min(lines, rows + 7 & ~7)
        linear = detile(data, columns, lines, 4)
        image = Image.frombytes('RGBA', (columns, lines), linear, 'raw', 'BGRA')
        return image, ('bgra', at, size, columns, lines)
    raise ValueError(f'unsupported pixel format {fmt}')


def store_texture(uexp, image, how):
    kind, at, size, columns, lines = how
    data = bytearray(uexp[at:at + size])
    if kind == 'dxt5':
        out = io.BytesIO()
        image.save(out, 'DDS', pixel_format='DXT5')
        retile(out.getvalue()[128:], data, columns, lines, 16)
    else:
        retile(image.tobytes('raw', 'BGRA'), data, columns, lines, 4)
    return uexp[:at] + bytes(data) + uexp[at + size:]


# ---- drawing ---------------------------------------------------------------------------------

def font(size):
    from PIL import ImageFont
    for folder in filter(None, (os.environ.get('MEDIEVIL_FONT_DIR'), '/usr/share/fonts/TTF',
                                '/usr/share/fonts/truetype/dejavu', '/usr/share/fonts/dejavu-sans-fonts')):
        for name in ('DejaVuSans-Bold.ttf', 'DejaVuSans.ttf'):
            path = Path(folder) / name
            if path.is_file():
                return ImageFont.truetype(str(path), size)
    return ImageFont.load_default(size)


def saturated(image, box, threshold=45):
    """Mask (L) of the coloured symbol pixels in `box`: the PlayStation shapes are the only
    strongly coloured parts of these grey or parchment buttons."""
    from PIL import Image, ImageFilter
    hsv = image.convert('RGB').convert('HSV')
    s = hsv.getchannel('S').point(lambda v: 255 if v > threshold else 0)
    alpha = image.getchannel('A').point(lambda v: 255 if v > 40 else 0)
    from PIL import ImageChops
    mask = ImageChops.multiply(s, alpha)
    keep = Image.new('L', image.size, 0)
    keep.paste(mask.crop(box), box)
    return keep.filter(ImageFilter.MaxFilter(7))


def clear(image, box):
    """Makes `box` transparent (text standing on its own, such as OPTIONS under the button)."""
    from PIL import Image
    image.paste(Image.new('RGBA', (box[2] - box[0], box[3] - box[1]), (0, 0, 0, 0)), box[:2])
    return image


def dark(image, box, threshold=90):
    """Mask (L) of dark text pixels in `box` (the shoulder buttons' L1 R1 L2 R2)."""
    from PIL import Image, ImageFilter
    luma = image.convert('L').point(lambda v: 255 if v < threshold else 0)
    keep = Image.new('L', image.size, 0)
    keep.paste(luma.crop(box), box)
    return keep.filter(ImageFilter.MaxFilter(3))


def light(image, box, threshold=120):
    from PIL import Image, ImageFilter
    luma = image.convert('L').point(lambda v: 255 if v > threshold else 0)
    keep = Image.new('L', image.size, 0)
    keep.paste(luma.crop(box), box)
    return keep.filter(ImageFilter.MaxFilter(3))


def erase(image, mask, passes=24):
    """Fills the masked pixels from their surroundings (repeated blur, unmasked pixels kept)."""
    from PIL import Image, ImageFilter
    if not mask.getbbox():
        return image
    alpha = image.getchannel('A')
    filled = image.convert('RGB')
    # Start from the average colour around the symbol, then let the surroundings flow in.
    from PIL import ImageStat
    ring = mask.filter(ImageFilter.MaxFilter(9))
    ring = Image.eval(Image.merge('RGB', (ring, ring, ring)).convert('L'), lambda v: v)
    from PIL import ImageChops
    border = ImageChops.subtract(ring, mask)
    mean = tuple(int(v) for v in ImageStat.Stat(filled, border).mean) if border.getbbox() else (128, 128, 128)
    filled.paste(mean, mask=mask)
    for _ in range(passes):
        filled = Image.composite(filled.filter(ImageFilter.GaussianBlur(2)), filled, mask)
    result = filled.convert('RGBA')
    result.putalpha(alpha)
    return result


def letter(image, text, centre, size, fill, outline=None, stroke=0):
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    draw.text(centre, text, font=font(size), fill=fill, anchor='mm',
              stroke_width=stroke, stroke_fill=outline)


def menu_symbol(image, centre, width, colour):
    """☰: three bars."""
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    x, y = centre
    bar = max(2, width // 6)
    for k in (-1, 0, 1):
        top = y + k * bar * 2 - bar // 2
        draw.rounded_rectangle((x - width // 2, top, x + width // 2, top + bar), radius=bar // 2, fill=colour)


def view_symbol(image, centre, width, colour):
    """⧉: two overlapping squares."""
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    x, y = centre
    side = width * 2 // 3
    line = max(2, width // 9)
    draw.rectangle((x - width // 2 + side // 3, y - width // 2, x - width // 2 + side // 3 + side,
                    y - width // 2 + side), outline=colour, width=line)
    draw.rectangle((x - width // 2, y - width // 2 + side // 3, x - width // 2 + side,
                    y - width // 2 + side // 3 + side), fill=None, outline=colour, width=line)


def face_button(image, box, name, style, parchment):
    """A, B, X or Y in place of the PlayStation shape inside `box`."""
    from PIL import ImageDraw
    x0, y0, x1, y1 = box
    centre = ((x0 + x1) // 2, (y0 + y1) // 2)
    size = x1 - x0
    text = FACE[name]
    image = erase(image, saturated(image, box))
    if style == 'steamdeck' and parchment:
        # The Deck's dark button with a white letter, inside the painted parchment rim.
        r = size * 37 // 100
        ImageDraw.Draw(image).ellipse((centre[0] - r, centre[1] - r, centre[0] + r, centre[1] + r),
                                      fill=(38, 38, 40, 255))
    if style == 'steamdeck':
        letter(image, text, centre, size * 46 // 100, (245, 245, 245, 255))
    else:
        letter(image, text, centre, size * 50 // 100, XBOX_COLOURS[text] + (255,), (24, 22, 20, 255),
               max(1, size // 40))
    return image


# ---- the textures ----------------------------------------------------------------------------

def pictures(read, style):
    """{pak path: bytes} of the repainted textures; `read(path)` reads the game's own file."""
    files = {}

    def edit(path, change, rows=None):
        try:
            uasset, uexp = read(path + '.uasset'), read(path + '.uexp')
            image, how = load_texture(uexp, rows)
            image = change(image) or image
            files[path + '.uasset'] = uasset
            files[path + '.uexp'] = store_texture(uexp, image, how)
        except (FileNotFoundError, ValueError, OSError) as error:
            print(f'Button pictures: {path.rsplit("/", 1)[-1]} unchanged ({error})', flush=True)

    # The tutorial prompts' painted buttons (100x100 in 128x128).
    for name in FACE:
        edit(PROMPTS + 'PS4_' + name, lambda im, n=name: face_button(im, (0, 0, 100, 100), n, style, True))

    def options(im):
        clear(im, (0, 66, 100, 100))  # the word OPTIONS
        menu_symbol(im, (50, 35), 22, (230, 230, 230, 255))
        letter(im, 'MENU', (50, 84), 20, (30, 30, 30, 255))
        return im
    edit(PROMPTS + 'PS4_Options', options)

    def touchpad(im):
        view_symbol(im, (50, 50), 40, (235, 235, 235, 255))
        return im
    edit(PROMPTS + 'PS4_Touch_Pad', touchpad)

    if style == 'xbox':
        for ps, xbox in SHOULDERS.items():
            def shoulder(im, xbox=xbox):
                im = erase(im, light(im, (25, 15, 80, 70), 110))
                letter(im, xbox, (52, 42), 26, (200, 200, 200, 255))
                return im
            edit(PROMPTS + 'PS4_' + ps, shoulder)

    # The HUD's small triangle (the weapon switch) and its button atlas.
    edit(PROMPTS + 'HUD_PS4Button_Triangle', lambda im: face_button(im, (0, 0, 64, 64), 'Triangle', style, False))

    def atlas(im):
        for name, box in ATLAS_FACE.items():
            im = face_button(im, box, name, style, False)
        x0, y0, x1, y1 = ATLAS_OPTIONS
        menu_symbol(im, ((x0 + x1) // 2, (y0 + y1) // 2), 16, (40, 40, 40, 255))
        clear(im, ATLAS_OPTIONS_LABEL)
        letter(im, 'MENU', ((ATLAS_OPTIONS_LABEL[0] + ATLAS_OPTIONS_LABEL[2]) // 2, 59), 9, (40, 30, 20, 255))
        x0, y0, x1, y1 = ATLAS_TOUCHPAD
        view_symbol(im, ((x0 + x1) // 2, (y0 + y1) // 2), 24, (40, 40, 40, 255))
        if style == 'xbox':
            for ps, box in ATLAS_SHOULDERS.items():
                im = erase(im, dark(im, (box[0] + 8, box[1] + 6, box[2] - 8, box[3] - 4), 70))
                letter(im, SHOULDERS[ps], ((box[0] + box[2]) // 2, (box[1] + box[3]) // 2 + 2), 20,
                       (25, 25, 25, 255))
        return im
    edit(HUD + 'InventoryButtonAtlas', atlas, ATLAS_ROWS)
    return files


def prompt_files(game, style, cache_dir=None):
    """The override pak's files for `style` ({} for the PlayStation pictures), cached."""
    if style not in STYLES or style == 'playstation':
        return {}
    import ue4_override
    paks = sorted(ue4_override.paks_dir(game).glob('pakchunk*-ps4.pak'))
    key = hashlib.sha1(repr((VERSION, style, [(p.name, p.stat().st_size, int(p.stat().st_mtime))
                                               for p in paks])).encode()).hexdigest()[:16]
    cache = Path(cache_dir) / f'button-prompts-{style}-{key}.bin' if cache_dir else None
    if cache and cache.is_file():
        try:
            return pickle.loads(cache.read_bytes())
        except (OSError, ValueError, pickle.UnpicklingError):
            pass
    indexes = {p: ue4_override.read_index(p) for p in paks}

    def read(name):
        for pak, entries in indexes.items():
            if name in entries:
                return ue4_override.read_entry(pak, entries[name])
        raise FileNotFoundError(name)
    files = pictures(read, style)
    if cache:
        cache.parent.mkdir(parents=True, exist_ok=True)
        for old in cache.parent.glob(f'button-prompts-{style}-*.bin'):
            old.unlink(missing_ok=True)
        cache.write_bytes(pickle.dumps(files))
    return files


if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    game, style, out = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    for path, data in prompt_files(game, style).items():
        if path.endswith('.uexp'):
            image, _how = load_texture(data, ATLAS_ROWS if 'Atlas' in path else None)
            image.save(out / (path.rsplit('/', 1)[-1][:-5] + '.png'))
            print('wrote', path)
