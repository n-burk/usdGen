"""Independent Far-sampled clearance oracle for the convex Collide demo Shield.

The native collision path uses Bfr patches. This proof path samples the same
authored Catmull-Clark topology through Far and checks radial clearance against
a finer triangulation. The finite samples have an explicit convergence error;
they are not represented as the exact limit surface.
"""

# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT

from __future__ import annotations

from pathlib import Path
import struct

import numpy as np
from scipy.spatial import cKDTree


def read_far_grid(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        header = stream.read(16)
        if len(header) != 16 or header[:8] != b"FARCC001":
            raise RuntimeError(f"Invalid Far oracle sample: {path}")
        faces, rate = struct.unpack("<II", header[8:])
        vertices = np.fromfile(stream, dtype="<f8")
    expected = faces * (rate + 1) ** 2 * 3
    if len(vertices) != expected:
        raise RuntimeError(f"Truncated Far oracle sample: {path}")
    return vertices.reshape(faces, rate + 1, rate + 1, 3)


def convergence_error(coarse: np.ndarray, fine: np.ndarray) -> float:
    """Max distance between finer Far points and coarse linear facets."""
    faces, side, _, _ = coarse.shape
    if fine.shape != (faces, side * 2 - 1, side * 2 - 1, 3):
        raise RuntimeError("Far grids do not have a 2:1 sampling ratio")
    if np.max(np.abs(coarse - fine[:, ::2, ::2])) > 1e-8:
        raise RuntimeError("Far grids disagree at shared analytic samples")
    y, x = np.meshgrid(np.arange(fine.shape[1]),
                       np.arange(fine.shape[2]), indexing="ij")
    ix = np.minimum(x // 2, side - 2)
    iy = np.minimum(y // 2, side - 2)
    u = ((x - 2 * ix) / 2)[:, :, None]
    v = ((y - 2 * iy) / 2)[:, :, None]
    error = 0.0
    for face in range(faces):
        a = coarse[face, iy, ix]
        b = coarse[face, iy, ix + 1]
        c = coarse[face, iy + 1, ix + 1]
        d = coarse[face, iy + 1, ix]
        linear = np.where(u >= v,
                          a + (b - a) * u + (c - b) * v,
                          a + (c - d) * u + (d - a) * v)
        error = max(error, float(np.linalg.norm(fine[face] - linear,
                                                 axis=-1).max()))
    return error


def stitch_far_grid(grid: np.ndarray, face_vertices: np.ndarray,
                    maximum_shift: float = 1e-6) -> tuple[np.ndarray, float]:
    """Make duplicated samples on authored shared edges byte-identical."""
    stitched = np.array(grid, copy=True)
    original = np.array(grid, copy=True)
    side = stitched.shape[1]
    edge_refs: dict[tuple[int, int], list[tuple[int, int, bool]]] = {}
    for face, vertices in enumerate(np.asarray(face_vertices, dtype=np.int64)):
        for edge, (a, b) in enumerate(zip(vertices, np.roll(vertices, -1))):
            key = (min(int(a), int(b)), max(int(a), int(b)))
            edge_refs.setdefault(key, []).append((face, edge, int(a) > int(b)))

    def edge_view(face: int, edge: int) -> np.ndarray:
        if edge == 0:
            return stitched[face, 0, :]
        if edge == 1:
            return stitched[face, :, -1]
        if edge == 2:
            return stitched[face, -1, ::-1]
        return stitched[face, ::-1, 0]

    max_seen = 0.0
    for refs in edge_refs.values():
        owner_face, owner_edge, owner_reverse = min(refs)
        owner = edge_view(owner_face, owner_edge)
        canonical = owner[::-1].copy() if owner_reverse else owner.copy()
        for face, edge, reverse in refs:
            target = edge_view(face, edge)
            values = canonical[::-1] if reverse else canonical
            max_seen = max(max_seen, float(np.linalg.norm(target - values,
                                                            axis=1).max()))
            target[...] = values

    # Edge ownership can select different copies of a corner. Stitch authored
    # vertices once more so all incident edge endpoints agree exactly.
    corner_refs: dict[int, list[tuple[int, int, int]]] = {}
    corners = ((0, 0), (0, side - 1), (side - 1, side - 1),
               (side - 1, 0))
    for face, vertices in enumerate(np.asarray(face_vertices, dtype=np.int64)):
        for vertex, (row, column) in zip(vertices, corners):
            corner_refs.setdefault(int(vertex), []).append((face, row, column))
    for refs in corner_refs.values():
        owner = min(refs)
        value = stitched[owner[0], owner[1], owner[2]].copy()
        for face, row, column in refs:
            max_seen = max(max_seen, float(np.linalg.norm(
                stitched[face, row, column] - value)))
            stitched[face, row, column] = value
    # Report and bound the net displacement after every edge and corner
    # ownership assignment, including samples assigned more than once.
    max_seen = float(np.linalg.norm(stitched - original, axis=-1).max())
    if max_seen > maximum_shift:
        raise RuntimeError(f"Far shared-edge stitch exceeded {maximum_shift}: "
                           f"{max_seen}")
    return stitched, max_seen


class RadialSurface:
    """Signed radial clearance of a convex, star-shaped closed triangle mesh."""

    def __init__(self, vertices: np.ndarray, triangles: np.ndarray,
                 center: np.ndarray):
        local = np.asarray(vertices, dtype=np.float64) - center
        tri = local[np.asarray(triangles, dtype=np.int64)]
        normals = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        orient = np.einsum("ij,ij->i", normals, tri.mean(axis=1))
        if np.any(orient <= 0):
            raise RuntimeError("Radial oracle requires outward convex triangles")
        self.triangles = tri
        self.tree = cKDTree(tri.mean(axis=1) /
                              np.linalg.norm(tri.mean(axis=1), axis=1)[:, None])
        # A facet interior can lie closer to the center than every vertex.
        # Its plane distance is a conservative lower bound for fast culling.
        self.min_radius = float(np.min(
            orient / np.linalg.norm(normals, axis=1)))
        self.max_radius = float(np.linalg.norm(local, axis=1).max())
        self.exhaustive_fallback_rays = 0

    @classmethod
    def from_far(cls, grid: np.ndarray, center: np.ndarray) -> "RadialSurface":
        faces, side, _, _ = grid.shape
        ids = np.arange(faces * side * side).reshape(faces, side, side)
        a = ids[:, :-1, :-1].ravel()
        b = ids[:, :-1, 1:].ravel()
        c = ids[:, 1:, 1:].ravel()
        d = ids[:, 1:, :-1].ravel()
        triangles = np.concatenate((np.stack((a, b, c), axis=1),
                                    np.stack((a, c, d), axis=1)))
        return cls(grid.reshape(-1, 3), triangles, center)

    def _ray_radius(self, directions: np.ndarray, k: int) -> np.ndarray:
        _, ids = self.tree.query(directions, k=k)
        tri = self.triangles[ids]
        a, b, c = tri[:, :, 0], tri[:, :, 1], tri[:, :, 2]
        e0, e1 = b - a, c - a
        normal = np.cross(e0, e1)
        rays = directions[:, None, :]
        denom = np.einsum("nkj,nkj->nk", normal, np.broadcast_to(rays, normal.shape))
        radius = np.divide(np.einsum("nkj,nkj->nk", normal, a), denom,
                           out=np.full_like(denom, np.nan), where=denom > 1e-15)
        q = rays * radius[:, :, None] - a
        dot00 = np.einsum("nkj,nkj->nk", e0, e0)
        dot01 = np.einsum("nkj,nkj->nk", e0, e1)
        dot11 = np.einsum("nkj,nkj->nk", e1, e1)
        dot02 = np.einsum("nkj,nkj->nk", e0, q)
        dot12 = np.einsum("nkj,nkj->nk", e1, q)
        inverse = 1.0 / (dot00 * dot11 - dot01 * dot01)
        u = (dot11 * dot02 - dot01 * dot12) * inverse
        v = (dot00 * dot12 - dot01 * dot02) * inverse
        hit = (np.isfinite(radius) & (radius > 0) & (u >= -1e-8) &
               (v >= -1e-8) & (u + v <= 1 + 1e-8))
        return np.min(np.where(hit, radius, np.inf), axis=1)

    def _exhaustive_triangle_radius(self, directions: np.ndarray) -> np.ndarray:
        """Intersect rare missed rays with every actual Far triangle."""
        result = np.full(len(directions), np.inf, dtype=np.float64)
        # Bound temporary arrays while retaining the same intersection and
        # barycentric tests as _ray_radius.
        for start in range(0, len(self.triangles), 4096):
            tri = self.triangles[start:start + 4096]
            a, b, c = tri[:, 0], tri[:, 1], tri[:, 2]
            e0, e1 = b - a, c - a
            normal = np.cross(e0, e1)
            rays = directions[:, None, :]
            denom = directions @ normal.T
            radius = np.divide(
                np.einsum("kj,kj->k", normal, a)[None, :], denom,
                out=np.full_like(denom, np.nan), where=denom > 1e-15)
            q = rays * radius[:, :, None] - a[None, :, :]
            dot00 = np.einsum("kj,kj->k", e0, e0)[None, :]
            dot01 = np.einsum("kj,kj->k", e0, e1)[None, :]
            dot11 = np.einsum("kj,kj->k", e1, e1)[None, :]
            dot02 = np.einsum("nkj,kj->nk", q, e0)
            dot12 = np.einsum("nkj,kj->nk", q, e1)
            inverse = 1.0 / (dot00 * dot11 - dot01 * dot01)
            u = (dot11 * dot02 - dot01 * dot12) * inverse
            v = (dot00 * dot12 - dot01 * dot02) * inverse
            hit = (np.isfinite(radius) & (radius > 0) & (u >= -1e-8) &
                   (v >= -1e-8) & (u + v <= 1 + 1e-8))
            result = np.minimum(result,
                                np.min(np.where(hit, radius, np.inf), axis=1))
        return result

    def clearance(self, points: np.ndarray, center: np.ndarray) -> np.ndarray:
        local = np.asarray(points, dtype=np.float64) - center
        lengths = np.linalg.norm(local, axis=1)
        result = np.empty(len(local), dtype=np.float64)
        low = lengths < self.min_radius
        high = lengths > self.max_radius
        result[low] = lengths[low] - self.min_radius
        result[high] = lengths[high] - self.max_radius
        shell = ~(low | high)
        if np.any(shell):
            directions = local[shell] / lengths[shell, None]
            radius = self._ray_radius(directions, 16)
            missing = ~np.isfinite(radius)
            if np.any(missing):
                radius[missing] = self._ray_radius(directions[missing], 128)
            missing = ~np.isfinite(radius)
            if np.any(missing):
                fallback = self._exhaustive_triangle_radius(directions[missing])
                radius[missing] = fallback
                self.exhaustive_fallback_rays += int(np.count_nonzero(
                    np.isfinite(fallback)))
            if not np.all(np.isfinite(radius)):
                raise RuntimeError("Far radial query missed a surface triangle")
            result[shell] = lengths[shell] - radius
        return result


def strand_masks(counts: list[int], points: list[tuple[float, float, float]],
                 surface: RadialSurface, center: np.ndarray,
                 tolerance: float = 2e-4) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    positions = np.asarray(points, dtype=np.float64)
    clearance = surface.clearance(positions, center)
    inside = clearance < -tolerance
    valid = np.ones(len(positions) - 1, dtype=bool)
    valid[np.cumsum(counts)[:-1] - 1] = False
    a, b = positions[:-1], positions[1:]
    direction = b - a
    closest_t = np.clip(np.einsum("ij,ij->i", center - a, direction) /
                        np.maximum(np.einsum("ij,ij->i", direction, direction),
                                   1e-30), 0, 1)
    nearest = a + direction * closest_t[:, None]
    candidates = np.flatnonzero(valid &
        (np.linalg.norm(nearest - center, axis=1) <= surface.max_radius + tolerance))
    crossed = np.zeros(len(valid), dtype=bool)
    if len(candidates):
        # Dense point samples are a visual-control proof. Their finite spacing
        # and Far tessellation uncertainty are reported separately.
        samples = np.linspace(0, 1, 33)
        sample_points = a[candidates, None, :] + direction[candidates, None, :] * samples[None, :, None]
        minimum = surface.clearance(sample_points.reshape(-1, 3), center).reshape(
            len(candidates), len(samples)).min(axis=1)
        crossed[candidates] = minimum < -tolerance
    return inside, crossed, clearance
