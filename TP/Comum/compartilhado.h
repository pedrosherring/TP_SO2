#ifndef COMPARTILHADO_H
#define COMPARTILHADO_H

#include <windows.h>
#include <tchar.h>

#define MAX_USERNAME 32
#define MAX_WORD 64
#define SHM_NAME _T("memoria_letras")
#define EVENT_NAME _T("evento_letras")
#define PIPE_NAME _T("\\\\.\\pipe\\jogo")
#define MAXLETRAS_PADRAO 6
#define RITMO_PADRAO 3
#define MAXLETRAS_MAXIMO 12

typedef struct {
    TCHAR type[16]; // e.g., "JOIN", "EXIT", "WORD", "SCORE", "JOGS", "INFO"
    TCHAR username[MAX_USERNAME];
    TCHAR data[MAX_WORD];
} MESSAGE;

#endif