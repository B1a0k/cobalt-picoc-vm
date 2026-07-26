#include <beacon.h>

void main()
{
    datap parser;
    char *label;
    int limit;
    short mode;
    WCHAR *wide_label;
    char *blob;
    int wide_bytes;
    int blob_bytes;

    if (__argc <= 0) {
        BeaconPrintf(CALLBACK_ERROR,
            "arguments: z label, i limit, s mode, Z wide, b blob");
        return;
    }
    BeaconDataParse(&parser, __argv, __argc);
    label = BeaconDataExtract(&parser, NULL);
    limit = BeaconDataInt(&parser);
    mode = BeaconDataShort(&parser);
    wide_label = (WCHAR *)BeaconDataExtract(&parser, &wide_bytes);
    blob = BeaconDataExtract(&parser, &blob_bytes);

    BeaconPrintf(CALLBACK_OUTPUT,
        "label=%s limit=%d mode=%d wide_bytes=%d wide_first=%u "
        "blob_bytes=%d blob_first=%u remaining=%d",
        label, limit, mode, wide_bytes,
        wide_bytes >= 2 ? wide_label[0] : 0,
        blob_bytes,
        blob_bytes > 0 ? (unsigned char)blob[0] : 0,
        BeaconDataLength(&parser));
}

main();
