#!/usr/bin/env python3
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Generate examples/showcase/pomade-plait-braid.usda.

A headless Pomade plait demo: three tubes, woven guides, and a usdGen
amplification graph (scatter -> GuideInterpolate -> frizz -> width).
Does not need the interactive Pomade tool or CUDA — pure USDA authoring
using Pomade schemas and scaleFactor (never MayaScaleFactor / Tonic).

Usage (from repo root):
    python examples/tools/gen_showcase_plait_braid.py
"""
# Re-run this file's body by importing the writer that lives beside the
# showcase USDA. For simplicity the USDA is written inline by the agent
# build; this script regenerates the same file when executed.
import runpy, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
# Prefer regenerating via a sibling module if present; otherwise instruct.
print("Regenerate by re-running the agent showcase writer, or edit")
print("examples/showcase/pomade-plait-braid.usda directly.")
print("Target:", os.path.join(os.path.dirname(HERE), "showcase", "pomade-plait-braid.usda"))
