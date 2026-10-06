#!/usr/bin/env python3
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Fetch only the ALab stoat files needed for a groom demonstration.

The DPEL Techvar package is a 9.6 GB ZIP. Its host supports byte ranges, so
this tool reads the central directory and requested members without storing
the whole archive. The output is local, ignored by Git, and remains subject to
the ASWF Digital Assets License shipped with ALab.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

TECHVAR_URL = "https://dpel-assets.aswf.io/usd-alab/alab-techvars.v2.2.0.zip"
GITHUB_RAW = "https://raw.githubusercontent.com/DigitalProductionExampleLibrary/ALab/v2.3.0/"
GITHUB_TREE = "https://api.github.com/repos/DigitalProductionExampleLibrary/ALab/git/trees/v2.3.0?recursive=1"


class RemoteRangeFile(io.RawIOBase):
    """Seekable HTTP object using cached 4 MiB byte ranges."""

    def __init__(self, url: str, block_size: int = 4 << 20):
        self.url = url
        self.block_size = block_size
        request = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(request, timeout=60) as response:
            self.size = int(response.headers["Content-Length"])
            if response.headers.get("Accept-Ranges") != "bytes":
                raise RuntimeError(f"Server does not support HTTP byte ranges: {url}")
        self.pos = 0
        self.cache: dict[int, bytes] = {}

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=os.SEEK_SET):
        if whence == os.SEEK_END:
            offset += self.size
        elif whence == os.SEEK_CUR:
            offset += self.pos
        self.pos = max(0, offset)
        return self.pos

    def read(self, length=-1):
        if length is None or length < 0:
            length = self.size - self.pos
        length = min(length, self.size - self.pos)
        if length <= 0:
            return b""
        pieces = []
        while length:
            index = self.pos // self.block_size
            block = self.cache.get(index)
            if block is None:
                start = index * self.block_size
                end = min(self.size, start + self.block_size) - 1
                request = urllib.request.Request(self.url, headers={"Range": f"bytes={start}-{end}"})
                with urllib.request.urlopen(request, timeout=120) as response:
                    if response.status != 206:
                        raise RuntimeError(f"Expected HTTP 206 for {self.url}, got {response.status}")
                    block = response.read()
                if len(block) != end - start + 1:
                    raise IOError(f"Short byte range {start}-{end}")
                # The central directory fits in a few blocks. Bound cache for
                # extraction of many disconnected members.
                if len(self.cache) >= 16:
                    self.cache.pop(next(iter(self.cache)))
                self.cache[index] = block
            offset = self.pos % self.block_size
            chunk = block[offset:offset + length]
            pieces.append(chunk)
            self.pos += len(chunk)
            length -= len(chunk)
        return b"".join(pieces)


def download_raw(relative: str, destination: Path):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        return
    with urllib.request.urlopen(GITHUB_RAW + relative, timeout=90) as source:
        destination.write_bytes(source.read())


def safe_extract_member(archive: zipfile.ZipFile, member: str, destination: Path):
    if ".." in Path(member).parts or Path(member).is_absolute():
        raise ValueError(f"Unsafe ZIP path: {member}")
    # DPEL distributes overlays rooted at techvar_assets/fragment while the
    # structure checkout expects ALab/fragment.
    relative = member.removeprefix("techvar_assets/")
    target = destination / "ALab" / relative
    root = (destination / "ALab").resolve()
    if not target.resolve().is_relative_to(root):
        raise ValueError(f"ZIP member escapes destination: {member}")
    if target.exists() and target.stat().st_size == archive.getinfo(member).file_size:
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with archive.open(member) as source, target.open("wb") as sink:
        shutil.copyfileobj(source, sink, 1 << 20)


def inspect_zip(archive: zipfile.ZipFile, pattern: str):
    rex = re.compile(pattern, re.IGNORECASE)
    matches = [info for info in archive.infolist() if rex.search(info.filename)]
    for info in matches:
        print(f"{info.file_size:>12,}  {info.filename}")
    print(f"{len(matches)} matches, {sum(info.file_size for info in matches):,} uncompressed bytes")


def fetch_structure(out: Path):
    with urllib.request.urlopen(GITHUB_TREE, timeout=90) as response:
        tree = json.load(response)["tree"]
    paths = ["LICENSE.md"] + [
        item["path"] for item in tree
        if "stoat" in item["path"].lower()
        and ("/entity/" in item["path"] or "/fragment/" in item["path"])
        and (
            item["path"].endswith(".usda")
            or ("/entity/" in item["path"] and "/preview/" in item["path"]
                and item["path"].endswith(".png"))
        )
    ]
    with ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(lambda path: download_raw(path, out / path), paths))
    print(f"Fetched {len(paths)} structure files")


PREPARE_PATTERN = (
    r"fragment/(geo/modelling/stoat_(body|outfit|backpack)01/render_high/mesh/.*\.usd"
    r"|perfrig/rigging/stoat01/base/rig/.*/(rig\.usda|FX_skeleton\.usdc)"
    r"|look/surfacing/stoat_(body|outfit|backpack)01/.*\.(usda|jpg|exr))$"
)


def extract_matching(out: Path, pattern: str):
    remote = RemoteRangeFile(TECHVAR_URL)
    with zipfile.ZipFile(remote) as archive:
        names = [info.filename for info in archive.infolist()
                 if re.search(pattern, info.filename, re.IGNORECASE) and not info.is_dir()]
        for i, name in enumerate(names, 1):
            safe_extract_member(archive, name, out)
            print(f"{i}/{len(names)} {name}", flush=True)
    return names


def consolidate(out: Path, usdcat_arg: str | None):
    source = out
    result_dir = source.parent
    usdcat = usdcat_arg or os.environ.get("USD") and str(Path(os.environ["USD"]) / "bin" / "usdcat") or shutil.which("usdcat")
    if not usdcat:
        raise RuntimeError("Provide --usdcat or set USD to the OpenUSD install prefix")
    usdcat = str(Path(usdcat).resolve())
    if not Path(usdcat).is_file():
        raise FileNotFoundError(usdcat)
    env = dict(os.environ)
    if os.name == "nt":
        prefix = Path(usdcat).parent.parent
        env["PATH"] = os.pathsep.join([str(prefix / "lib"), str(prefix / "bin"), env.get("PATH", "")])

    wrapper = source / "stoat_source.usda"
    wrapper.write_text('''#usda 1.0
(
    defaultPrim = "Stoat"
    metersPerUnit = 0.01
    upAxis = "Y"
)
def SkelRoot "Stoat" (
    prepend references = @ALab/entity/stoat01/stoat01.usda@
    variants = {
        string alfro = "off"
        string geo = "render_high"
        string geo_vis = "default"
        string skeleton = "on"
    }
)
{
}
''', encoding="utf-8")
    flat = result_dir / "stoat01_flat.usda"
    subprocess.run([usdcat, "--flatten", str(wrapper), "-o", str(flat)], env=env, check=True)
    data = flat.read_text(encoding="utf-8")
    # `usdcat --flatten` writes absolute paths for texture assets. Keep the
    # single USD independent of its preparation location and copy sidecars.
    source_textures = source / "ALab" / "fragment" / "look" / "surfacing"
    texture_files = list(source_textures.glob("stoat_*01/render_high/texture/*/*"))
    for texture in texture_files:
        target = result_dir / "textures" / texture.parent.parent.parent.parent.name / texture.name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.stat().st_size != texture.stat().st_size:
            shutil.copyfile(texture, target)
    card_files = list((source / "ALab" / "entity").glob("stoat_*01/preview/*/*.png"))
    for card in card_files:
        target = result_dir / card.parent.name / card.name
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.stat().st_size != card.stat().st_size:
            shutil.copyfile(card, target)
    path_pattern = re.compile(r"@[^@\n]*/fragment/look/surfacing/(stoat_(?:body|outfit|backpack)01)/render_high/texture/[^/@]+/([^/@]+)@")
    data, replacements = path_pattern.subn(lambda m: f"@./textures/{m.group(1)}/{m.group(2)}@", data)
    if replacements < 20:
        raise RuntimeError(f"Only {replacements} texture paths rewritten; expected at least 20")
    card_pattern = re.compile(r"@[^@\n]*/entity/stoat_(?:body|outfit|backpack)01/preview/(stoat_(?:body|outfit|backpack)01_preview)/([^/@]+)@")
    data, card_replacements = card_pattern.subn(lambda m: f"@./{m.group(1)}/{m.group(2)}@", data)
    if card_replacements != len(card_files):
        raise RuntimeError(f"Rewrote {card_replacements} of {len(card_files)} card paths")
    # The upstream outfit shader references stoat_outfit01.<UDIM>.exr for AO,
    # but that filename has no member in the Techvar archive. A neutral AO
    # value keeps the authored look usable without inventing a texture.
    outfit_start = data.index('    def Xform "outfit_M_hrc"')
    outfit_end = data.index('    def Xform "backpack_M_hrc"', outfit_start)
    outfit = data[outfit_start:outfit_end]
    outfit, ao_connection_count = re.subn(
        r'float inputs:occlusion.connect = </Stoat/outfit_M_hrc/MATERIAL/usd_full/full_ao_texture.outputs:r>',
        'float inputs:occlusion = 1', outfit,
    )
    outfit, ao_shader_count = re.subn(
        r'                def Shader "full_ao_texture"\n                \{\n.*?\n                \}\n',
        '', outfit, count=1, flags=re.DOTALL,
    )
    if ao_connection_count != 1 or ao_shader_count != 1:
        raise RuntimeError("Could not neutralize ALab outfit's missing AO texture")
    data = data[:outfit_start] + outfit + data[outfit_end:]
    data = re.sub(r'    doc = """Generated from Composed Stage.*?"""\n', '', data, count=1, flags=re.DOTALL)
    absolute_assets = [path for path in re.findall(r"@([^@\n]+)@", data)
                       if re.match(r"(?i)^[a-z]:[/\\]|^/|^\\\\", path)]
    if absolute_assets or re.search(r"(?i)[a-z]:[/\\](?:work|users|home)[/\\]", data):
        raise RuntimeError(f"Flattened USD still contains workstation paths: {absolute_assets[:3]}")
    assets = re.findall(r"@([^@\n]+)@", data)
    missing_assets = []
    for asset in assets:
        path = result_dir / asset.removeprefix("./")
        if "<UDIM>" in asset:
            if not list(path.parent.glob(path.name.replace("<UDIM>", "[0-9][0-9][0-9][0-9]"))):
                missing_assets.append(asset)
        elif not path.is_file():
            missing_assets.append(asset)
    if missing_assets:
        raise RuntimeError(f"Missing sidecars: {missing_assets}")
    flat.write_text(data, encoding="utf-8")
    output = result_dir / "stoat01.usdc"
    subprocess.run([usdcat, str(flat), "-o", str(output)], env=env, check=True)
    subprocess.run([usdcat, "-l", str(output)], env=env, check=True)
    license_source = source / "LICENSE.md"
    shutil.copyfile(license_source, result_dir / "ALab-LICENSE.md")
    usdzip = str(Path(usdcat).with_name("usdzip.exe" if os.name == "nt" else "usdzip"))
    if not Path(usdzip).is_file():
        raise RuntimeError(f"OpenUSD usdzip is required for the portable package: {usdzip}")
    package = result_dir / "stoat01.usdz"
    report = {
        "source": "Netflix Animation Studios ALab",
        "asset_structure": "v2.3.0",
        "techvar_assets": "v2.2.0",
        "source_repository": "https://github.com/DigitalProductionExampleLibrary/ALab",
        "techvar_url": TECHVAR_URL,
        "license": "ASWF Digital Assets License v1.1",
        "license_file": "ALab-LICENSE.md",
        "modification": "Flattened the ALab stoat assembly, selected render_high geometry and skeleton:on, rewrote texture paths to local sidecars, and set the outfit's missing ambient-occlusion texture input to neutral 1.",
        "output_usd": output.name,
        "output_usdz": package.name,
        "root_prim": "/Stoat",
        "skin_mesh": "/Stoat/body_M_hrc/GEO/body_M_geo",
        "skeleton": "/Stoat/RIG/root_jnt",
        "texture_count": len(texture_files),
        "card_count": len(card_files),
        "verification": "usdcat load-only opens the USDC and USDZ; each authored sidecar has a file or numbered UDIM tiles. OpenUSD 26.08 usdchecker on Windows reports false unresolvable dependencies for UDIM templates and Windows asset paths, so it is not a passing validator for this source material.",
    }
    (result_dir / "provenance.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    package.unlink(missing_ok=True)
    subprocess.run(
        [usdzip, "-r", package.name, output.name,
         "textures/stoat_body01", "textures/stoat_outfit01", "textures/stoat_backpack01",
         "stoat_body01_preview", "stoat_outfit01_preview",
         "stoat_backpack01_preview", "ALab-LICENSE.md", "provenance.json"],
        cwd=result_dir, env=env, check=True,
    )
    subprocess.run([usdcat, "-l", str(package)], env=env, check=True)
    print(f"Wrote {output} ({output.stat().st_size:,} bytes), {len(texture_files)} textures, {replacements + card_replacements} relative paths")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["list", "extract", "fetch-structure", "consolidate", "prepare"])
    parser.add_argument("--out", type=Path, default=Path("out/alab/source"))
    parser.add_argument("--pattern", help="Regex for list/extract; extract defaults to the bounded character subset")
    parser.add_argument("--usdcat", help="Path to OpenUSD usdcat; defaults to $USD/bin/usdcat or PATH")
    args = parser.parse_args()

    if args.command == "fetch-structure":
        fetch_structure(args.out)
        return
    if args.command == "consolidate":
        consolidate(args.out, args.usdcat)
        return
    if args.command == "prepare":
        fetch_structure(args.out)
        extract_matching(args.out, PREPARE_PATTERN)
        consolidate(args.out, args.usdcat)
        return

    if args.command == "list":
        with zipfile.ZipFile(RemoteRangeFile(TECHVAR_URL)) as archive:
            inspect_zip(archive, args.pattern or r"stoat|LICENSE")
    else:
        extract_matching(args.out, args.pattern or PREPARE_PATTERN)


if __name__ == "__main__":
    main()
