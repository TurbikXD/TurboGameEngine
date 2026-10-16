"""Original procedural space illustrations and vector icons for the UI demo.

Requires Pillow. No downloads, fonts or third-party image material are used.
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
PALETTES = {
    "ion": ((92, 240, 207), (15, 68, 100)),
    "ember": ((255, 200, 125), (126, 43, 69)),
    "azure": ((145, 220, 255), (41, 77, 155)),
    "dusk": ((220, 162, 255), (78, 49, 130)),
    "gaia": ((207, 247, 156), (24, 97, 105)),
    "sol": ((255, 237, 171), (194, 83, 52)),
}


def planet(name, size=256):
    bright, dark = PALETTES[name]
    image = Image.new("RGBA", (size, size))
    pixels = image.load()
    radius = size * 0.38
    for y in range(size):
        for x in range(size):
            nx, ny = (x - size * 0.5) / radius, (y - size * 0.5) / radius
            squared = nx * nx + ny * ny
            if squared >= 1:
                continue
            nz = math.sqrt(1 - squared)
            light = max(0, nx * -0.47 + ny * -0.43 + nz * 0.77)
            bands = 0.5 + 0.19 * math.sin(ny * 20 + nx * 2 + math.sin(nx * 7) * 0.45)
            mix = min(1, bands * 0.55 + light * 0.65)
            rim = pow(1 - nz, 4) * 0.2
            rgb = tuple(int(min(255, (a * mix + b * (1 - mix)) * (0.22 + light * 0.78) + 100 * rim))
                        for a, b in zip(bright, dark))
            pixels[x, y] = (*rgb, int(min(1, (1 - squared) * size) * 255))
    return image


def icon_atlas():
    scale = 4
    image = Image.new("RGBA", (256 * scale, 64 * scale))
    draw = ImageDraw.Draw(image)

    def line(points, width=3):
        draw.line([(int(x * scale), int(y * scale)) for x, y in points], fill="white", width=width * scale, joint="curve")

    draw.ellipse((17 * scale, 17 * scale, 47 * scale, 47 * scale), outline="white", width=3 * scale)
    draw.ellipse((9 * scale, 25 * scale, 55 * scale, 40 * scale), outline="white", width=3 * scale)
    line([(82, 43), (91, 20), (103, 12), (108, 25), (99, 46), (82, 43)])
    line([(91, 43), (85, 53), (96, 47)])
    draw.ellipse((94 * scale, 22 * scale, 102 * scale, 30 * scale), outline="white", width=2 * scale)
    line([(145, 45), (151, 24), (161, 12), (172, 24), (178, 45), (165, 39), (145, 45)])
    line([(151, 51), (171, 51)])
    line([(216, 23), (232, 39)])
    line([(232, 23), (216, 39)])
    draw.ellipse((202 * scale, 10 * scale, 246 * scale, 54 * scale), outline="white", width=2 * scale)
    return image.resize((256, 64), Image.Resampling.LANCZOS)


def hero():
    scale = 2
    image = Image.new("RGBA", (1024 * scale, 640 * scale))
    draw = ImageDraw.Draw(image)
    # Sparse deterministic stars; the art remains transparent over UIGradient.
    for index in range(85):
        x, y = (index * 149 + 59) % 990 + 17, (index * 113 + 41) % 580 + 20
        radius = 1.7 if index % 7 == 0 else 0.8
        draw.ellipse(((x - radius) * scale, (y - radius) * scale, (x + radius) * scale, (y + radius) * scale),
                     fill=(204, 222, 255, 210 if index % 7 == 0 else 115))
    # Orbital tracks and tilted station silhouette.
    for radius in (178, 219, 269):
        draw.ellipse(((596 - radius) * scale, (323 - radius * 0.58) * scale,
                      (596 + radius) * scale, (323 + radius * 0.58) * scale),
                     outline=(166, 185, 238, 95), width=scale)
    world = planet("dusk", 570 * scale)
    image.alpha_composite(world, (315 * scale, 19 * scale))
    draw = ImageDraw.Draw(image)
    # Warm crescent satellite.
    satellite = planet("ember", 130 * scale)
    image.alpha_composite(satellite, (785 * scale, 67 * scale))
    draw = ImageDraw.Draw(image)
    station = [(296, 391), (345, 362), (389, 387), (340, 416)]
    draw.polygon([(x * scale, y * scale) for x, y in station], fill=(172, 203, 230, 255))
    draw.polygon([(296 * scale, 391 * scale), (340 * scale, 416 * scale), (340 * scale, 437 * scale), (296 * scale, 412 * scale)], fill=(62, 88, 128, 255))
    draw.polygon([(340 * scale, 416 * scale), (389 * scale, 387 * scale), (389 * scale, 409 * scale), (340 * scale, 437 * scale)], fill=(93, 136, 171, 255))
    for x, y in ((272, 365), (355, 429)):
        draw.polygon([(x * scale, y * scale), ((x + 42) * scale, (y - 24) * scale),
                      ((x + 68) * scale, (y - 9) * scale), ((x + 26) * scale, (y + 15) * scale)], fill=(73, 185, 207, 255))
    draw.line([(285 * scale, 376 * scale), (372 * scale, 426 * scale)], fill=(220, 239, 255, 255), width=4 * scale)
    draw.ellipse((708 * scale, 431 * scale, 718 * scale, 441 * scale), fill=(116, 242, 211, 255))
    return image.resize((1024, 640), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    ROOT.mkdir(exist_ok=True)
    for name in PALETTES:
        planet(name).save(ROOT / (name + ".png"), optimize=True)
    icon_atlas().save(ROOT / "icons.png", optimize=True)
    hero().save(ROOT / "frontier.png", optimize=True)
    print("Generated 8 original PNG assets; no external artwork used.")
