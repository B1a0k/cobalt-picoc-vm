#include <windows.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <beacon.h>

#define SCRIPT_LMEM_FIXED 0
#define SCRIPT_TCP_TABLE_OWNER_PID_ALL 5

typedef struct _SCRIPT_TCP_ROW {
    DWORD state;
    DWORD local_address;
    DWORD local_port;
    DWORD remote_address;
    DWORD remote_port;
    DWORD owning_pid;
} SCRIPT_TCP_ROW;

typedef struct _SCRIPT_TCP_TABLE {
    DWORD entry_count;
    SCRIPT_TCP_ROW rows[1];
} SCRIPT_TCP_TABLE;

DWORD table_size;
DWORD status;
SCRIPT_TCP_TABLE *table;
DWORD index;
DWORD listening;
DWORD established;

table_size = 0;
table = NULL;
status = GetExtendedTcpTable(NULL, &table_size, FALSE, AF_INET,
    SCRIPT_TCP_TABLE_OWNER_PID_ALL, 0);
if (status != ERROR_INSUFFICIENT_BUFFER) {
    BeaconPrintf(CALLBACK_ERROR,
        "TCP table sizing failed status=%lu", status);
}
else {
    table = (SCRIPT_TCP_TABLE *)LocalAlloc(
        SCRIPT_LMEM_FIXED, table_size);
    if (table == NULL) {
        BeaconPrintf(CALLBACK_ERROR, "TCP table allocation failed");
    }
    else {
        status = GetExtendedTcpTable(table, &table_size, FALSE, AF_INET,
            SCRIPT_TCP_TABLE_OWNER_PID_ALL, 0);
        listening = 0;
        established = 0;
        if (status == NO_ERROR) {
            for (index = 0; index < table->entry_count; index++) {
                if (table->rows[index].state == 2)
                    listening++;
                if (table->rows[index].state == 5)
                    established++;
            }
            BeaconPrintf(CALLBACK_OUTPUT,
                "tcp4 total=%lu listening=%lu established=%lu",
                table->entry_count, listening, established);
        }
        else {
            BeaconPrintf(CALLBACK_ERROR,
                "TCP table query failed status=%lu", status);
        }
        LocalFree(table);
    }
}
