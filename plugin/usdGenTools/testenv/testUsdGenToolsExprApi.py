# T1 â€” the parts of the usdview expression editor that need no Qt: the ctypes
# binding to the usdGenImaging C ABI, the text helpers behind the literal
# controls and the error markers, and the connection authoring.
#
# Run as:  python testUsdGenToolsExprApi.py <path to expression-width-plane.usda>
# CMake registers it as testUsdGenToolsExprApi. Nothing is written to disk: the
# stage is opened and edited in memory and never saved.

import os
import shutil
import sys
import tempfile

from pxr import Sdf, Usd

from usdGenTools import exprApi, exprAuthor, exprLibrary

FAILURES = []


def check(condition, message):
    if condition:
        print("ok: " + message)
    else:
        print("FAIL: " + message)
        FAILURES.append(message)


def testTextHelpers():
    line, column = exprApi.OffsetToLineColumn("abc\ndefg", 5)
    check((line, column) == (2, 2), "a character offset maps to line and column")
    check(exprApi.LineColumnToOffset("abc\ndefg", 2, 2) == 5,
          "line and column map back to the same offset")

    source = "$value * (0.15 + 0.85 * (1 - $t))"
    literals = exprApi.ScanLiterals(source)
    check([l.text for l in literals] == ["0.15", "0.85", "1"],
          "every numeric literal is found, in source order")
    check(literals[0].value == 0.15, "a literal carries its numeric value")
    check(all(source[l.start:l.end] == l.text for l in literals),
          "a literal's offsets select exactly its own text")
    check(exprApi.ScanLiterals("$value")[:] == [],
          "an expression without numbers has no literals")

    # Digits that are not literals must not become sliders.
    hidden = exprApi.ScanLiterals('# 12 in a comment\n"34 in a string"\n$rootP')
    check(hidden == [], "digits in comments, strings and names are not literals")

    labelled = exprApi.ScanLiterals("clamp($value, 0, 0.15)")
    check(labelled[1].label == "clamp(..., ..., 0.15)",
          "a literal inside a call is labelled with the call")
    check(labelled[0].label == "clamp(..., 0, ...)",
          "the label says which argument the literal is")
    loose = exprApi.ScanLiterals("$value * 0.8")
    check("0.8" in loose[0].label and "clamp" not in loose[0].label,
          "a literal outside a call falls back to its surrounding text")

    check(exprApi.FormatLiteral(0.5, "1") == "0.5",
          "an integer literal widens when the value stops being whole")
    check(exprApi.FormatLiteral(3.0, "1") == "3",
          "an integer literal stays an integer while the value is whole")
    check(exprApi.FormatLiteral(2.0, "1.0") == "2.0",
          "a decimal literal stays a decimal")
    check(exprApi.DefaultRange(0.15) == (0.0, 1.0),
          "a literal in [0, 1] keeps that range")
    check(exprApi.DefaultRange(4.0) == (0.0, 8.0),
          "a larger literal gets [0, 2x]")
    check(exprApi.DefaultRange(-2.0) == (-4.0, 0.0),
          "a negative literal gets [2x, 0]")


def testApi():
    api = exprApi.GetApi()
    check(api.available,
          "the usdGenImaging C ABI loads (%s)"
          % (api.libraryPath or api.unavailableReason))
    if not api.available:
        return

    functions = api.functions()
    check(bool(functions), "the ABI reports the functions the engine accepts")
    names = [f.name for f in functions]
    check("clamp" in names and "?:" in names,
          "the function list carries clamp and the ternary operator")
    check(all(f.signature and f.doc for f in functions),
          "every function carries a signature and a doc line")

    variables = api.variables(exprApi.DOMAIN_GROOM)
    byName = dict((v.name, v) for v in variables)
    check("$t" in byName and "$value" in byName,
          "the variable list carries $t and $value")
    check(not byName["$t"].valid,
          "$t is listed but marked invalid for the groom domain")
    check(byName["$seed"].valid, "$seed is valid for the groom domain")
    check(api.variables(exprApi.DOMAIN_POINT)[
              [v.name for v in api.variables(exprApi.DOMAIN_POINT)].index("$t")].valid,
          "$t is valid for the point domain")
    check(byName["$P"].components == 3, "$P reports three components")

    check(api.compile("$value * (1 - $t)", exprApi.DOMAIN_POINT, 1) == [],
          "a good expression compiles clean")
    diagnostics = api.compile("$value * (1 - ", exprApi.DOMAIN_POINT, 1)
    check(bool(diagnostics), "a bad expression produces diagnostics")
    check(diagnostics[0].line >= 1 and diagnostics[0].column >= 1,
          "a diagnostic carries a line and a column")
    check(bool(diagnostics[0].message), "a diagnostic carries a message")
    check(bool(api.compile("$value * $t", exprApi.DOMAIN_GROOM, 1)),
          "a variable outside its domain is refused")


def testMissingLibrary():
    """Without usdGenImaging the editor must degrade to a plain text editor
    that says why, not raise out of the dock's constructor."""
    names, directories = exprApi._libraryNames, exprApi._searchDirectories
    exprApi._libraryNames = lambda: ["usdGenNoSuchLibrary"]
    exprApi._searchDirectories = lambda: []
    try:
        api = exprApi.ExpressionApi()
    finally:
        exprApi._libraryNames = names
        exprApi._searchDirectories = directories

    check(not api.available, "a missing library is reported, not raised")
    check("usdGenNoSuchLibrary" in api.unavailableReason,
          "the reason names what was tried")
    check(api.variables() == [] and api.functions() == [],
          "the browsers get empty lists rather than an exception")
    raised = False
    try:
        api.compile("$value", exprApi.DOMAIN_POINT, 1)
    except RuntimeError:
        raised = True
    check(raised, "compiling without a library raises for the caller to report")


def testAuthoring(stagePath):
    stage = Usd.Stage.Open(stagePath)
    check(stage is not None, "the expression example opens")
    width = stage.GetPrimAtPath("/World/Groom/Fur/Ops/width")
    check(bool(width), "the Width operator is there")

    description = exprAuthor.DescriptionOf(width)
    check(description is not None and description.GetName() == "Fur",
          "the enclosing UsdGenDescription is found")

    attributeNames = [a.GetName() for a in exprAuthor.ConnectableAttributes(width)]
    check("usdGen:width" in attributeNames,
          "usdGen:width is offered for connection")
    check("outputs:result" not in attributeNames,
          "outputs are not offered as connection sources")

    widthAttr = width.GetAttribute("usdGen:width")
    check(exprAuthor.EvaluationOf(widthAttr) == "point",
          "usdGen:width evaluates per point")
    seed = stage.GetPrimAtPath("/World/Groom/Fur/Ops/grow").GetAttribute("usdGen:seed")
    check(exprAuthor.EvaluationOf(seed) == "groom", "usdGen:seed evaluates per groom")

    target = exprAuthor.ExpressionTarget(widthAttr)
    check(target is not None and target.GetName() == "rootTipWidth",
          "an outputs:result connection resolves to the expression prim")
    mask = stage.GetPrimAtPath("/World/Groom/Fur/Ops/frizz").GetAttribute("usdGen:mask")
    check(exprAuthor.ExpressionTarget(mask).GetName() == "strandMask",
          "a prim-path connection resolves to the expression prim too")

    check([a.GetPath() for a in exprAuthor.BindingsOf(target)] ==
          [widthAttr.GetPath()],
          "the expression's binding is found from the expression prim")

    # --- connect ---------------------------------------------------------
    magnitude = stage.GetPrimAtPath(
        "/World/Groom/Fur/Ops/frizz").GetAttribute("usdGen:noise:magnitude")
    check(exprAuthor.SuggestExpressionName(magnitude) == "noiseMagnitudeExpr",
          "a new expression gets a readable default name")
    expression = exprAuthor.ConnectToExpression(magnitude)
    check(expression and expression.IsValid(), "Connect creates an expression prim")
    check(expression.GetTypeName() == "UsdGenExpression",
          "the new prim is typed UsdGenExpression")
    check(expression.GetPath().GetParentPath() ==
          Sdf.Path("/World/Groom/Fur/Expressions"),
          "the new expression goes under the description's Expressions scope")
    check(expression.GetAttribute("usdGen:expr:source").Get() == "$value",
          "the new expression passes the attribute's own value through")
    result = expression.GetAttribute("outputs:result")
    check(result and result.GetTypeName() == magnitude.GetTypeName(),
          "outputs:result matches the type of the attribute it drives")
    connections = magnitude.GetConnections()
    check(connections == [expression.GetPath()],
          "the connection is authored by PRIM path")
    # The schema declares usdGen:noise:magnitude as a per-CV attribute, so
    # Connect must carry that onto the destination rather than assume one.
    check(magnitude.GetCustomDataByKey("usdGen:evaluation") == "point",
          "the evaluation customData is authored on the destination")
    check(exprAuthor.ExpressionTarget(magnitude) == expression,
          "the new connection resolves back to the expression")

    # A control attribute the schema evaluates once per groom must not be
    # given the per-CV domain of the attribute above.
    growSeed = stage.GetPrimAtPath("/World/Groom/Fur/Ops/grow").GetAttribute(
        "usdGen:seed")
    exprAuthor.ConnectToExpression(growSeed)
    check(growSeed.GetCustomDataByKey("usdGen:evaluation") == "groom",
          "a per-groom control attribute is connected as a groom expression")

    # A second connect on the same attribute must not collide with the first.
    frequency = stage.GetPrimAtPath(
        "/World/Groom/Fur/Ops/frizz").GetAttribute("usdGen:noise:frequency")
    again = exprAuthor.ConnectToExpression(frequency, name="noiseMagnitudeExpr")
    check(again.GetPath() != expression.GetPath(),
          "a name already in use is made unique rather than overwritten")

    reused = exprAuthor.ConnectToExpression(
        stage.GetPrimAtPath("/World/Groom/Fur/Ops/frizz").GetAttribute(
            "usdGen:noise:octaves"),
        expressionPath=expression.GetPath())
    check(reused.GetPath() == expression.GetPath(),
          "an existing expression can be reused instead of making a new one")

    existing = exprAuthor.ExistingExpressions(description, magnitude.GetTypeName())
    check(expression in existing,
          "the new expression is offered for reuse by matching output type")

    # --- evaluation domain ------------------------------------------------
    check(exprAuthor.SetEvaluation(magnitude, "point"),
          "the evaluation domain can be rewritten")
    check(magnitude.GetCustomDataByKey("usdGen:evaluation") == "point",
          "the rewritten evaluation domain is on the stage")
    check(not exprAuthor.SetEvaluation(magnitude, "nonsense"),
          "an unknown evaluation domain is refused")

    # --- disconnect -------------------------------------------------------
    check(exprAuthor.DisconnectExpression(magnitude), "Disconnect reports success")
    check(magnitude.GetConnections() == [],
          "the connection is gone from the stage")
    check(exprAuthor.ExpressionTarget(magnitude) is None,
          "the attribute no longer resolves to an expression")
    check(exprAuthor.DisconnectExpression(widthAttr),
          "an outputs:result connection can be removed as well")
    check(widthAttr.GetConnections() == [],
          "the outputs:result connection is gone")

    # --- components -------------------------------------------------------
    check(exprAuthor.OutputComponents(Sdf.ValueTypeNames.Float) == 1,
          "a float destination needs one component")
    check(exprAuthor.OutputComponents(Sdf.ValueTypeNames.Float3) == 3,
          "a float3 destination needs three components")


def testSessionLayerEditTarget(stagePath):
    """usdview's own edit target is the session layer, so every authoring path
    has to work when the opinion it is changing lives in a weaker layer."""
    stage = Usd.Stage.Open(stagePath)
    stage.SetEditTarget(Usd.EditTarget(stage.GetSessionLayer()))
    root = stage.GetRootLayer()

    width = stage.GetPrimAtPath("/World/Groom/Fur/Ops/width").GetAttribute(
        "usdGen:width")
    check(len(width.GetConnections()) == 1,
          "the width connection starts out in the root layer")
    check(exprAuthor.DisconnectExpression(width),
          "Disconnect works against a connection in a weaker layer")
    check(width.GetConnections() == [],
          "the weaker layer's connection is blocked, not merely unedited")
    check(len(root.GetAttributeAtPath(width.GetPath()).connectionPathList
              .explicitItems) == 1,
          "the root layer is left untouched by the session-layer edit")

    expression = exprAuthor.ConnectToExpression(width, name="taperWidth")
    check(width.GetConnections() == [expression.GetPath()],
          "Connect works from the session layer too")
    check(stage.GetSessionLayer().GetPrimAtPath(expression.GetPath())
          is not None,
          "the new expression is authored into the edit target's layer")
    check(width.GetCustomDataByKey("usdGen:evaluation") == "point",
          "the evaluation customData is authored from the session layer")





CONTROL_SOURCE = (
    "$tip = 0.15; # 0, 1\n"
    "$segments = 4; # 1, 10\n"
    "$tint = [1, 0.5, 0.2]; # color\n"
    "$offset = [0, 1, 0];\n"
    "$profile = curve($t, 0, 1, 4, 0.5, 0.7, 4, 1, 0, 4); # curve\n"
    "$ramp = ccurve($t, 0, [1, 0, 0], 4, 1, [0, 0, 1], 4); # ccurve\n"
    "$label = \"root\"; # string\n"
    "$computed = $tip * 2;\n"
    "$value * $tip * $profile\n")


def controlNamed(controls, name):
    for control in controls:
        if control.name == name:
            return control
    return None


def testControlGrammar():
    controls = exprApi.ScanControls(CONTROL_SOURCE)
    kinds = [(c.name, c.kind) for c in controls]
    check(kinds == [("$tip", "float"), ("$segments", "int"),
                    ("$tint", "color"), ("$offset", "vector"),
                    ("$profile", "curve"), ("$ramp", "ccurve"),
                    ("$label", "string")],
          "one control per top-level assignment, typed by its value and "
          "its comment")
    check(controlNamed(controls, "$computed") is None,
          "a computed right-hand side gets no control")

    check(all(CONTROL_SOURCE[c.valueStart:c.valueEnd] == c.valueText
              for c in controls),
          "a control's offsets select exactly its own value text")

    tip = controlNamed(controls, "$tip")
    check((tip.data["minimum"], tip.data["maximum"]) == (0.0, 1.0),
          "the `# 0, 1` comment sets the slider range")
    check(tip.data["value"] == 0.15, "the control carries its value")

    segments = controlNamed(controls, "$segments")
    check((segments.data["minimum"], segments.data["maximum"]) == (1.0, 10.0),
          "an integer control takes its range from the comment too")

    # No comment at all: SeExpr's [0, 1], widened only if the value needs it.
    check(exprApi.ScanControls("$g = 0.25")[0].data["minimum"] == 0.0 and
          exprApi.ScanControls("$g = 0.25")[0].data["maximum"] == 1.0,
          "a number with no comment gets the default [0, 1] range")
    wide = exprApi.ScanControls("$g = 4.0")[0]
    check((wide.data["minimum"], wide.data["maximum"]) == (0.0, 4.0),
          "the default range widens far enough to contain the value")

    check(controlNamed(controls, "$tint").data["values"] == (1.0, 0.5, 0.2),
          "a colour control carries its components")
    check(exprApi.ScanControls("$c = [1, 0.5, 0.2]")[0].kind == "vector",
          "a vector without `# color` stays a plain vector")

    profile = controlNamed(controls, "$profile")
    check(profile.data["lookup"] == "$t", "a curve remembers what looks it up")
    check(profile.data["knots"] == [(0.0, 1.0, 4), (0.5, 0.7, 4), (1.0, 0.0, 4)],
          "a curve's knots are read as (position, value, interpolation)")
    ramp = controlNamed(controls, "$ramp")
    check(ramp.data["knots"][0][1] == (1.0, 0.0, 0.0),
          "a colour curve's knot values are colours")
    check(controlNamed(controls, "$label").data["value"] == "root",
          "a string control carries the text without its quotes")

    # A curve whose knots are not literals cannot be driven by a widget, and
    # saying so beats showing a widget that would rewrite the text wrongly.
    check(exprApi.ScanControls("$p = curve($t, 0, $x, 4, 1, 0, 4);") == [],
          "a curve with a computed knot yields no control")
    check(exprApi.ScanControls("$p = curve($t, 0, 1, 4, 0.5, 0.7);") == [],
          "a curve with an incomplete knot triple yields no control")

    # Statement scanning has to survive the things that look like separators.
    inString = exprApi.ScanControls('$a = 1; $b = "x; y"; # string\n')
    check([c.name for c in inString] == ["$a", "$b"],
          "a semicolon inside a string does not split a statement")
    check(exprApi.ScanControls("$f = clamp($v, 0, 1);") == [],
          "a call on the right-hand side is not mistaken for a value")
    leading = exprApi.ScanControls("# a header\n$x = 1; # 0, 2\n")
    check(len(leading) == 1 and leading[0].data["maximum"] == 2.0,
          "a header comment above a statement does not hide it")


def testControlRewriting():
    controls = exprApi.ScanControls(CONTROL_SOURCE)

    def rewrite(control, value):
        return (CONTROL_SOURCE[:control.valueStart]
                + exprApi.FormatControlValue(control, value)
                + CONTROL_SOURCE[control.valueEnd:])

    tint = controlNamed(controls, "$tint")
    changed = rewrite(tint, (0.2, 0.3, 0.4))
    check("$tint = [0.2, 0.3, 0.4]; # color" in changed,
          "a colour control rewrites exactly its own value")
    check(changed.replace("[0.2, 0.3, 0.4]", "[1, 0.5, 0.2]") == CONTROL_SOURCE,
          "and leaves every other character of the expression alone")

    profile = controlNamed(controls, "$profile")
    knots = list(profile.data["knots"])
    knots[1] = (0.5, 0.25, exprApi.INTERP_SMOOTH)
    check("curve($t, 0.0, 1.0, 4, 0.5, 0.25, 2, 1.0, 0.0, 4)"
          in rewrite(profile, knots),
          "moving a knot rewrites the knot list in place")

    ramp = controlNamed(controls, "$ramp")
    colourKnots = list(ramp.data["knots"])
    colourKnots[0] = (0.0, (0.0, 1.0, 0.0), 4)
    check("ccurve($t, 0.0, [0.0, 1.0, 0.0], 4," in rewrite(ramp, colourKnots),
          "a colour knot rewrites as a vector literal")

    segments = controlNamed(controls, "$segments")
    check("$segments = 7;" in rewrite(segments, 7.0),
          "an integer control stays integral")
    check("$label = \"tip\";" in rewrite(controlNamed(controls, "$label"), "tip"),
          "a string control rewrites as a quoted literal")

    spans = exprApi.ControlSpans(controls)
    check(all(CONTROL_SOURCE[a:b] == c.valueText
              for (a, b), c in zip(spans, controls)),
          "the control spans are the value spans the literal fallback excludes")


def testDeclarations():
    check(exprApi.DeclarationFor("$gain", "float", 0.5, 0.0, 2.0)
          == "$gain = 0.5; # 0.0, 2.0",
          "a float declaration carries its range in the comment")
    check(exprApi.DeclarationFor("$n", "int", 3, 1, 10) == "$n = 3; # 1, 10",
          "an integer declaration keeps the range integral")
    check(exprApi.DeclarationFor("$c", "color", (1.0, 1.0, 1.0))
          == "$c = [1.0, 1.0, 1.0]; # color",
          "a colour declaration is annotated as a colour")
    check(exprApi.DeclarationFor("$s", "string", "hi") == '$s = "hi"; # string',
          "a string declaration quotes its value")
    declared = exprApi.DeclarationFor(
        "$cv", "curve", [(0.0, 0.0, 4), (1.0, 1.0, 4)])
    check(declared == "$cv = curve($t, 0.0, 0.0, 4, 1.0, 1.0, 4); # curve",
          "a curve declaration writes a two-knot curve")
    # Every declaration must parse back into the control it describes.
    for kind, value in (("float", 0.5), ("int", 3), ("color", (1.0, 0.0, 0.0)),
                        ("vector", (0.0, 1.0, 0.0)), ("string", "hi"),
                        ("curve", [(0.0, 0.0, 4), (1.0, 1.0, 4)]),
                        ("ccurve", [(0.0, (0.0, 0.0, 0.0), 4),
                                    (1.0, (1.0, 1.0, 1.0), 4)])):
        line = exprApi.DeclarationFor("$w", kind, value, 0, 10)
        parsed = exprApi.ScanControls(line + "\n$value")
        check(len(parsed) == 1 and parsed[0].kind == kind,
              "the %s declaration parses back as a %s control" % (kind, kind))


def testCurveEvaluation():
    ramp = [(0.0, 0.0, exprApi.INTERP_LINEAR), (1.0, 1.0, exprApi.INTERP_LINEAR)]
    check(exprApi.EvaluateCurve(ramp, 0.25) == 0.25,
          "a linear segment interpolates linearly")
    check(exprApi.EvaluateCurve(ramp, -1.0) == 0.0 and
          exprApi.EvaluateCurve(ramp, 2.0) == 1.0,
          "a curve is constant outside its knot range")

    step = [(0.0, 0.0, exprApi.INTERP_NONE), (1.0, 1.0, exprApi.INTERP_NONE)]
    check(exprApi.EvaluateCurve(step, 0.9) == 0.0,
          "interpolation `none` holds the left knot's value")

    smooth = [(0.0, 0.0, exprApi.INTERP_SMOOTH),
              (1.0, 1.0, exprApi.INTERP_SMOOTH)]
    check(abs(exprApi.EvaluateCurve(smooth, 0.5) - 0.5) < 1e-9 and
          exprApi.EvaluateCurve(smooth, 0.25) < 0.25,
          "a smooth segment eases in")

    # The interpolation code travels with the knot that ENDS the segment,
    # which is how SeExpr's Curve reads it; the second knot decides here.
    mixed = [(0.0, 0.0, exprApi.INTERP_NONE), (1.0, 1.0, exprApi.INTERP_LINEAR)]
    check(exprApi.EvaluateCurve(mixed, 0.5) == 0.5,
          "the segment takes its interpolation from its right-hand knot")

    hump = [(0.0, 0.0, 4), (0.5, 1.0, 4), (1.0, 0.0, 4)]
    samples = [exprApi.EvaluateCurve(hump, x / 20.0) for x in range(21)]
    check(max(samples) <= 1.0 + 1e-9,
          "a monotone segment does not overshoot its knots")

    colour = [(0.0, (1.0, 0.0, 0.0), 1), (1.0, (0.0, 0.0, 1.0), 1)]
    middle = exprApi.EvaluateCurve(colour, 0.5)
    check(tuple(round(c, 6) for c in middle) == (0.5, 0.0, 0.5),
          "a colour curve interpolates each component")


def testLibrary():
    directory = tempfile.mkdtemp(prefix="usdGenExpr")
    try:
        os.environ[exprLibrary.PATH_ENVIRONMENT] = directory
        check(exprLibrary.UserDirectory() == os.path.abspath(directory),
              "USDGEN_EXPRESSION_PATH names the library saves go to")

        text = "$tip = 0.2; # 0, 1\n$value * $tip\n"
        path = exprLibrary.Save("my taper", text, domain="point")
        check(os.path.basename(path) == "my_taper.se",
              "a name with a space becomes one safe file name")
        check(exprLibrary.DomainOf(exprLibrary.Load(path)) == "point",
              "the evaluation domain is saved with the expression")
        check("$tip = 0.2" in exprLibrary.Load(path),
              "the expression itself round-trips")

        entries = exprLibrary.ListExpressions()
        byName = dict((e.name, e) for e in entries)
        check("my_taper" in byName, "the saved expression is listed")
        check(byName["my_taper"].library == "user",
              "it is listed in the writable user library")
        check("rootTipTaper" in byName,
              "the shipped presets are found beside the staged package")
        check(byName["rootTipTaper"].library == "usdGen",
              "the presets are listed as the read-only usdGen library")
        check(byName["rootTipTaper"].domain == "point",
              "a preset reports the domain it was written for")

        # Saving twice must overwrite rather than accumulate.
        exprLibrary.Save("my taper", "$value\n", domain="groom")
        again = dict((e.name, e) for e in exprLibrary.ListExpressions())
        check(again["my_taper"].domain == "groom",
              "saving again replaces the expression and its domain marker")
        check(len([e for e in exprLibrary.ListExpressions()
                   if e.name == "my_taper"]) == 1,
              "and leaves exactly one file behind")

        check(exprLibrary.SetDomain("# domain: point\n$value", "groom")
              == "# domain: groom\n$value",
              "setting the domain replaces the marker rather than stacking one")
        check(exprLibrary.DomainOf("$value * 2") == "",
              "an expression with no marker suits any domain")
    finally:
        os.environ.pop(exprLibrary.PATH_ENVIRONMENT, None)
        shutil.rmtree(directory, ignore_errors=True)


def testFunctionCategories():
    check(exprApi.FunctionCategory("sin") == "Trigonometry",
          "the name-based fallback files sin under Trigonometry")
    check(exprApi.FunctionCategory("curve") == "Curves",
          "and curve under Curves")
    check(exprApi.FunctionCategory("nosuchfunction") == "Other",
          "an unknown name falls into the leftovers bin")

    api = exprApi.GetApi()
    if api.available:
        functions = api.functions()
        check(all(f.category for f in functions),
              "every function the ABI reports carries a category")
        check(any(f.category == "Operators" for f in functions),
              "the operators are grouped apart from the functions")


def main():
    if len(sys.argv) < 2:
        print("FAIL: expected the path of expression-width-plane.usda")
        return 1
    testTextHelpers()
    testControlGrammar()
    testControlRewriting()
    testDeclarations()
    testCurveEvaluation()
    testLibrary()
    testFunctionCategories()
    testApi()
    testMissingLibrary()
    testAuthoring(sys.argv[1])
    testSessionLayerEditTarget(sys.argv[1])
    if FAILURES:
        print("FAIL: %d check(s) failed" % len(FAILURES))
        return 1
    print("PASS: usdGenTools expression API and authoring")
    return 0


if __name__ == "__main__":
    sys.exit(main())
