#!/bin/bash
# bin/gen_schema.sh — regenerate the codeless usdGen schema plugin resources
# (plugInfo.json + generatedSchema.usda) from libs/usdGenSchema/schema.usda.
#
# Mirrors usdRig's bin/gen_schema.sh. Run this ONLY after editing
# libs/usdGenSchema/schema.usda, and review the diff before committing.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]:-$0}")/_env.sh"

usdgen_require_python
usdgen_require_usd "usdGenSchema"

cd "$GEN/libs/usdGenSchema"
"$PY" "$USD/bin/usdGenSchema" schema.usda ../../plugin/usdGenSchema/resources

# usdGenSchema intentionally omits arbitrary property customData.  Restore
# usdGen evaluation-granularity metadata from the source schema deterministically
# so generatedSchema.usda remains an equivalent authoring contract.
"$PY" restore_generated_metadata.py schema.usda ../../plugin/usdGenSchema/resources/generatedSchema.usda

# Post-process into the checked-in "resource" (data-only) form:
#  - strip the @PLUG_INFO_LIBRARY_PATH@ placeholder (the build-tree copy in
#    CMake re-adds LibraryPath + implementsComputeExtent),
#  - root the resource dir at ".",
#  - inject the AutoApplyAPISchemas block INSIDE "Info" (plan §7.4). OpenUSD
#    reads it via PlugPlugin::GetMetadata(), which returns the Info object;
#    unknown top-level plugin keys are rejected by the parser, so it must be
#    a sibling of "Types", not of "Name".
"$PY" - <<'PY'
import io, re
p = '../../plugin/usdGenSchema/resources/plugInfo.json'
s = io.open(p, encoding='utf-8').read()
s = s.replace('"LibraryPath": "@PLUG_INFO_LIBRARY_PATH@", \n', '')
s = s.replace('"LibraryPath": "@PLUG_INFO_LIBRARY_PATH@"', '')
s = s.replace('"@PLUG_INFO_RESOURCE_PATH@"', '"."')
s = s.replace('"@PLUG_INFO_ROOT@"', '"."')
lines = s.splitlines()
for i, l in enumerate(lines):
    if not re.match(r'^\s*"Types": \{$', l):
        continue
    ind = l[:len(l) - len(l.lstrip())]
    for j in range(i + 1, len(lines)):
        if lines[j] == ind + '}':
            lines[j] = ind + '},'
            lines[j + 1:j + 1] = [
                ind + '"AutoApplyAPISchemas": {',
                ind + '    "UsdGenMaskAPI": { "apiSchemaAutoApplyTo": ["UsdGenOperator"] }',
                ind + '}',
            ]
            break
    break
io.open(p, 'w', encoding='utf-8').write('\n'.join(line.rstrip() for line in lines) + '\n')
print("post-processed", p)
PY

echo "regenerated plugin/usdGenSchema/resources — review the diff before committing"
