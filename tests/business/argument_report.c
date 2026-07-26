#include <beacon.h>

datap arguments;
char *label;
int requested_limit;

if (__argc <= 0) {
    BeaconPrintf(CALLBACK_ERROR, "expected arguments: str, int");
}
else {
    BeaconDataParse(&arguments, __argv, __argc);
    label = BeaconDataExtract(&arguments, NULL);
    requested_limit = BeaconDataInt(&arguments);
    if (label == NULL || requested_limit < 0 || requested_limit > 1000) {
        BeaconPrintf(CALLBACK_ERROR, "invalid label or limit");
    }
    else {
        BeaconPrintf(CALLBACK_OUTPUT, "label=%s limit=%d",
            label, requested_limit);
    }
}
