"""Generate a linear HDR lat-long studio with bright windows and dark walls.

Requires NumPy and OpenImageIO maketx; the tiled, mipmapped EXR works in both
Storm and Moonray. This is a procedural lighting environment, not a photograph.
"""
from pathlib import Path
import shutil
import subprocess
import numpy as np


def main():
    here = Path(__file__).resolve().parent
    w, h = 1024, 512
    longitude = ((np.arange(w) + .5) / w * 2 * np.pi - np.pi)[None, :]
    latitude = (np.pi / 2 - (np.arange(h) + .5) / h * np.pi)[:, None]
    # Conventional Y-up lat-long; DomeLight.OrientToStageUpAxis handles USD Z-up.
    direction = np.stack(np.broadcast_arrays(
        np.cos(latitude) * np.sin(longitude), np.sin(latitude),
        np.cos(latitude) * np.cos(longitude)), axis=-1)
    hdr = np.zeros((h, w, 3), np.float32) + [.012, .014, .018]
    for center, half_width, half_height, color in (
        ((-1.8, 2.2, -2.4), .30, .34, (16., 14.8, 13.5)),
        ((2.2, 1., -1.2), .45, .36, (.9, 1.05, 1.3)),
        ((1.1, 2., 1.8), .15, .38, (6., 6., 6.)),
    ):
        aim = np.array(center, dtype=float); aim /= np.linalg.norm(aim)
        right = np.cross(aim, [0, 1, 0]); right /= np.linalg.norm(right)
        up = np.cross(right, aim)
        facing = direction @ aim
        x = np.arctan2(direction @ right, facing)
        y = np.arctan2(direction @ up, facing)
        # Soft rectangular windows, with black space between them.
        window = np.exp(-((x / half_width)**8 + (y / half_height)**8))
        window *= facing > 0
        hdr += window[..., None] * color
    scratch = here.parents[1] / 'build' / 'felt_studio_hdr.pfm'
    scratch.parent.mkdir(exist_ok=True)
    with scratch.open('wb') as file:
        file.write(('PF\n%d %d\n-1.0\n' % (w, h)).encode())
        file.write(hdr[::-1].astype('<f4').tobytes())
    maketx = shutil.which('maketx') or 'D:/vcpkg/installed/x64-windows/tools/openimageio/maketx.exe'
    output = here / 'textures' / 'studio_contrast.exr'
    subprocess.run([maketx, '--oiio', '--envlatl', '-d', 'half', '--format', 'openexr',
                    str(scratch), '-o', str(output)], check=True)
    print(str(output), 'linear range:', float(hdr.min()), float(hdr.max()))


if __name__ == '__main__':
    main()
