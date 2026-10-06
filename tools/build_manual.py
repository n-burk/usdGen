#!/usr/bin/env python3
# Copyright (c) 2026 Nick Burkard
# SPDX-License-Identifier: MIT
"""Build the offline artist manual from the checked-in operator and media registries.

Run ``python tools/build_manual.py`` from any directory. The resulting
``docs/site/index.html`` opens directly from disk; it never fetches JSON.
"""

from __future__ import annotations

import argparse
import html
import hashlib
import json
import re
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SITE = ROOT / "docs" / "site"
REFERENCE = ROOT / "docs" / "reference"
OPERATORS = REFERENCE / "operators.json"
MEDIA = REFERENCE / "media.json"
CHARACTER = REFERENCE / "character.json"
UI = REFERENCE / "ui.json"


def h(value: object) -> str:
    return html.escape(str(value), quote=True)


def slug(identifier: str) -> str:
    name = identifier.removeprefix("UsdGen")
    return re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()


def page_for(op: dict) -> str:
    return "operators/" + slug(op["id"]) + ".html"


def relative_url(path: str, page: str) -> str:
    """Turn a repository-relative source path into a working local file link."""
    clean = path.replace("\\", "/").split("#", 1)[0]
    target = (ROOT / clean).resolve()
    if not target.is_relative_to(ROOT) or not target.exists():
        return ""
    depth = len(Path(page).parts) - 1
    return "../" * (depth + 2) + h(clean)


def site_url(path: str, page: str) -> str:
    return "../" * (len(Path(page).parts) - 1) + path


def media_url(path: str, page: str) -> str:
    """Version local visual assets so a rebuilt manual shows replaced captures."""
    url = relative_url(path, page)
    if not url:
        return ""
    target = ROOT / path.replace("\\", "/").split("#", 1)[0]
    version = hashlib.sha256(target.read_bytes()).hexdigest()[:12]
    return f"{url}?v={version}"


def image_dimensions(path: str) -> tuple[int, int] | None:
    """Read a local PNG/GIF header without decoding the visual asset."""
    clean = path.replace("\\", "/").split("#", 1)[0]
    target = (ROOT / clean).resolve()
    if not target.is_relative_to(ROOT):
        return None
    try:
        with target.open("rb") as source:
            header = source.read(24)
    except OSError:
        return None
    if (len(header) >= 24 and header[:8] == b"\x89PNG\r\n\x1a\n" and
            header[8:12] == b"\x00\x00\x00\x0d" and header[12:16] == b"IHDR"):
        width, height = struct.unpack(">II", header[16:24])
    elif len(header) >= 13 and header[:6] in (b"GIF87a", b"GIF89a"):
        width, height = struct.unpack("<HH", header[6:10])
    else:
        return None
    return (width, height) if width > 0 and height > 0 else None


def image_size_attributes(path: str) -> str:
    dimensions = image_dimensions(path)
    if dimensions is None:
        return ""
    width, height = dimensions
    return f' width="{width}" height="{height}"'


def link_list(paths: list[str], page: str, empty: str) -> str:
    links = []
    for path in paths:
        url = relative_url(path, page)
        if url:
            links.append(f'<li><a href="{url}">{h(Path(path).name)}</a><small>{h(path)}</small></li>')
    return '<ul class="resource-list">' + "".join(links) + "</ul>" if links else f'<p class="muted">{h(empty)}</p>'


def media_for(op: dict, registry: dict) -> dict:
    entry = registry.get("operators", {}).get(op["id"], {})
    if isinstance(entry, str):
        entry = {"image": entry}
    result = dict(op.get("media") or {})
    result.update(entry)
    return result


def ui_capture(registry: dict, identifier: str) -> dict:
    """Accept either a capture list or a keyed capture map from the UI recorder."""
    captures = registry.get("captures", registry.get("screenshots", {}))
    if isinstance(captures, list):
        entry = next((item for item in captures if item.get("id") == identifier), {})
    else:
        entry = captures.get(identifier, {})
    if not isinstance(entry, dict):
        return {}
    entry = dict(entry)
    path = entry.get("image") or entry.get("path") or ""
    if path.startswith("media/"):
        path = "docs/site/" + path
    entry["image"] = path
    return entry


def operator_ui_capture(registry: dict, operator_id: str) -> dict:
    """Find the captured Noodles state for one typed operator."""
    captures = registry.get("captures", registry.get("screenshots", []))
    if isinstance(captures, dict):
        captures = [{"id": key, **value} for key, value in captures.items() if isinstance(value, dict)]
    return next((entry for entry in captures if isinstance(entry, dict) and
                 entry.get("operator") == operator_id), {})


def ui_visual(registry: dict, identifier: str, page: str, alt: str) -> str:
    entry = ui_capture(registry, identifier)
    image_path = entry.get("image", "")
    if not image_path or not relative_url(image_path, page):
        return ('<div class="visual visual--waiting ui-pending" role="note">'
                '<strong>usdview screenshot pending</strong>'
                '<span>This step will show the actual tool state when its capture is verified.</span></div>')
    caption = entry.get("caption") or entry.get("title") or alt
    source = entry.get("scene")
    source_url = relative_url(source, page) if source else ""
    image_url = media_url(image_path, page)
    source_link = f'<a href="{source_url}">Open practice scene ↗</a>' if source_url else ""
    return (f'<figure class="visual ui-visual"><a href="{image_url}" class="ui-visual__image-link" '
            f'aria-label="Open full-size screenshot: {h(alt)}"><img src="{image_url}"{image_size_attributes(image_path)} alt="{h(alt)}" loading="lazy"></a>'
            f'<figcaption><span>{h(caption)}</span><span class="ui-visual__links">'
            f'<a href="{image_url}">Open full-size screenshot ↗</a>{source_link}</span></figcaption></figure>')


def ui_step(number: int, anchor: str, title: str, instructions: str,
            capture_id: str, page: str, ui: dict, alt: str) -> str:
    return (f'<section id="{h(anchor)}" class="section ui-step">'
            f'<div class="ui-step__heading"><span>{number:02d}</span><h2>{h(title)}</h2></div>'
            f'<p>{instructions}</p>{ui_visual(ui, capture_id, page, alt)}</section>')


def visual(entry: dict, page: str, alt: str, compact: bool = False) -> str:
    inline_animation = bool(entry.get("inlineAnimation") and entry.get("animation") and not compact)
    path = entry.get("animation") if inline_animation else entry.get("image") or entry.get("animation")
    if inline_animation:
        alt = entry.get("animationAlt") or alt
    url = media_url(path, page) if path else ""
    if not url:
        return ('<div class="visual visual--waiting" role="note">'
                '<span class="visual-icon" aria-hidden="true">◌</span>'
                '<strong>Image awaiting render</strong>'
                '<span>This example has no verified capture yet.</span></div>')
    caption = entry.get("caption") or alt
    credit = " · ".join(str(entry[k]) for k in ("renderer", "provenance") if entry.get(k))
    extra = f'<span>{h(credit)}</span>' if credit and not compact else ""
    animation = entry.get("animation")
    animation_url = media_url(animation, page) if animation else ""
    animation_label = entry.get("animationLabel", "View motion")
    animate = (f' <a href="{animation_url}" title="Open animated preview">{h(animation_label)} ↗</a>'
               if animation_url and not compact and not inline_animation else "")
    animation_credit = entry.get("animationProvenance")
    if animation_url and animation_credit and not compact:
        extra += f'<span>{h(animation_credit)}</span>'
    if entry.get("provenanceDisclosure") and not compact:
        provenance_text = f'<p>{h(credit)}</p>' if credit else ""
        if animation_url and animation_credit:
            provenance_text += f'<p>{h(animation_credit)}</p>'
        extra = (f'<details><summary>How this was captured</summary>'
                 f'{provenance_text}</details>')
    image = f'<img src="{url}"{image_size_attributes(path)} alt="{h(alt)}" loading="lazy">'
    full_size_link = ""
    if (entry.get("comparison") or entry.get("fullSize")) and not compact:
        full_size_label = ("Open full-size animation" if inline_animation else
                           "Open full-size comparison" if entry.get("comparison") else "Open full-size image")
        image = (f'<a href="{url}" aria-label="{full_size_label}: {h(alt)}">{image}</a>')
        full_size_link = f'<a href="{url}">{full_size_label} ↗</a>'
    if inline_animation and entry.get("image"):
        still_url = media_url(entry["image"], page)
        still_label = "Open still comparison" if entry.get("comparison") else "Open still render"
        full_size_link += f' <a href="{still_url}">{still_label} ↗</a>'
    if not compact and (entry.get("comparison") or entry.get("fullSize") or entry.get("provenanceDisclosure")):
        return (f'<figure class="visual">{image}<figcaption class="result-caption">'
                f'<p>{h(caption)}</p><div class="result-caption__actions">{animate}{full_size_link}</div>'
                f'{extra}</figcaption></figure>')
    return (f'<figure class="visual">{image}'
            f'<figcaption>{h(caption)}{animate}{extra}</figcaption></figure>')


def status_label(op: dict) -> str:
    if op.get("status") == "reserved":
        return "Reserved · no kernel"
    if op["id"] == "UsdGenWidthBlend":
        return "Runtime only"
    return "Implemented"


def nav(ops: list[dict], page: str) -> str:
    groups: dict[str, list[dict]] = {}
    for op in ops:
        groups.setdefault(op.get("category") or "Other", []).append(op)
    selected = page
    top = [
        ("index.html", "Overview"),
        ("getting-started.html", "Get started"),
        ("pomade-workspace.html", "Pomade workspace"),
        ("gallery.html", "Example gallery"),
        ("character-groom.html", "Character grooming"),
        ("technical-setup.html", "Technical setup"),
    ]
    navitems = ""
    for url, label in top:
        current = " is-current" if selected == url else ""
        aria = ' aria-current="page"' if selected == url else ""
        navitems += f'<a class="nav-link{current}" href="{site_url(url, page)}"{aria}>{label}</a>'
    sections = []
    for category, items in groups.items():
        active = any(page_for(op) == page for op in items)
        rows = ""
        for op in items:
            op_url = page_for(op)
            current = " is-current" if selected == op_url else ""
            aria = ' aria-current="page"' if selected == op_url else ""
            reserved = '<span class="nav-reserved">Reserved</span>' if op.get("status") == "reserved" else ""
            rows += (f'<a class="nav-link nav-link--operator{current}" '
                     f'href="{site_url(op_url, page)}"{aria}>{h(op["title"])}{reserved}</a>')
        sections.append(f'<details class="nav-group"{" open" if active else ""}><summary>{h(category)}'
                        f'<span>{len(items)}</span></summary><div>{rows}</div></details>')
    return (f'<a class="nav-link nav-link--home" href="{site_url("index.html", page)}">'
            '<span class="brand-mark" aria-hidden="true">u<span>G</span></span>'
            '<span class="brand-name">usdGen <small>Artist manual</small></span></a>'
            '<button class="search-trigger" type="button" data-search-open aria-keyshortcuts="Control+K Meta+K /">'
            '<span aria-hidden="true">⌕</span> Search the manual <kbd>/</kbd></button>'
            '<div class="nav-label">Learn</div>' + navitems + '<div class="nav-label">Operator reference</div>'
            + "".join(sections))


def shell(title: str, eyebrow: str, body: str, page: str, ops: list[dict], toc: list[tuple[str, str]] = []) -> str:
    search = [{"title": op["title"], "kind": op.get("category", "Operator"), "url": site_url(page_for(op), page),
               "text": " ".join([op.get("summary", ""), op.get("availability", ""), op.get("execution", ""),
                                  " ".join(op.get("workflow", [])), " ".join(op.get("tips", [])),
                                  " ".join(op.get("pitfalls", []))] +
                                 [" ".join([str(p.get("name", "")), str(p.get("description", "")),
                                            str(p.get("default", "")), " ".join(map(str, p.get("allowed", [])))])
                                  for p in op.get("parameters", [])])} for op in ops]
    search = [{"title": title, "kind": "Guide", "url": site_url(url, page), "text": description}
              for url, title, description in [
                  ("index.html", "Overview", "Hair and fur grooming with usdGen"),
                  ("getting-started.html", "Get started", "Visual usdview Pomade walkthrough scalp region tube guides output save"),
                  ("pomade-workspace.html", "Pomade workspace", "Visual tour of Graph Tube Fill Hierarchy Sculpt Output"),
                  ("gallery.html", "Example gallery", "Operator patches and working USD scenes"),
                  ("character-groom.html", "Character grooming", "ALab stoat in usdview, sparse guides, Noodles noise edit, and dense coat"),
                  ("technical-setup.html", "Technical setup", "Build install launch and generate example scenes")]] + search
    encoded = json.dumps(search, ensure_ascii=False).replace("<", "\\u003c")
    toc_html = "".join(f'<a href="#{h(anchor)}">{h(label)}</a>' for anchor, label in toc)
    css_version = hashlib.sha256((SITE / "assets" / "manual.css").read_bytes()).hexdigest()[:12]
    return f'''<!doctype html>
<!-- Copyright (c) 2026 Nick Burkard; SPDX-License-Identifier: MIT -->
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light"><title>{h(title)} · usdGen manual</title>
<link rel="stylesheet" href="{site_url("assets/manual.css", page)}?v={css_version}">
</head><body><a class="skip-link" href="#main">Skip to content</a>
<header class="mobile-header"><button type="button" data-nav-toggle aria-controls="sidebar" aria-expanded="false" aria-label="Open navigation">☰</button><a href="{site_url("index.html", page)}">usdGen <span>Artist manual</span></a><button type="button" data-search-open aria-label="Search">⌕</button></header>
<aside class="sidebar" id="sidebar" aria-label="Manual navigation"><nav>{nav(ops,page)}</nav></aside>
<div class="workspace"><main id="main" tabindex="-1"><div class="content"><div class="eyebrow">{eyebrow}</div>{body}</div></main>
<aside class="toc" aria-label="On this page"><p>On this page</p>{toc_html}</aside>
<footer>usdGen artist manual <span>·</span> <a href="{relative_url('README.md', page)}">Repository README</a> <span>·</span> <a href="{relative_url('docs/README.md', page)}">All documentation</a></footer></div>
<dialog class="search-dialog" aria-label="Search the manual"><div class="search-box"><label for="manual-search">Search the manual</label><button type="button" data-search-close aria-label="Close search">×</button><input id="manual-search" type="search" placeholder="Try “clump”, “guide”, or “width”" autocomplete="off"><div class="search-results" role="listbox" aria-label="Search results"></div><p class="search-help">↑ ↓ to move · Enter to open · Esc to close</p></div></dialog>
<script type="application/json" id="search-data">{encoded}</script><script src="{site_url("assets/manual.js", page)}" defer></script>
</body></html>'''


def bullet_section(label: str, items: list[str], empty: str = "No notes recorded.") -> str:
    if not items:
        return f'<p class="muted">{h(empty)}</p>'
    return "<ul class=\"prose-list\">" + "".join(f"<li>{h(x)}</li>" for x in items) + "</ul>"


def operator_page(op: dict, ops: list[dict], media: dict, ui: dict) -> str:
    page = page_for(op)
    entry = media_for(op, media)
    params = op.get("parameters", [])
    inputs = op.get("inputs", [])
    examples = op.get("examples", [])
    source = op.get("source", [])
    if isinstance(examples, str): examples = [examples]
    if isinstance(source, str): source = [source]
    def param_table(items: list[dict]) -> str:
        rows = ""
        for p in items:
            choices = p.get("allowed") or []
            allowed = ('<div class="allowed"><b>Choices</b> ' +
                       ", ".join("<code>" + h(v if v else "empty · automatic") + "</code>" for v in choices) +
                       "</div>") if choices else ""
            property_name = h(p.get("name", "")).replace(":", ":<wbr>")
            rows += (f'<tr><th scope="row"><code>{property_name}</code></th>'
                     f'<td>{h(p.get("type") or "—")}</td>'
                     f'<td>{h(p.get("default") if p.get("default") is not None else "—")}</td>'
                     f'<td>{h(p.get("description") or "—")}{allowed}</td></tr>')
        return ('<div class="table-wrap"><table><thead><tr><th>Property</th><th>Type</th><th>Default</th><th>What it controls</th></tr></thead>'
                f'<tbody>{rows}</tbody></table></div>') if rows else '<p class="muted">No operator-specific properties are listed.</p>'
    common_names = {"usdGen:enabled", "usdGen:seed", "usdGen:mask"}
    common = [p for p in params if p.get("name") in common_names]
    specific = [p for p in params if p.get("name") not in common_names]
    common_html = (f'<details class="parameter-details"><summary>Shared operator controls <span>{len(common)} properties</span></summary>'
                   f'{param_table(common)}</details>') if common else ""
    params_html = param_table(specific) + common_html
    status = status_label(op)
    badge_class = "badge--reserved" if op.get("status") == "reserved" else "badge--live"
    scene = entry.get("scene")
    scene_url = relative_url(scene, page) if scene else ""
    scene_button = f'<a class="button button--soft" href="{scene_url}">Open captured scene ↗</a>' if scene_url else ""
    type_label = "Runtime node ID" if op["id"] == "UsdGenWidthBlend" else "USD prim type"
    availability = op.get("availability", status)
    execution = op.get("execution", "See implementation notes")
    variants = entry.get("variants") or []
    has_media = bool(entry.get("image") or entry.get("animation") or variants)
    visual_html = ('<div class="visual visual--waiting" role="note"><strong>No rendered result</strong>'
                   '<span>This type is reserved in the schema and has no registered kernel.</span></div>'
                   if op.get("status") == "reserved" and not has_media else
                   visual(entry, page, op["title"] + (" concept diagram" if entry.get("status") == "diagram" else " operator result")))
    if variants:
        visual_html = ""
        for variant in variants:
            label = variant.get("label") or op["title"]
            practice = variant.get("scene")
            practice_link = (f'<p class="source-note"><a href="{relative_url(practice, page)}">'
                             f'Open {h(label.lower())} practice scene ↗</a></p>') if practice else ""
            visual_html += (f'<h3>{h(label)}</h3>' + visual(variant, page, label + " operator result") +
                            practice_link)
    if entry.get("status") == "diagram":
        visual_html += '<p class="diagram-note">Concept diagram · this is an explanation of the operator, not a rendered output.</p>'
    if op["id"] == "UsdGenWidthBlend":
        ui_section = ('<section id="in-usdview" class="section ui-exception" data-ui-state="runtime-only">'
                      '<h2>In usdview</h2><p>Width Blend is a runtime node without a concrete USD schema type. '
                      'It does not appear as a typed creation choice in Noodles. Inspect the checked-in '
                      'fixture and its output instead.</p></section>')
    elif op.get("status") == "reserved":
        ui_section = ('<section id="in-usdview" class="section ui-exception" data-ui-state="unavailable">'
                      '<h2>In usdview</h2><p>This schema type has no registered runtime kernel. '
                      'Noodles may show its authored prim, but it cannot produce the pictured groom result.</p></section>')
    else:
        capture = operator_ui_capture(ui, op["id"])
        actions = capture.get("actions") or []
        if isinstance(actions, str):
            actions = [actions]
        instruction = ('<ol class="prose-list ui-action-list">' +
                       ''.join(f'<li>{h(action)}</li>' for action in actions) + '</ol>' if actions else
                       '<p>Open the linked stage in usdview. Select this existing operator in the Scene Graph, '
                       'choose <strong>Window → Noodles Editor</strong>, focus the graph, and press <kbd>A</kbd> '
                       'to add the selected prim. The <code>Ops</code> sibling order determines execution; '
                       'graph card placement does not.</p>')
        selected_prim = capture.get("selectedPrim")
        if selected_prim:
            instruction += f'<p class="source-note">Selected prim: <code>{h(selected_prim)}</code></p>'
        edited_field = capture.get("editedField")
        visible_field = capture.get("visibleField")
        if visible_field:
            instruction += f'<p class="source-note">On-screen label: <strong>{h(visible_field)}</strong></p>'
        if edited_field:
            instruction += f'<p class="source-note">USD property: <code>{h(edited_field)}</code></p>'
        related_ui = "".join(ui_visual(ui, identifier, page, "Practice-scene control in Noodles Editor")
                             for identifier in capture.get("relatedCaptures", []))
        ui_section = (f'<section id="in-usdview" class="section ui-step" data-ui-state="captured">'
                      f'<div class="ui-step__heading"><span>UI</span><h2>Inspect in usdview</h2></div>'
                      f'{instruction}{ui_visual(ui, capture.get("id", ""), page, op["title"] + " in the Noodles Editor")}'
                      f'{related_ui}'
                      '<p class="source-note">The USD <code>Ops</code> sibling order controls the cook. '
                      'Moving graph cards or drawing relationship wires does not reorder ordinary operators. '
                      'Array properties and relationships have no inline value cell.</p></section>')
    body = f'''<div class="crumbs"><a href="{site_url('index.html', page)}">Manual</a><span>/</span><a href="{site_url('gallery.html', page)}">Operators</a><span>/</span>{h(op.get('category','Other'))}</div>
<div class="title-row"><div><h1>{h(op['title'])}</h1><p class="lede">{h(op.get('summary') or '')}</p></div><span class="badge {badge_class}">{status}</span></div>
<div class="operator-id">{type_label} <code>{h(op['id'])}</code></div>
<section id="at-a-glance" class="section"><h2>At a glance</h2><div class="fact-grid"><div><span>Receives</span><strong>{h('; '.join(inputs) if inputs else 'See source contract')}</strong></div><div><span>Produces</span><strong>{h(op.get('output') or 'See source contract')}</strong></div><div><span>Execution</span><strong>{h(execution)}</strong></div></div><p class="availability"><strong>Availability:</strong> {h(availability)}.</p></section>
<section id="visual" class="section"><h2>What it looks like</h2>{visual_html}{scene_button}</section>
{ui_section}
<section id="workflow" class="section"><h2>Use in a groom</h2>{bullet_section('',op.get('workflow',[]),'A workflow has not been documented for this operator yet.')}</section>
<section id="settings" class="section"><h2>Useful settings</h2><p class="section-intro">These are the authored USD property names and defaults from the schema and implementation. <a href="{relative_url('docs/reference/operator-authoring.md',page)}">How operator stacks are authored ↗</a></p>{params_html}</section>
<section id="tips" class="section"><h2>Working notes</h2><div class="note-columns"><div><h3>Good to know</h3>{bullet_section('',op.get('tips',[]))}</div><div><h3>Watch for</h3>{bullet_section('',op.get('pitfalls',[]))}</div></div></section>
<section id="files" class="section"><h2>Open the source</h2><div class="note-columns"><div><h3>Example scenes</h3>{link_list(examples,page,'No standalone scene is listed for this operator.')}</div><div><h3>Implementation</h3>{link_list(source,page,'Source paths are not listed yet.')}</div></div></section>
<div class="next-card"><span>Keep exploring</span><strong>See every operator in context</strong><a href="{site_url('gallery.html', page)}">Browse the example gallery →</a></div>'''
    return shell(op["title"], "Operator reference", body, page, ops,
                 [("at-a-glance", "At a glance"), ("visual", "What it looks like"), ("in-usdview", "In usdview"), ("workflow", "Use in a groom"),
                  ("settings", "Useful settings"), ("tips", "Working notes"), ("files", "Open the source")])


def home(ops: list[dict], media: dict, character: dict) -> str:
    page = "index.html"
    featured = [next((o for o in ops if o["id"] == ident), None) for ident in
                ("UsdGenScatter", "UsdGenGrow", "UsdGenGuideInterpolate", "UsdGenClump")]
    cards = "".join(f'<a class="feature-card" href="{site_url(page_for(op),page)}"><span>{h(op.get("category","Operator"))}</span>'
                    f'<strong>{h(op["title"])}</strong><p>{h(op.get("summary",""))}</p><em>Read reference ↗</em></a>'
                    for op in featured if op)
    hero_path = "docs/site/media/stoat-groom.png" if character.get("hero_approved") else ""
    if not hero_path or not (ROOT / hero_path).exists():
        clump = next((o for o in ops if o["id"] == "UsdGenClump"), None)
        hero_path = (media_for(clump, media).get("image") if clump else None) or ""
    hero_url = relative_url(hero_path, page) if hero_path else ""
    hero_credit = '<small class="hero-credit">Netflix Animation Studios ALab Copyright 2025 Netflix, Inc. All rights reserved.</small>' if hero_path == "docs/site/media/stoat-groom.png" else ""
    hero_art = (f'<div class="hero-art hero-art--image"><img src="{hero_url}"{image_size_attributes(hero_path)} alt="Verified usdGen groom render">{hero_credit}</div>' if hero_url else
                '<div class="hero-art" aria-hidden="true"><div class="flow-label">DESCRIPTION</div><div class="flow-node">Scatter</div><div class="flow-line"></div><div class="flow-node">Grow</div><div class="flow-line"></div><div class="flow-node flow-node--accent">Style &amp; groom</div><div class="flow-caption">A readable operator stack, authored in USD</div></div>')
    body = f'''<div class="hero"><div class="hero-copy"><div class="hero-kicker">OPENUSD HAIR & FUR</div><h1>Shape the groom.<br><em>Keep the graph.</em></h1><p>Follow real usdview screens to bind a scalp, draw regions, shape guides, and build hair in Pomade. Then inspect the guide-driven ALab stoat and its operator stack.</p><div class="hero-actions"><a class="button button--primary" href="getting-started.html">Start in usdview <span>→</span></a><a class="button button--outline" href="character-groom.html">Explore the stoat</a></div></div>{hero_art}</div>
<section id="path" class="section home-section"><div class="section-heading"><span class="index-number">01 / LEARN</span><h2>Follow the tool on screen</h2><p>Each lesson pairs a visible control with the actual usdview state and its result.</p></div><div class="path-grid"><a href="getting-started.html" class="path-card"><span class="path-icon">↗</span><small>FIRST GROOM</small><h3>Make hair in Pomade</h3><p>Follow the dock from scalp binding to guides, visible output, and a saved groom.</p><b>Get started →</b></a><a href="gallery.html" class="path-card"><span class="path-icon">◎</span><small>LOOK DEVELOPMENT</small><h3>Compare operator patches</h3><p>Jump from a visual result to its scene and the settings that made it.</p><b>Browse gallery →</b></a><a href="character-groom.html" class="path-card"><span class="path-icon">✦</span><small>CHARACTER FUR</small><h3>Inspect the ALab stoat</h3><p>Open the prepared character in usdview, find the sparse guides, and inspect its coat controls.</p><b>Follow the route →</b></a></div></section>
<section id="operators" class="section home-section"><div class="section-heading"><span class="index-number">02 / REFERENCE</span><h2>Build the stack, one decision at a time</h2><p>The operator pages focus on when to use a node, what it consumes, and which controls change the result.</p></div><div class="feature-grid">{cards}</div><a class="text-link" href="gallery.html">All {len(ops)} documented operator types →</a></section>
<section id="model" class="section home-section"><div class="section-heading"><span class="index-number">03 / MENTAL MODEL</span><h2>Grooms are ordinary USD</h2></div><div class="model-grid"><div><span>01</span><h3>Author a description</h3><p>A groom holds descriptions, operator prims, guides, maps, and looks on the stage.</p></div><div><span>02</span><h3>Order the stack</h3><p>Arrange operators under <code>Ops</code>. Read siblings bottom to top; the first child publishes the result.</p></div><div><span>03</span><h3>Preview in Hydra</h3><p>The scene index cooks the graph and publishes hair curves for the renderer.</p></div></div><p class="source-note">Learn <a href="{relative_url('docs/reference/operator-authoring.md',page)}">how to author an operator stack</a>, or open the <a href="{relative_url('examples/README.md',page)}">working examples</a>.</p></section>'''
    return shell("Overview", "Artist manual / Overview", body, page, ops, [("path", "Learning path"), ("operators", "Operator reference"), ("model", "How grooms work")])


def getting_started(ops: list[dict], ui: dict) -> str:
    page = "getting-started.html"
    body = (f'<div class="crumbs"><a href="index.html">Manual</a><span>/</span>Learn</div>'
            '<h1>Make your first groom in usdview</h1>'
            '<p class="lede">Follow the Pomade workspace from a scalp mesh to renderable hair. '
            'Each step names the control you can see in usdview and shows the matching tool state.</p>'
            f'<p class="setup-line">Need to install or launch the plugins first? <a href="technical-setup.html">Technical setup →</a> '
            f'The pictured practice scalp is <a href="{relative_url("examples/pomade-single-quad.usda",page)}">pomade-single-quad.usda</a>; '
            'its two small regions intentionally leave part of the surface uncovered.</p>')
    steps = [
        (1, "workspace", "Open the Pomade workspace",
         'Open a stage with a scalp mesh in usdview. In the menu bar choose <strong>usdGen → Pomade → Open workspace</strong>. The Pomade dock appears on the right.',
         "pomade-01-open-workspace", "Pomade workspace in usdview after opening it"),
        (2, "bind", "Bind the scalp",
         'Select the scalp <strong>Mesh</strong> in usdview. In the Pomade dock, click <strong>Bind scalp mesh...</strong>. The dock then switches to Graph mode and names the bound scalp.',
         "pomade-02-bind-scalp", "Pomade first-run dock with Bind scalp mesh button"),
        (3, "region", "Draw a root region",
         'In <strong>Graph → Create region</strong>, click at least three positions on the scalp. Click the first point or press <kbd>Enter</kbd> to close the outline. Pomade makes an L1 tube for the closed region.',
         "pomade-03-first-region", "Pomade Graph mode with a closed scalp region"),
        (4, "tube", "Shape the tube",
         'Choose <strong>Tube → Whole tube</strong>, click the tube wall, then choose <strong>Move</strong>. The transform controls adjust the selected tube. For a local bend, switch the component to <strong>Center CV</strong> and drag one center control.',
         "pomade-05-select-tube", "Pomade Tube mode with a selected tube"),
        (5, "fill", "Fill the volume with guides",
         'Choose <strong>Fill → Params</strong>. Select a tube and adjust <strong>Density</strong> and <strong>CVs per guide</strong> in Parameters. Entering Fill generates guides; <strong>Display → Show generated curves</strong> keeps them visible. The practice scene shows an uncovered-scalp warning because its regions are intentionally partial.',
         "pomade-06-fill-parameters", "Pomade Fill parameters beside generated guide curves"),
        (6, "output", "Build visible hair",
         'Choose <strong>Output</strong> and click <strong>Build hair description</strong>. Turn on <strong>Show amplified hair</strong> to view the result. The Output panel exposes strand density multiplier and width. The practice groom is intentionally incomplete, so its coverage and kink warnings remain visible.',
         "pomade-10-build-output", "Pomade Output mode with Build hair description and amplified hair"),
        (7, "save", "Save the groom",
         'Click <strong>Save</strong> in the dock file row and choose a <code>.usdc</code> destination. Pomade flushes the newest commit, writes its region map beside the groom, and adds the saved layer to the usdview session.',
         "pomade-11-save-groom", "Pomade Output workspace showing the Save button"),
        (8, "resume", "Resume the saved groom",
         'When a saved groom is composed on the reopened stage, choose <strong>usdGen → Pomade → Open workspace</strong>, then click <strong>Resume groom</strong> in the dock. The Graph region and guides return to the live workspace.',
         "pomade-13-resume-offered", "Pomade dock offering Resume groom after reopening the stage"),
    ]
    body += "".join(ui_step(number, anchor, title, instructions, capture, page, ui, alt)
                    for number, anchor, title, instructions, capture, alt in steps)
    body += (f'<section id="next" class="section"><h2>Keep exploring</h2><p>The <a href="{relative_url("docs/pomade-tool.md",page)}">Pomade guide</a> covers every mode and shortcut. '
             '<a href="character-groom.html">Inspect the ALab stoat</a> to see sparse guides and a dense coat on a character, or '
             '<a href="gallery.html">browse the operator gallery</a> to learn what each stack operator changes.</p></section>')
    return shell("Get started", "Learn / In usdview", body, page, ops,
                 [(anchor, title) for _, anchor, title, *_ in steps] + [("next", "Keep exploring")])


def pomade_workspace(ops: list[dict], ui: dict) -> str:
    page = "pomade-workspace.html"
    known = [
        ("pomade-01-open-workspace", "Open the workspace"),
        ("pomade-02-bind-scalp", "Bind a scalp"),
        ("pomade-03-first-region", "Create the first region"),
        ("pomade-04-two-regions", "Share a boundary"),
        ("pomade-05-select-tube", "Shape a tube"),
        ("pomade-06-fill-parameters", "Set fill parameters"),
        ("pomade-07-fill-density", "Inspect guide density"),
        ("pomade-08-hierarchy", "Work at another level"),
        ("pomade-09-sculpt", "Sculpt the form"),
        ("pomade-10-build-output", "Build output hair"),
        ("pomade-11-save-groom", "Save the groom"),
        ("pomade-12-reopen-workspace", "Resume work"),
        ("pomade-13-resume-offered", "Resume offered"),
        ("pomade-14-resumed-groom", "Saved groom restored"),
    ]
    body = (f'<div class="crumbs"><a href="index.html">Manual</a><span>/</span>Learn</div>'
            '<h1>Pomade workspace, on screen</h1>'
            '<p class="lede">A visual tour of the real usdview dock and viewport states, from the first scalp bind through saved output. '
            '<a href="getting-started.html">Follow the numbered first-groom lesson</a> if you are working alongside the tool.</p>')
    toc = []
    for index, (identifier, fallback) in enumerate(known, 1):
        entry = ui_capture(ui, identifier)
        title = entry.get("title") or fallback
        anchor = "state-" + str(index)
        body += (f'<section id="{anchor}" class="section ui-step">'
                 f'<div class="ui-step__heading"><span>{index:02d}</span><h2>{h(title)}</h2></div>'
                 f'{ui_visual(ui, identifier, page, title + " in the Pomade workspace")}</section>')
        toc.append((anchor, title))
    body += f'<p class="source-note">Controls and shortcuts: <a href="{relative_url("docs/pomade-tool.md",page)}">Pomade tool documentation ↗</a></p>'
    return shell("Pomade workspace", "Learn / Visual tour", body, page, ops, toc)


def technical_setup(ops: list[dict]) -> str:
    page = "technical-setup.html"
    body = rf'''<div class="crumbs"><a href="index.html">Manual</a><span>/</span>Reference</div><h1>Technical setup</h1><p class="lede">Build and launch commands for the local examples. After the plugins load in usdview, return to the <a href="getting-started.html">visual first-groom lesson</a>.</p>
<section id="build" class="section"><h2>Build the plugins</h2><p>Use OpenUSD 26.08. The <a href="{relative_url('README.md',page)}">repository README</a> covers prerequisites and installation.</p><h3>Unix</h3><pre><code>export USD=/path/to/OpenUSD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSD_INSTALL_DIR="$USD"
ninja -C build
source bin/_env.sh</code></pre><h3>Windows PowerShell</h3><pre><code>.\bin\build_usdgen.ps1 -UsdInstallDir $env:USD</code></pre></section>
<section id="launch" class="section"><h2>Open a scene in usdview</h2><h3>Unix</h3><pre><code>"$PY" "$USD/bin/usdview" examples/pomade-single-quad.usda</code></pre><h3>Windows PowerShell</h3><pre><code>.\bin\launch_usdview.ps1 examples\pomade-single-quad.usda</code></pre><p>Other working scenes are listed in the <a href="{relative_url('examples/README.md',page)}">examples index</a>.</p></section>
<section id="stoat" class="section"><h2>Prepare the ALab stoat study</h2><p>The separately licensed ALab character is prepared locally under ignored <code>out/alab</code>. The <a href="{relative_url('examples/alab/README.md',page)}">ALab example guide</a> records source versions, the license, and the preparation outputs.</p><h3>Unix</h3><pre><code>python tools/prepare_alab.py prepare --usdcat "$USD/bin/usdcat"
python -S tools/make_stoat_groom.py
"$PY" "$USD/bin/usdview" examples/alab/stoat-groom.usda</code></pre><h3>Windows PowerShell</h3><pre><code>python tools\prepare_alab.py prepare --usdcat "$env:USD\bin\usdcat.exe"
python -S tools\make_stoat_groom.py
.\bin\launch_usdview.ps1 examples\alab\stoat-groom.usda</code></pre><p>The generated stage references the local ALab asset. The <a href="character-groom.html">character lesson</a> uses usdview images and visible controls.</p></section>'''
    return shell("Technical setup", "Reference / Setup", body, page, ops,
                 [("build", "Build"), ("launch", "Launch usdview"), ("stoat", "ALab study")])


def gallery(ops: list[dict], media: dict) -> str:
    page = "gallery.html"
    cards = []
    for op in ops:
        entry = media_for(op, media)
        picture = visual(entry, page, op["title"] + (" concept diagram" if entry.get("status") == "diagram" else " result"), compact=True)
        cards.append(f'<article class="gallery-card">{picture}<div class="gallery-card__body"><span>{h(op.get("category","Operator"))}</span><h3>{h(op["title"])}</h3><p>{h(op.get("summary",""))}</p><a href="{site_url(page_for(op),page)}">Explore settings →</a></div></article>')
    body = f'''<div class="crumbs"><a href="index.html">Manual</a><span>/</span>Explore</div><h1>Operator gallery</h1><p class="lede">Small, focused patches make it easier to see the job of each operator. Each card leads to its controls, workflow, and checked-in scenes. Explanatory diagrams are labeled separately from renderer captures.</p>
<div class="gallery-toolbar"><span>{len(ops)} operator types</span><label for="gallery-filter">Filter by category</label><select id="gallery-filter"><option value="all">All categories</option>{''.join(f'<option value="{h(c)}">{h(c)}</option>' for c in dict.fromkeys(o.get('category','Other') for o in ops))}</select></div>
<section id="patches" class="gallery-grid" aria-label="Operator patches">{''.join(cards)}</section>
<section id="scenes" class="section"><h2>Working scenes to open</h2><p>The <a href="{relative_url('examples/README.md',page)}">examples index</a> describes each scene and its CPU or CUDA lane. The simplest CPU scenes display in usdview.</p><div class="scene-list"><a href="{relative_url('examples/scatter-grow-plane.usda',page)}"><strong>Scatter → Grow</strong><span>The smallest complete groom</span>↗</a><a href="{relative_url('examples/styled-fur-plane.usda',page)}"><strong>Styled fur</strong><span>Noise, Length, Width</span>↗</a><a href="{relative_url('examples/guide-interpolate-plane.usda',page)}"><strong>Guide interpolation</strong><span>Sparse guides and region choices</span>↗</a><a href="{relative_url('examples/clump-ptex-plane.usda',page)}"><strong>Clump with Ptex</strong><span>Membership and tightness maps</span>↗</a></div></section>'''
    return shell("Example gallery", "Explore / Operator patches", body, page, ops, [("patches", "Operator patches"), ("scenes", "Working scenes")])


def character_groom(ops: list[dict], metadata: dict, ui: dict) -> str:
    page = "character-groom.html"
    image_meta = metadata.get("images", {})
    def character_image(key: str, caption: str, alt: str) -> str:
        entry = dict(image_meta.get(key, {}))
        path = entry.get("image") or entry.get("path") or f"docs/site/media/stoat-{key}.png"
        if path.startswith("media/"):
            path = "docs/site/" + path
        entry["image"] = path
        entry.setdefault("caption", caption)
        return visual(entry, page, alt)
    bare = character_image("bare", "The native ALab stoat before usdGen fur.", "Bare ALab stoat")
    guides = character_image("guides", "Sparse cyan guides show the comb direction.", "Sparse guides on ALab stoat")
    groom = character_image("groom", "Guide-driven usdGen coat on the ALab stoat.", "Full usdGen stoat coat")
    detail = character_image("detail", "Facial fur at the detail camera.", "ALab stoat fur detail")
    body = (f'<div class="crumbs"><a href="index.html">Manual</a><span>/</span>Learn</div>'
            '<h1>Inspect the ALab stoat in usdview</h1>'
            '<p class="lede">Follow the prepared character study on screen: open the stage, find the skin and sparse guides, inspect one coat operator, then compare the rendered result.</p>'
            '<div class="callout"><strong>Before you start</strong><p>The character comes from the separately licensed Netflix Animation Studios ALab asset. '
            'The local stage needs its prepared asset under <code>out/alab</code>. If it is missing, use the '
            '<a href="technical-setup.html#stoat">technical setup appendix</a>. '
            f'<a href="{relative_url("examples/alab/README.md",page)}">Read the source and license notes ↗</a></p></div>')
    steps = [
        (1, "open", "Open the prepared stoat",
         f'Open <a href="{relative_url("examples/alab/stoat-groom.usda",page)}"><code>examples/alab/stoat-groom.usda</code></a> in usdview. '
         'Choose <strong>HeroCam</strong> from the camera control to see the whole character. '
         'The packaged stage already contains the region descriptions and recipe-authored guide curves. '
         'If you plan to edit and keep the prepared scene, duplicate this file beside the original and open the copy instead.',
         "alab-01-open-stoat-hero", "usdview showing the ALab stoat from HeroCam"),
        (2, "skin", "Find the coat surface",
         'In the usdview Scene Graph, expand <strong>World → Character → body_M_hrc → GEO</strong> and select the '
         '<strong>body_M_geo</strong> mesh. The body and face region descriptions use this skin; the separate '
         'study tail emitter is fitted to the original ALab tuft.',
         "alab-02-select-body-mesh", "usdview Scene Graph with selected ALab body mesh"),
        (3, "guide-view", "Read the sparse guide direction",
         f'Open the companion <a href="{relative_url("examples/alab/stoat-guides.usda",page)}"><code>stoat-guides.usda</code></a> '
         'in usdview. Follow the cyan curves over the back, belly, face, and tail. The recipe authored these '
         'sparse control curves before the dense coat was grown; this step inspects them rather than creating them.',
         "alab-03-sparse-guides", "usdview showing sparse cyan guides on the ALab stoat"),
    ]
    body += "".join(ui_step(number, anchor, title, instructions, capture, page, ui, alt)
                    for number, anchor, title, instructions, capture, alt in steps)
    body += ('<section id="coat-map" class="section"><h2>What the prepared coat contains</h2>'
             '<p>The body mesh supplies BrownBody, CreamBody, BrownHead, and CreamFace. Mouth faces stay bare. '
             'Tail and DarkTip use a study-only smooth emitter fitted to the ALab tail tuft; the original tuft remains in the consolidated asset. '
             'Each region runs <a href="operators/scatter.html">Scatter</a> → '
             '<a href="operators/guide-interpolate.html">Guide Interpolate</a> → '
             '<a href="operators/noise.html">Noise</a> → <a href="operators/width.html">Width</a>. '
             'In the <code>Ops</code> hierarchy, usdGen evaluates the sibling stack bottom to top.</p>'
             '<div class="pipeline"><span>Skin regions</span><b>→</b><span>Sparse guides</span><b>→</b>'
             '<span>Scatter + Guide Interpolate</span><b>→</b><span>Noise + Width</span></div>'
             f'{guides}</section>')
    editable = [
        (4, "noodles", "Open the operator graph",
         'Return to the groom stage you opened. Choose <strong>Window → Layer Editor</strong> '
         '(<kbd>L</kbd>) and click its file-backed root layer, such as <code>stoat-groom.usda</code>, '
         'to set the edit target. '
         'Then choose <strong>Window → Noodles Editor</strong> (<kbd>N</kbd>). The graph starts empty. '
         'In the usdview Scene Graph expand '
         '<strong>World → Groom → BrownBody → Ops</strong> and select <strong>noise</strong>; focus the '
         'Noodles canvas and press <kbd>A</kbd> to add the selected prim to the graph.',
         "alab-05-select-noise-in-noodles", "Noodles Editor with the BrownBody noise operator"),
        (5, "edit", "Adjust a visible coat control",
         'In Noodles, right-click the canvas and choose <strong>Write Values → Default</strong> for a static edit. '
         'Click the numeric value cell for <code>usdGen:noise:magnitude</code>, type <code>0.031</code>, and press '
         '<kbd>Enter</kbd>. The prepared BrownBody value is <code>0.0238</code>; changing it varies the breakup '
         'on the brown coat. Numeric scalar values are editable in the graph; array controls and relationships '
         'do not have an inline value editor.',
         "alab-06-edit-noise-magnitude", "Noodles numeric noise magnitude control"),
        (6, "save-edit", "Save the editable layer",
         'Press <kbd>Ctrl</kbd>+<kbd>S</kbd> in Noodles to save the selected file-backed edit layer directly. '
         'There is no save dialog for this command; a brief saved-layer message may appear. '
         'The <code>0.031</code> value remains visible after saving. Rerunning the source recipe can overwrite '
         'the generated groom, so keep edits in the copy you opened.',
         "alab-07-save-noise-layer", "Noodles operator showing the edited noise magnitude after saving"),
    ]
    for number, anchor, title, instructions, capture, alt in editable:
        body += ui_step(number, anchor, title, instructions, capture, page, ui, alt)
        if anchor == "noodles" and ui_capture(ui, "alab-08-layer-editor"):
            body += ('<div class="section ui-extra" aria-label="Layer Editor view">'
                     '<h3>Check the edit target</h3>' +
                     '<p>The capture uses a temporary file-backed <code>stoat-ui-edit.usda</code> scratch layer to preserve the supplied scene. '
                     'That layer is created by the capture script; it is not included in the delivered example. '
                     'When following along, use the root layer of the stage you opened or your own persistent layer.</p>' +
                     ui_visual(ui, "alab-08-layer-editor", page,
                               "Layer Editor with temporary stoat-ui-edit.usda scratch layer selected") + '</div>')
    body += (f'<section id="outcome" class="section ui-step"><div class="ui-step__heading"><span>07</span><h2>Compare the coat</h2></div>'
             '<p>The full-coat image is a verified MoonRay capture from the recipe-generated, baked '
             '<strong>188,914-curve</strong> study. It is an outcome reference for this character, not an output '
             'of the separate Pomade first-groom lesson. The usdview screenshots show inspection and editing '
             'of the prepared scene; the final render uses its own studio lighting. '
             'The study is static and does not author skeletal deformation or simulation.</p>'
             f'{ui_visual(ui, "alab-04-final-detail", page, "usdview ALab coat detail")}'
             '<div class="note-columns"><div><h3>Before</h3>' + bare +
             '</div><div><h3>Guide-driven coat</h3>' + groom + '</div></div>' + detail +
             f'<p class="source-note">The stage and generator are <a href="{relative_url("examples/alab/stoat-groom.usda",page)}">stoat-groom.usda</a> '
             f'and <a href="{relative_url("tools/make_stoat_groom.py",page)}">make_stoat_groom.py</a>. '
             'Build and launch commands are in <a href="technical-setup.html#stoat">Technical setup</a>.</p></section>')
    body += ('<section id="next" class="section"><h2>Continue in the tool</h2>'
             '<p>The <a href="pomade-workspace.html">Pomade workspace tour</a> shows interactive guide and tube '
             'authoring on a simpler scalp. The <a href="gallery.html">operator gallery</a> shows focused '
             'results for the coat operators. <a href="operators/deform.html">Deform</a> is available for a '
             'separate animated-guide workflow; this static stoat study does not use it.</p>'
             '<p class="asset-credit">Netflix Animation Studios ALab Copyright 2025 Netflix, Inc. All rights reserved.</p></section>')
    toc = [(anchor, title) for _, anchor, title, *_ in steps] + [
        ("coat-map", "Coat structure")] + [(anchor, title) for _, anchor, title, *_ in editable] + [
        ("outcome", "Compare the coat"), ("next", "Continue")]
    return shell("Character grooming", "Learn / Character route", body, page, ops, toc)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--op", action="append",
                        help="build only this operator page; repeat to select more")
    parser.add_argument("--page", choices=("gallery",),
                        help="build one index page without rebuilding operator pages")
    args = parser.parse_args()
    if args.op and args.page:
        parser.error("--op and --page cannot be combined")
    if not OPERATORS.exists():
        raise SystemExit(f"Missing {OPERATORS}; generate the operator registry first.")
    data = json.loads(OPERATORS.read_text(encoding="utf-8"))
    ops = data.get("operators", [])
    if not ops or any(not op.get("id", "").startswith("UsdGen") for op in ops):
        raise SystemExit("Operator registry is empty or contains an invalid id")
    media = json.loads(MEDIA.read_text(encoding="utf-8")) if MEDIA.exists() else {}
    character = json.loads(CHARACTER.read_text(encoding="utf-8")) if CHARACTER.exists() else {}
    ui = json.loads(UI.read_text(encoding="utf-8")) if UI.exists() else {}
    (SITE / "operators").mkdir(parents=True, exist_ok=True)
    if args.page == "gallery":
        pages = {"gallery.html": gallery(ops, media)}
    elif args.op:
        selected = set(args.op)
        unknown = selected - {op["id"] for op in ops}
        if unknown:
            parser.error("unknown operator(s): " + ", ".join(sorted(unknown)))
        pages = {page_for(op): operator_page(op, ops, media, ui)
                 for op in ops if op["id"] in selected}
    else:
        pages = {"index.html": home(ops, media, character), "getting-started.html": getting_started(ops, ui),
                 "pomade-workspace.html": pomade_workspace(ops, ui), "technical-setup.html": technical_setup(ops),
                 "gallery.html": gallery(ops, media), "character-groom.html": character_groom(ops, character, ui)}
        pages.update({page_for(op): operator_page(op, ops, media, ui) for op in ops})
    for name, content in pages.items():
        (SITE / name).write_text(content, encoding="utf-8", newline="\n")
    print(f"Built {len(pages)} offline manual pages in {SITE}")


if __name__ == "__main__":
    main()
