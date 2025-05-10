#pragma once
#include <windows.h>
#include <tchar.h>

#define MAX_USERNAME 32
#define MAX_JOGADORES 20
#define MAX_WORD 64
#define PIPE_NAME _T("\\\\.\\pipe\\SO2_GamePipe")
#define SHM_NAME _T("SO2_SharedMemory")
#define EVENT_NAME _T("SO2_SharedEvent")
#define MAXLETRAS_PADRAO 6
#define MAXLETRAS_MAXIMO 12
#define RITMO_PADRAO 3

typedef struct {
    TCHAR type[16];
    TCHAR username[MAX_USERNAME];
    TCHAR data[MAX_WORD];
} MESSAGE;
