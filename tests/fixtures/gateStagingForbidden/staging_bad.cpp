// B-2 negative fixture: forbidden stage-API include trips the fence.
// Never compiled; the include line below is the tripwire.
#include <pxr/usd/usd/stage.h>
void usdGenGateStagingForbiddenProbe();
