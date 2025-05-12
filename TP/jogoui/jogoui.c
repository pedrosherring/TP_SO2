#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <fcntl.h>
#include <io.h>
#include "../Comum/compartilhado.h"

void enviarMensagem(HANDLE hPipe, MESSAGE* msg);
void mostrarLetras();

int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    if (argc != 2) {
        _tprintf(_T("Uso: jogoui <username>\n"));
        return 1;
    }

    TCHAR* username = argv[1];
    HANDLE hPipe;
    while (1) {
        hPipe = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) {
            _tprintf(_T("Erro ao conectar ao pipe: %ld\n"), GetLastError());
            return 1;
        }
        Sleep(1000);
    }

    MESSAGE msg = { _T("JOIN"), _T(""), _T("") };
    _tcscpy_s(msg.username, MAX_USERNAME, username);
    enviarMensagem(hPipe, &msg);

    HANDLE hEvent = OpenEvent(SYNCHRONIZE, FALSE, EVENT_NAME);
    if (!hEvent) {
        _tprintf(_T("Erro ao abrir evento: %ld\n"), GetLastError());
        CloseHandle(hPipe);
        return 1;
    }

    HANDLE hConsole = GetStdHandle(STD_INPUT_HANDLE);
    TCHAR input[MAX_WORD];
    while (1) {
        // Verificar entrada do usuário e eventos
        HANDLE handles[] = { hConsole, hEvent };
        DWORD resultado = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        if (resultado == WAIT_OBJECT_0) {
            // Entrada do usuário
            if (_fgetts(input, MAX_WORD, stdin)) {
                input[_tcslen(input) - 1] = _T('\0');

                if (_tcscmp(input, _T(":sair")) == 0) {
                    _tcscpy_s(msg.type, 16, _T("EXIT"));
                    _tcscpy_s(msg.data, MAX_WORD, _T(""));
                    enviarMensagem(hPipe, &msg);
                    break;
                }
                else if (_tcscmp(input, _T(":pont")) == 0) {
                    _tcscpy_s(msg.type, 16, _T("SCORE"));
                    _tcscpy_s(msg.data, MAX_WORD, _T(""));
                }
                else if (_tcscmp(input, _T(":jogs")) == 0) {
                    _tcscpy_s(msg.type, 16, _T("JOGS"));
                    _tcscpy_s(msg.data, MAX_WORD, _T(""));
                }
                else {
                    _tcscpy_s(msg.type, 16, _T("WORD"));
                    _tcscpy_s(msg.data, MAX_WORD, input);
                }
                enviarMensagem(hPipe, &msg);
            }
        }
        else if (resultado == WAIT_OBJECT_0 + 1) {
            // Atualização de letras
            mostrarLetras();
            ResetEvent(hEvent);
        }

        // Verificar mensagens do árbitro
        MESSAGE resposta;
        DWORD read;
        while (PeekNamedPipe(hPipe, NULL, 0, NULL, &read, NULL) && read > 0) {
            if (ReadFile(hPipe, &resposta, sizeof(MESSAGE), &read, NULL) && read > 0) {
                if (_tcscmp(resposta.type, _T("INFO")) == 0) {
                    _tprintf(_T("[INFO] %s\n"), resposta.data);
                    if (_tcsstr(resposta.data, _T("Username em uso")) ||
                        _tcsstr(resposta.data, _T("excluído")) ||
                        _tcsstr(resposta.data, _T("Jogo encerrado"))) {
                        CloseHandle(hPipe);
                        CloseHandle(hEvent);
                        return 0;
                    }
                }
            }
        }
    }

    CloseHandle(hPipe);
    CloseHandle(hEvent);
    return 0;
}

void enviarMensagem(HANDLE hPipe, MESSAGE* msg) {
    DWORD written;
    if (!WriteFile(hPipe, msg, sizeof(MESSAGE), &written, NULL)) {
        _tprintf(_T("Erro ao enviar mensagem: %ld\n"), GetLastError());
    }
}

void mostrarLetras() {
    HANDLE hMap = OpenFileMapping(FILE_MAP_READ, FALSE, SHM_NAME);
    if (!hMap) {
        _tprintf(_T("Não foi possível abrir a memória partilhada: %ld\n"), GetLastError());
        return;
    }

    TCHAR* pBuf = (TCHAR*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (pBuf) {
        _tprintf(_T("Letras visíveis: %s\n"), pBuf);
        UnmapViewOfFile(pBuf);
    }
    CloseHandle(hMap);
}