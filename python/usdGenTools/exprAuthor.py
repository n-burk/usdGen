# Connection authoring for the expression editor: everything the "Connect to
# expression" / "Disconnect" / domain-combo actions do to a stage.
#
# Kept free of Qt so the authoring can be exercised in a plain python process,
# and kept free of the editor's state so each entry point is a pure function of
# (stage object, arguments).
#
# Every write goes through the stage's CURRENT EDIT TARGET and is made with the
# Sdf API inside one Sdf.ChangeBlock per action, so a connect is a single
# stage notice rather than a burst of four, and the groom scene index recooks
# once. (Usd API calls are deliberately not made inside the block: the stage is
# not resynced until it closes, so only layer-level editing is safe there.)

from pxr import Sdf, Usd

EXPRESSION_TYPE = "UsdGenExpression"
DESCRIPTION_TYPE = "UsdGenDescription"
SOURCE_ATTR = "usdGen:expr:source"
RESULT_ATTR = "outputs:result"
OUTPUT_PREFIX = "outputs:"
EXPRESSIONS_SCOPE = "Expressions"
EVALUATION_KEY = "usdGen:evaluation"

# The default expression body for a newly connected attribute: pass the
# literal through unchanged, so connecting on its own never changes the groom.
DEFAULT_SOURCE = "$value"

# Attribute types worth driving with a scalar expression. Vector-valued
# attributes are excluded: the frontend lowers up to four components, but
# nothing in the operator set consumes a connected vector yet.
CONNECTABLE_TYPES = (
    Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Double,
    Sdf.ValueTypeNames.Half, Sdf.ValueTypeNames.Int,
    Sdf.ValueTypeNames.UInt, Sdf.ValueTypeNames.Bool,
)

# Where an expression is evaluated when the attribute carries no
# usdGen:evaluation customData at all. The schema declares that customData on
# every operator attribute (point for usdGen:width, primitive for usdGen:mask,
# groom for enabled/seed/segments/cvCount and the rest of the control names),
# so this table is only the backstop for an attribute the schema does not
# describe -- a custom one, or a schema older than the plugin.
_EVALUATION_FALLBACK = {
    "usdGen:width": "point",
    "usdGen:mask": "primitive",
}
DEFAULT_EVALUATION = "groom"

# Components an expression must produce to fill an attribute of each type.
_COMPONENTS = {
    "float": 1, "double": 1, "half": 1, "int": 1, "uint": 1, "int64": 1,
    "uint64": 1, "bool": 1, "timecode": 1,
    "float2": 2, "double2": 2, "half2": 2, "int2": 2, "texCoord2f": 2,
    "float3": 3, "double3": 3, "half3": 3, "int3": 3, "color3f": 3,
    "vector3f": 3, "normal3f": 3, "point3f": 3, "texCoord3f": 3,
    "float4": 4, "double4": 4, "half4": 4, "int4": 4, "color4f": 4,
    "quatf": 4,
}


def OutputComponents(typeName):
    """How many components an expression feeding `typeName` must produce."""
    return _COMPONENTS.get(str(typeName), 1)


def DescriptionOf(prim):
    """The UsdGenDescription `prim` lives under, or None."""
    while prim and prim.IsValid() and not prim.IsPseudoRoot():
        if prim.GetTypeName() == DESCRIPTION_TYPE:
            return prim
        prim = prim.GetParent()
    return None


def EvaluationOf(attr):
    """The evaluation domain of `attr`: authored customData, else the schema's
    fallback for it, else this module's table, else groom."""
    if attr and attr.IsValid():
        value = attr.GetCustomDataByKey(EVALUATION_KEY)
        if value:
            return str(value)
        return _EVALUATION_FALLBACK.get(attr.GetName(), DEFAULT_EVALUATION)
    return DEFAULT_EVALUATION


def ConnectableAttributes(prim):
    """The scalar attributes of `prim` an expression may drive, in name order."""
    if not prim or not prim.IsValid():
        return []
    found = [a for a in prim.GetAttributes()
             if not a.GetName().startswith(OUTPUT_PREFIX)
             and a.GetTypeName() in CONNECTABLE_TYPES]
    return sorted(found, key=lambda a: a.GetName())


def ExpressionTarget(attr):
    """The UsdGenExpression prim `attr` is connected to, or None.

    A connection may name the expression PRIM or one of its outputs; both
    spellings are valid and both resolve to the same binding."""
    if not attr or not attr.IsValid():
        return None
    stage = attr.GetPrim().GetStage()
    for target in attr.GetConnections():
        if target.IsPropertyPath():
            if not target.name.startswith(OUTPUT_PREFIX):
                continue
        elif not target.IsPrimPath():
            continue
        prim = stage.GetPrimAtPath(target.GetPrimPath())
        if prim and prim.GetTypeName() == EXPRESSION_TYPE:
            return prim
    return None


def BindingsOf(expressionPrim):
    """Every attribute in the enclosing description connected to this
    expression, so the editor can find the destination whose evaluation
    customData chooses the domain even when the expression prim is what the
    user selected."""
    description = DescriptionOf(expressionPrim)
    if not description:
        return []
    found = []
    for prim in Usd.PrimRange(description):
        if prim.GetTypeName() == EXPRESSION_TYPE:
            continue
        for attr in prim.GetAttributes():
            if ExpressionTarget(attr) == expressionPrim:
                found.append(attr)
    return found


def ExistingExpressions(description, typeName=None):
    """The UsdGenExpression prims under `description`/Expressions, optionally
    only those whose outputs:result already matches `typeName`."""
    if not description or not description.IsValid():
        return []
    scope = description.GetStage().GetPrimAtPath(
        description.GetPath().AppendChild(EXPRESSIONS_SCOPE))
    if not scope or not scope.IsValid():
        return []
    found = []
    for child in scope.GetChildren():
        if child.GetTypeName() != EXPRESSION_TYPE:
            continue
        if typeName is not None:
            result = child.GetAttribute(RESULT_ATTR)
            if not result or result.GetTypeName() != typeName:
                continue
        found.append(child)
    return found


def _UniqueName(scopePath, stage, base):
    name = base if Sdf.Path.IsValidIdentifier(base) else "expression"
    candidate = name
    index = 1
    while stage.GetPrimAtPath(scopePath.AppendChild(candidate)):
        index += 1
        candidate = "%s%d" % (name, index)
    return candidate


def SuggestExpressionName(attr):
    """A readable default name for the expression driving `attr`, e.g.
    usdGen:noise:magnitude -> noiseMagnitudeExpr."""
    parts = [p for p in attr.GetName().split(":") if p and p != "usdGen"]
    if not parts:
        return "expression"
    name = parts[0] + "".join(p[:1].upper() + p[1:] for p in parts[1:])
    return name + "Expr"


def _PrimSpec(layer, path, typeName=None):
    spec = layer.GetPrimAtPath(path)
    if spec is None:
        spec = Sdf.CreatePrimInLayer(layer, path)
    if spec.specifier != Sdf.SpecifierDef:
        spec.specifier = Sdf.SpecifierDef
    if typeName and spec.typeName != typeName:
        spec.typeName = typeName
    return spec


def _AttrSpec(layer, primSpec, name, typeName, custom=False):
    path = primSpec.path.AppendProperty(name)
    spec = layer.GetAttributeAtPath(path)
    if spec is None:
        spec = Sdf.AttributeSpec(primSpec, name, typeName,
                                 declaresCustom=custom)
    return spec


def ConnectToExpression(attr, name=None, expressionPath=None,
                        source=DEFAULT_SOURCE, evaluation=None):
    """Drive `attr` from a UsdGenExpression, creating one if needed.

    Returns the expression prim. The connection is authored by PRIM path (the
    spelling both graph-desc builders resolve), the expression declares a typed
    outputs:result matching `attr`, and `attr` gets the evaluation customData
    that says where the expression runs.

    Raises ValueError when `attr` is not inside a UsdGenDescription, since
    there is nowhere sensible to put the expression then."""
    prim = attr.GetPrim()
    stage = prim.GetStage()
    description = DescriptionOf(prim)
    if description is None:
        raise ValueError("%s is not under a %s; there is no Expressions scope "
                         "to author into" % (attr.GetPath(), DESCRIPTION_TYPE))

    scopePath = description.GetPath().AppendChild(EXPRESSIONS_SCOPE)
    creating = expressionPath is None
    if creating:
        expressionPath = scopePath.AppendChild(
            _UniqueName(scopePath, stage, name or SuggestExpressionName(attr)))
    else:
        expressionPath = Sdf.Path(expressionPath)

    if evaluation is None:
        evaluation = EvaluationOf(attr)

    editTarget = stage.GetEditTarget()
    layer = editTarget.GetLayer()
    typeName = attr.GetTypeName()

    with Sdf.ChangeBlock():
        if creating:
            _PrimSpec(layer, editTarget.MapToSpecPath(scopePath), "Scope")
            expressionSpec = _PrimSpec(
                layer, editTarget.MapToSpecPath(expressionPath),
                EXPRESSION_TYPE)
            sourceSpec = _AttrSpec(layer, expressionSpec, SOURCE_ATTR,
                                   Sdf.ValueTypeNames.String, custom=True)
            sourceSpec.default = source
            _AttrSpec(layer, expressionSpec, RESULT_ATTR, typeName, custom=True)

        destinationSpec = _AttrSpec(
            layer, _PrimSpec(layer, editTarget.MapToSpecPath(prim.GetPath()),
                             prim.GetTypeName()),
            attr.GetName(), typeName, custom=attr.IsCustom())
        destinationSpec.connectionPathList.explicitItems = [expressionPath]
        customData = dict(destinationSpec.customData or {})
        usdGenData = dict(customData.get("usdGen", {}))
        usdGenData["evaluation"] = evaluation
        customData["usdGen"] = usdGenData
        destinationSpec.customData = customData

    return stage.GetPrimAtPath(expressionPath)


def DisconnectExpression(attr):
    """Remove `attr`'s expression connection. True when it is gone afterwards.

    The edit layer's own opinion is cleared first. That is enough when the
    connection was authored there, which is the usual case; in usdview, where
    the edit target defaults to the session layer, the connection lives in a
    weaker layer and clearing a layer that never had the opinion does nothing.
    An explicit empty list op then blocks the weaker one -- authored through
    the Usd API, because Sdf's connectionPathList proxy writes NOTHING for an
    empty explicit list, leaving the attribute connected in silence."""
    if not attr or not attr.IsValid():
        return False
    stage = attr.GetPrim().GetStage()
    editTarget = stage.GetEditTarget()
    layer = editTarget.GetLayer()
    specPath = editTarget.MapToSpecPath(attr.GetPath())

    with Sdf.ChangeBlock():
        spec = layer.GetAttributeAtPath(specPath)
        if spec is not None:
            spec.connectionPathList.ClearEdits()

    if not attr.GetConnections():
        return True
    attr.SetConnections([])
    return not attr.GetConnections()


def SetEvaluation(attr, evaluation):
    """Author `attr`'s usdGen:evaluation customData. True when it took."""
    if not attr or not attr.IsValid() or evaluation not in (
            "groom", "primitive", "point"):
        return False
    stage = attr.GetPrim().GetStage()
    editTarget = stage.GetEditTarget()
    layer = editTarget.GetLayer()

    with Sdf.ChangeBlock():
        primSpec = _PrimSpec(layer,
                             editTarget.MapToSpecPath(attr.GetPrim().GetPath()),
                             attr.GetPrim().GetTypeName())
        spec = _AttrSpec(layer, primSpec, attr.GetName(), attr.GetTypeName(),
                         custom=attr.IsCustom())
        customData = dict(spec.customData or {})
        usdGenData = dict(customData.get("usdGen", {}))
        usdGenData["evaluation"] = evaluation
        customData["usdGen"] = usdGenData
        spec.customData = customData
    return EvaluationOf(attr) == evaluation
