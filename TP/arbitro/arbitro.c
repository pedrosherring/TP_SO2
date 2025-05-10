#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <fcntl.h>
#include <io.h>
#include "../Comum/compartilhado.h"

typedef struct {
    TCHAR username[MAX_USERNAME];
    float pontos;
    HANDLE hPipe;
} JOGADOR;

typedef struct {
    HANDLE hPipe;
    JOGADOR* jogadores;
    int* totalJogadores;
    CRITICAL_SECTION* csJogadores;
} THREAD_ARGS;

DWORD WINAPI ThreadCliente(LPVOID param);
void ConfigurarRegistry(DWORD* maxLetras, DWORD* ritmo);

int _tmain() {
    DWORD maxLetras, ritmo;
    ConfigurarRegistry(&maxLetras, &ritmo);

    HANDLE hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
        sizeof(TCHAR) * maxLetras, SHM_NAME);
    if (!hMapFile) {
        _tprintf(_T("Erro ao criar memória partilhada.\n"));
        return 1;
    }

    TCHAR* pMem = (TCHAR*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!pMem) {
        _tprintf(_T("Erro ao mapear memória.\n"));
        CloseHandle(hMapFile);
        return 1;
    }

    _tcscpy_s(pMem, 32, _T("A B C D E F"));

    HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, EVENT_NAME);
    if (!hEvent) {
        _tprintf(_T("Erro ao criar evento.\n"));
        UnmapViewOfFile(pMem);
        CloseHandle(hMapFile);
        return 1;
    }

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    JOGADOR jogadores[20] = { 0 };
    int totalJogadores = 0;
    CRITICAL_SECTION csJogadores;
    InitializeCriticalSection(&csJogadores);

    _tprintf_s(_T("[ARBITRO] Esperando jogadores...\n"));
    while (1) {
        HANDLE hPipe = CreateNamedPipe(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(MESSAGE), sizeof(MESSAGE), 0, NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            _tprintf(_T("Erro ao criar named pipe.\n"));
            break;
        }

        if (!ConnectNamedPipe(hPipe, NULL)) {
            CloseHandle(hPipe);
            continue;
        }

        THREAD_ARGS* args = (THREAD_ARGS*)malloc(sizeof(THREAD_ARGS));
        args->hPipe = hPipe;
        args->jogadores = jogadores;
        args->totalJogadores = &totalJogadores;
        args->csJogadores = &csJogadores;
        CreateThread(NULL, 0, ThreadCliente, args, 0, NULL);
    }

    UnmapViewOfFile(pMem);
    CloseHandle(hMapFile);
    CloseHandle(hEvent);
    DeleteCriticalSection(&csJogadores);
    return 0;
}

DWORD WINAPI ThreadCliente(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    HANDLE hPipe = args->hPipe;
    JOGADOR* jogadores = args->jogadores;
    int* totalJogadores = args->totalJogadores;
    CRITICAL_SECTION* cs = args->csJogadores;

    MESSAGE msg;
    DWORD bytesRead;

    TCHAR meuUsername[MAX_USERNAME] = _T("");
    int meuIndex = -1;

    while (ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesRead, NULL)) {
        if (_tcscmp(msg.type, _T("JOIN")) == 0) {
            EnterCriticalSection(cs);

            for (int i = 0; i < *totalJogadores; ++i) {
                if (_tcscmp(jogadores[i].username, msg.username) == 0) {
                    MESSAGE erro = { _T("INFO"), _T(""), _T("Username em uso. Encerrando.") };
                    WriteFile(hPipe, &erro, sizeof(MESSAGE), &bytesRead, NULL);
                    LeaveCriticalSection(cs);
                    CloseHandle(hPipe);
                    free(args);
                    return 0;
                }
            }

            meuIndex = *totalJogadores;
            _tcscpy_s(meuUsername, MAX_USERNAME, msg.username);
            _tcscpy_s(jogadores[meuIndex].username, MAX_USERNAME, msg.username);
            jogadores[meuIndex].hPipe = hPipe;
            jogadores[meuIndex].pontos = 0;
            (*totalJogadores)++;

            LeaveCriticalSection(cs);

            _tprintf(_T("[+] %s entrou no jogo\n"), msg.username);

            MESSAGE info;
            _tcscpy_s(info.type, 16, _T("INFO"));
            _tcscpy_s(info.username, MAX_USERNAME, msg.username);
            _stprintf_s(info.data, MAX_WORD, _T("%s entrou no jogo."), msg.username);

            EnterCriticalSection(cs);
            for (int i = 0; i < *totalJogadores; ++i) {
                if (jogadores[i].hPipe != hPipe) {
                    WriteFile(jogadores[i].hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
                }
            }
            LeaveCriticalSection(cs);
        }

        else if (_tcscmp(msg.type, _T("EXIT")) == 0) {
            EnterCriticalSection(cs);

            _tprintf(_T("[-] %s saiu do jogo\n"), msg.username);

            MESSAGE info;
            _tcscpy_s(info.type, 16, _T("INFO"));
            _tcscpy_s(info.username, MAX_USERNAME, msg.username);
            _stprintf_s(info.data, MAX_WORD, _T("%s saiu do jogo."), msg.username);

            for (int i = 0; i < *totalJogadores; ++i) {
                if (jogadores[i].hPipe != hPipe) {
                    WriteFile(jogadores[i].hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
                }
            }

            for (int i = meuIndex; i < (*totalJogadores) - 1; ++i) {
                jogadores[i] = jogadores[i + 1];
            }
            (*totalJogadores)--;

            LeaveCriticalSection(cs);
            break;
        }
    }

    CloseHandle(hPipe);
    free(args);
    return 0;
}

void ConfigurarRegistry(DWORD* maxLetras, DWORD* ritmo) {
    HKEY hKey;
    DWORD tamanho = sizeof(DWORD);
    LONG resultado;

    *maxLetras = MAXLETRAS_PADRAO;
    *ritmo = RITMO_PADRAO;

    resultado = RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\TrabSO2"), 0, KEY_READ, &hKey);
    if (resultado == ERROR_SUCCESS) {
        RegQueryValueEx(hKey, _T("MAXLETRAS"), NULL, NULL, (LPBYTE)maxLetras, &tamanho);
        tamanho = sizeof(DWORD);
        RegQueryValueEx(hKey, _T("RITMO"), NULL, NULL, (LPBYTE)ritmo, &tamanho);
        RegCloseKey(hKey);
    }
    else {
        resultado = RegCreateKey(HKEY_CURRENT_USER, _T("Software\\TrabSO2"), &hKey);
        if (resultado == ERROR_SUCCESS) {
            RegSetValueEx(hKey, _T("MAXLETRAS"), 0, REG_DWORD, (LPBYTE)maxLetras, sizeof(DWORD));
            RegSetValueEx(hKey, _T("RITMO"), 0, REG_DWORD, (LPBYTE)ritmo, sizeof(DWORD));
            RegCloseKey(hKey);
        }
        else {
            _tprintf(_T("Erro ao criar chave do registro: %ld\n"), resultado);
        }
    }

    if (*maxLetras > MAXLETRAS_MAXIMO) {
        *maxLetras = MAXLETRAS_MAXIMO;
        if (RegOpenKeyEx(HKEY_CURRENT_USER, _T("Software\\TrabSO2"), 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            RegSetValueEx(hKey, _T("MAXLETRAS"), 0, REG_DWORD, (LPBYTE)maxLetras, sizeof(DWORD));
            RegCloseKey(hKey);
        }
    }
}