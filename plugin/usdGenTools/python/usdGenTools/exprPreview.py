# Hair colour preview authoring for the expression editor: the
# usdGen:preview:* properties of a UsdGenDescription.
#
# While a description's usdGen:preview:source targets something, the engine
# publishes that value as the strands' displayColor through a colour map and
# binds a flat preview material instead of the look. The source can be:
#
#   * a UsdGenExpression prim, evaluated over the published strands;
#   * a UsdGenPtexMap prim, read at every strand root as ptex() reads it;
#   * an operator attribute, showing the values that operator was cooked with
#     (or its authored value when nothing drives it).
#
# The preview is a viewing aid, not part of the groom, so everything here is
# written to the stage's SESSION layer, whatever the edit target is, and never
# saved with the layer being authored. Writes use the Sdf API inside one
# Sdf.ChangeBlock, so switching the preview is one stage notice and one recook.
#
# Qt-free, like exprAuthor, so it runs in a plain python process.

from pxr import Sdf, Usd

from . import exprAuthor

SOURCE_REL = "usdGen:preview:source"
COLOR_MAP_ATTR = "usdGen:preview:colorMap"
RANGE_ATTR = "usdGen:preview:range"
EVALUATION_ATTR = "usdGen:preview:evaluation"
SHADING_ATTR = "usdGen:preview:shading"

PTEX_MAP_TYPE = "UsdGenPtexMap"

# Display names first, then the token the schema allows.
COLOR_MAPS = (
    ("Heat", "heat"),
    ("Viridis", "viridis"),
    ("Gray", "gray"),
    ("Distinct ids", "ids"),
    ("RGB components", "rgb"),
)
EVALUATIONS = (
    ("per strand", "primitive"),
    ("per CV", "point"),
    ("per groom", "groom"),
)
SHADINGS = ("lit", "flat")

DEFAULT_RANGE = (0.0, 1.0)

# Kinds of preview source.
EXPRESSION = "expression"
MAP = "map"
ATTRIBUTE = "attribute"

_ATTRIBUTES = (
    (COLOR_MAP_ATTR, Sdf.ValueTypeNames.Token),
    (RANGE_ATTR, Sdf.ValueTypeNames.Float2),
    (EVALUATION_ATTR, Sdf.ValueTypeNames.Token),
    (SHADING_ATTR, Sdf.ValueTypeNames.Token),
)


class PreviewSource(object):
    """Something the hair of `description` (a path) can be coloured by."""

    def __init__(self, path, kind, label, description):
        self.path = Sdf.Path(str(path))
        self.kind = kind
        self.label = label
        self.description = Sdf.Path(str(description)) if description else None

    def __eq__(self, other):
        return (isinstance(other, PreviewSource) and self.path == other.path
                and self.kind == other.kind)

    def __ne__(self, other):
        return not self == other

    def __repr__(self):
        return "PreviewSource(%r, %r)" % (str(self.path), self.kind)


def MapSources(expressionPrim):
    """A source for every UsdGenPtexMap an input:<name> of the expression
    targets, in relationship order."""
    found = []
    if not expressionPrim or not expressionPrim.IsValid():
        return found
    description = exprAuthor.DescriptionOf(expressionPrim)
    if description is None:
        return found
    stage = expressionPrim.GetStage()
    inputs = exprAuthor.InputRelationships(expressionPrim)
    for name in sorted(inputs):
        for target in inputs[name]:
            prim = stage.GetPrimAtPath(target.GetPrimPath())
            if not prim or prim.GetTypeName() != PTEX_MAP_TYPE:
                continue
            source = PreviewSource(
                prim.GetPath(), MAP,
                'Ptex map "%s" (%s)' % (name, prim.GetName()),
                description.GetPath())
            if source not in found:
                found.append(source)
    return found


def AttributeSource(attr):
    """The source that shows `attr` as its operator was cooked with it, or
    None when the operator is not under a UsdGenDescription."""
    description = exprAuthor.DescriptionOf(attr.GetPrim())
    if description is None:
        return None
    connected = exprAuthor.ExpressionTarget(attr) is not None
    return PreviewSource(
        attr.GetPath(), ATTRIBUTE,
        "%s.%s (%s)" % (attr.GetPrim().GetName(), attr.GetName(),
                        "evaluated" if connected else "authored value"),
        description.GetPath())


def SourcesFor(expressionPrim, binding=None):
    """What the editor offers for `expressionPrim`: the expression itself, the
    maps it reads, and the attribute it drives."""
    sources = []
    description = None
    if expressionPrim and expressionPrim.IsValid():
        description = exprAuthor.DescriptionOf(expressionPrim)
    if description is not None:
        sources.append(PreviewSource(
            expressionPrim.GetPath(), EXPRESSION,
            "Expression %s" % expressionPrim.GetName(), description.GetPath()))
        sources.extend(MapSources(expressionPrim))
    if binding is not None and binding.IsValid():
        attribute = AttributeSource(binding)
        if attribute is not None:
            sources.append(attribute)
    return sources


def SourceForPath(stage, path, description):
    """A PreviewSource for an already authored usdGen:preview:source."""
    path = Sdf.Path(str(path))
    prim = stage.GetPrimAtPath(path.GetPrimPath())
    if path.IsPropertyPath() and prim:
        attr = prim.GetAttribute(path.name)
        if attr:
            source = AttributeSource(attr)
            if source is not None:
                return source
    if prim and prim.GetTypeName() == PTEX_MAP_TYPE:
        return PreviewSource(path, MAP, "Ptex map %s" % prim.GetName(), description)
    if prim and prim.GetTypeName() == exprAuthor.EXPRESSION_TYPE:
        return PreviewSource(path, EXPRESSION, "Expression %s" % prim.GetName(),
                             description)
    return PreviewSource(path, ATTRIBUTE if path.IsPropertyPath() else EXPRESSION,
                         str(path), description)


def DefaultEvaluation(binding):
    """Where to evaluate a previewed expression: where its binding evaluates
    it, else once per strand."""
    if binding is not None and binding.IsValid():
        return exprAuthor.EvaluationOf(binding)
    return "primitive"


def PreviewState(description):
    """The composed preview of `description`: a dict with source (Sdf.Path or
    None), colorMap, range, evaluation and shading."""
    state = {"source": None, "colorMap": "heat", "range": DEFAULT_RANGE,
             "evaluation": "primitive", "shading": "lit"}
    if not description or not description.IsValid():
        return state
    rel = description.GetRelationship(SOURCE_REL)
    targets = rel.GetForwardedTargets() if rel else []
    if targets:
        state["source"] = targets[0]
    for key, name in (("colorMap", COLOR_MAP_ATTR),
                      ("evaluation", EVALUATION_ATTR),
                      ("shading", SHADING_ATTR)):
        attr = description.GetAttribute(name)
        value = attr.Get() if attr else None
        if value:
            state[key] = str(value)
    attr = description.GetAttribute(RANGE_ATTR)
    value = attr.Get() if attr else None
    if value is not None:
        state["range"] = (float(value[0]), float(value[1]))
    return state


def IsPreviewing(description):
    return PreviewState(description)["source"] is not None


def _OverSpec(layer, path):
    spec = layer.GetPrimAtPath(path)
    if spec is None:
        spec = Sdf.CreatePrimInLayer(layer, path)
    return spec


def SetPreview(description, source, colorMap="heat", valueRange=DEFAULT_RANGE,
               evaluation="primitive", shading="lit"):
    """Colour `description`'s strands by `source` (a path). True when the
    composed stage now says so."""
    if not description or not description.IsValid():
        return False
    stage = description.GetStage()
    layer = stage.GetSessionLayer()
    if layer is None:
        return False
    values = {
        COLOR_MAP_ATTR: colorMap,
        RANGE_ATTR: (float(valueRange[0]), float(valueRange[1])),
        EVALUATION_ATTR: evaluation,
        SHADING_ATTR: shading,
    }
    with Sdf.ChangeBlock():
        primSpec = _OverSpec(layer, description.GetPath())
        relSpec = layer.GetRelationshipAtPath(
            description.GetPath().AppendProperty(SOURCE_REL))
        if relSpec is None:
            relSpec = Sdf.RelationshipSpec(primSpec, SOURCE_REL, custom=False)
        relSpec.targetPathList.explicitItems = [Sdf.Path(source)]
        for name, typeName in _ATTRIBUTES:
            attrSpec = layer.GetAttributeAtPath(
                description.GetPath().AppendProperty(name))
            if attrSpec is None:
                attrSpec = Sdf.AttributeSpec(
                    primSpec, name, typeName,
                    variability=Sdf.VariabilityUniform, declaresCustom=False)
            attrSpec.default = values[name]
    return PreviewState(description)["source"] == Sdf.Path(source)


def ClearPreview(description):
    """Stop colouring `description`'s strands. True when nothing is
    previewed afterwards.

    The session layer's own opinions go first; a preview authored in a weaker
    layer is then blocked with an explicit empty target list, still in the
    session layer, so the layer being authored stays untouched."""
    if not description or not description.IsValid():
        return True
    stage = description.GetStage()
    layer = stage.GetSessionLayer()
    if layer is None:
        return not IsPreviewing(description)
    path = description.GetPath()
    with Sdf.ChangeBlock():
        primSpec = layer.GetPrimAtPath(path)
        if primSpec is not None:
            for name in [SOURCE_REL] + [n for n, _ in _ATTRIBUTES]:
                spec = primSpec.properties.get(name)
                if spec is not None:
                    primSpec.RemoveProperty(spec)
    if not IsPreviewing(description):
        return True
    with Usd.EditContext(stage, layer):
        description.GetRelationship(SOURCE_REL).SetTargets([])
    return not IsPreviewing(description)
