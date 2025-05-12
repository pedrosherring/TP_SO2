#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>
#include "../Comum/compartilhado.h"

typedef struct {
    TCHAR** palavras;
    DWORD totalPalavras;
    CRITICAL_SECTION csDicionario;
} DICIONARIO;

typedef struct {
    TCHAR username[MAX_USERNAME];
    float pontos;
    HANDLE hPipe;
} JOGADOR;

typedef struct {
    HANDLE hPipe;
    JOGADOR* jogadores;
    DWORD* totalJogadores;
    CRITICAL_SECTION* csJogadores;
    TCHAR* sharedMem;
    HANDLE hEvent;
    HANDLE hMutex;
    DWORD maxLetras;
    DWORD ritmo;
    DICIONARIO* dicionario;
} THREAD_ARGS;

// Funções e Threads
DWORD WINAPI ThreadCliente(LPVOID param);
DWORD WINAPI ThreadLetras(LPVOID param);
DWORD WINAPI ThreadAdmin(LPVOID param);
void ConfigurarRegistry(DWORD* maxLetras, DWORD* ritmo);
BOOL CarregarDicionario(DICIONARIO* dicionario, const TCHAR* nomeArquivo);
void LiberarDicionario(DICIONARIO* dicionario);
BOOL ValidarPalavra(TCHAR* palavra, TCHAR* letras, DWORD maxLetras, DICIONARIO* dicionario);
void AtualizarLetras(TCHAR* sharedMem, DWORD maxLetras, HANDLE hMutex, HANDLE hEvent);

int _tmain() {
#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    srand((unsigned)time(NULL));

    // Carregar dicionário
    DICIONARIO dicionario = { 0 };
    if (!CarregarDicionario(&dicionario, _T("D:\\SO2\\TP_SO2\\TP\\Comum\\dicionario.txt"))) {
        _tprintf(_T("[ARBITRO] Falha ao carregar dicionário. Encerrando.\n"));
        return 1;
    }

    // Registry
    DWORD maxLetras, ritmo;
    ConfigurarRegistry(&maxLetras, &ritmo);

    // Shared Memory
    HANDLE hMapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
        sizeof(TCHAR) * (maxLetras * 2), // Espaço para letras + espaços
        SHM_NAME);
    if (!hMapFile) {
        _tprintf(_T("Erro ao criar memória partilhada: %ld\n"), GetLastError());
        LiberarDicionario(&dicionario);
        return 1;
    }

    TCHAR* pMem = (TCHAR*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!pMem) {
        _tprintf(_T("Erro ao mapear memória: %ld\n"), GetLastError());
        CloseHandle(hMapFile);
        LiberarDicionario(&dicionario);
        return 1;
    }
    for (DWORD i = 0; i < maxLetras; i++) {
        pMem[i * 2] = _T('_');
        pMem[i * 2 + 1] = _T(' ');
    }
    pMem[maxLetras * 2 - 1] = _T('\0');

    // Mecanismos de Sincronização
    HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, EVENT_NAME);
    if (!hEvent) {
        _tprintf(_T("Erro ao criar evento: %ld\n"), GetLastError());
        UnmapViewOfFile(pMem);
        CloseHandle(hMapFile);
        LiberarDicionario(&dicionario);
        return 1;
    }

    HANDLE hMutex = CreateMutex(NULL, FALSE, _T("mutex_letras"));
    if (!hMutex) {
        _tprintf(_T("Erro ao criar mutex: %ld\n"), GetLastError());
        UnmapViewOfFile(pMem);
        CloseHandle(hMapFile);
        CloseHandle(hEvent);
        LiberarDicionario(&dicionario);
        return 1;
    }

    JOGADOR jogadores[20] = { 0 };
    DWORD totalJogadores = 0;
    CRITICAL_SECTION csJogadores;
    InitializeCriticalSection(&csJogadores);

    // Thread para geração de letras
    THREAD_ARGS argsLetras = {
        INVALID_HANDLE_VALUE, jogadores, &totalJogadores, &csJogadores,
        pMem, hEvent, hMutex, maxLetras, ritmo, &dicionario
    };
    CreateThread(NULL, 0, ThreadLetras, &argsLetras, 0, NULL);

    // Thread para comandos administrativos
    CreateThread(NULL, 0, ThreadAdmin, &argsLetras, 0, NULL);

    _tprintf(_T("[ARBITRO] Iniciando. Esperando jogadores...\n"));
    while (1) {
        HANDLE hPipe = CreateNamedPipe(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(MESSAGE), sizeof(MESSAGE), 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) {
            _tprintf(_T("Erro ao criar named pipe: %ld\n"), GetLastError());
            break;
        }

        if (!ConnectNamedPipe(hPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            continue;
        }

        THREAD_ARGS* args = malloc(sizeof(THREAD_ARGS));
        *args = (THREAD_ARGS){
            hPipe, jogadores, &totalJogadores, &csJogadores,
            pMem, hEvent, hMutex, maxLetras, ritmo, &dicionario
        };
        CreateThread(NULL, 0, ThreadCliente, args, 0, NULL);
    }

    UnmapViewOfFile(pMem);
    CloseHandle(hMapFile);
    CloseHandle(hEvent);
    CloseHandle(hMutex);
    DeleteCriticalSection(&csJogadores);
    LiberarDicionario(&dicionario);
    return 0;
}

DWORD WINAPI ThreadCliente(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    HANDLE hPipe = args->hPipe;
    JOGADOR* jogadores = args->jogadores;
    DWORD* totalJogadores = args->totalJogadores;
    CRITICAL_SECTION* cs = args->csJogadores;
    TCHAR* sharedMem = args->sharedMem;
    DWORD maxLetras = args->maxLetras;
    DICIONARIO* dicionario = args->dicionario;

    MESSAGE msg;
    DWORD bytesRead;
    TCHAR meuUsername[MAX_USERNAME] = _T("");
    int meuIndex = -1;

    while (1) {
        BOOL sucesso = ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesRead, NULL);
        if (!sucesso || bytesRead == 0) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                _tprintf(_T("[ARBITRO] Cliente desconectado: %s\n"), meuUsername);
            }
            else {
                _tprintf(_T("[ARBITRO] Erro ao ler pipe: %ld\n"), GetLastError());
            }
            break;
        }

        if (_tcscmp(msg.type, _T("JOIN")) == 0) {
            EnterCriticalSection(cs);
            for (int i = 0; i < *totalJogadores; i++) {
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
            for (int i = 0; i < *totalJogadores; i++) {
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

            for (int i = 0; i < *totalJogadores; i++) {
                if (jogadores[i].hPipe != hPipe) {
                    WriteFile(jogadores[i].hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
                }
            }

            for (int i = meuIndex; i < (*totalJogadores) - 1; i++) {
                jogadores[i] = jogadores[i + 1];
            }
            (*totalJogadores)--;

            LeaveCriticalSection(cs);
            break;
        }
        else if (_tcscmp(msg.type, _T("WORD")) == 0) {
            EnterCriticalSection(cs);
            if (ValidarPalavra(msg.data, sharedMem, maxLetras, dicionario)) {
                jogadores[meuIndex].pontos += (float)_tcslen(msg.data);
                _tprintf(_T("[ARBITRO] %s acertou a palavra '%s' (+%.1f pontos)\n"),
                    msg.username, msg.data, (float)_tcslen(msg.data));

                // Remover letras usadas
                TCHAR palavra[MAX_WORD];
                _tcscpy_s(palavra, MAX_WORD, msg.data);
                WaitForSingleObject(args->hMutex, INFINITE);
                for (int i = 0; palavra[i]; i++) {
                    for (int j = 0; j < maxLetras; j++) {
                        if (sharedMem[j * 2] == _totupper(palavra[i])) {
                            sharedMem[j * 2] = _T('_');
                            break;
                        }
                    }
                }
                ReleaseMutex(args->hMutex);
                SetEvent(args->hEvent);

                MESSAGE info;
                _tcscpy_s(info.type, 16, _T("INFO"));
                _tcscpy_s(info.username, MAX_USERNAME, msg.username);
                _stprintf_s(info.data, MAX_WORD, _T("%s acertou '%s' (+%.1f pontos)"),
                    msg.username, msg.data, (float)_tcslen(msg.data));
                for (int i = 0; i < *totalJogadores; i++) {
                    WriteFile(jogadores[i].hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
                }
            }
            else {
                jogadores[meuIndex].pontos -= (float)_tcslen(msg.data) * 0.5f;
                _tprintf(_T("[ARBITRO] %s errou a palavra '%s' (-%.1f pontos)\n"),
                    msg.username, msg.data, (float)_tcslen(msg.data) * 0.5f);

                MESSAGE info;
                _tcscpy_s(info.type, 16, _T("INFO"));
                _tcscpy_s(info.username, MAX_USERNAME, msg.username);
                _stprintf_s(info.data, MAX_WORD, _T("Palavra '%s' inválida."), msg.data);
                WriteFile(hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
            }
            LeaveCriticalSection(cs);
        }
        else if (_tcscmp(msg.type, _T("SCORE")) == 0) {
            MESSAGE info;
            _tcscpy_s(info.type, 16, _T("INFO"));
            _tcscpy_s(info.username, MAX_USERNAME, msg.username);
            _stprintf_s(info.data, MAX_WORD, _T("Sua pontuação: %.1f"), jogadores[meuIndex].pontos);
            WriteFile(hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
        }
        else if (_tcscmp(msg.type, _T("JOGS")) == 0) {
            EnterCriticalSection(cs);
            MESSAGE info;
            _tcscpy_s(info.type, 16, _T("INFO"));
            _tcscpy_s(info.username, MAX_USERNAME, msg.username);
            TCHAR buffer[MAX_WORD] = _T("Jogadores:\n");
            for (int i = 0; i < *totalJogadores; i++) {
                _stprintf_s(buffer + _tcslen(buffer), MAX_WORD - _tcslen(buffer),
                    _T("%s (%.1f)\n"), jogadores[i].username, jogadores[i].pontos);
            }
            _tcscpy_s(info.data, MAX_WORD, buffer);
            WriteFile(hPipe, &info, sizeof(MESSAGE), &bytesRead, NULL);
            LeaveCriticalSection(cs);
        }
    }

    CloseHandle(hPipe);
    free(args);
    return 0;
}

DWORD WINAPI ThreadLetras(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    while (1) {
        Sleep(args->ritmo * 1000);
        AtualizarLetras(args->sharedMem, args->maxLetras, args->hMutex, args->hEvent);
    }
    return 0;
}

DWORD WINAPI ThreadAdmin(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    TCHAR comando[MAX_WORD];
    while (1) {
        _fgetts(comando, MAX_WORD, stdin);
        comando[_tcslen(comando) - 1] = _T('\0');

        EnterCriticalSection(args->csJogadores);
        if (_tcscmp(comando, _T("listar")) == 0) {
            _tprintf(_T("Jogadores:\n"));
            for (int i = 0; i < *args->totalJogadores; i++) {
                _tprintf(_T("%s: %.1f pontos\n"), args->jogadores[i].username, args->jogadores[i].pontos);
            }
        }
        else if (_tcsncmp(comando, _T("excluir "), 8) == 0) {
            TCHAR* username = comando + 8;
            for (int i = 0; i < *args->totalJogadores; i++) {
                if (_tcscmp(args->jogadores[i].username, username) == 0) {
                    MESSAGE info = { _T("INFO"), _T(""), _T("Você foi excluído pelo administrador.") };
                    WriteFile(args->jogadores[i].hPipe, &info, sizeof(MESSAGE), &(DWORD){0}, NULL);
                    CloseHandle(args->jogadores[i].hPipe);

                    for (int j = i; j < *args->totalJogadores - 1; j++) {
                        args->jogadores[j] = args->jogadores[j + 1];
                    }
                    (*args->totalJogadores)--;
                    _tprintf(_T("[ARBITRO] %s excluído\n"), username);
                    break;
                }
            }
        }
        else if (_tcsncmp(comando, _T("iniciarbot "), 11) == 0) {
            _tprintf(_T("Comando iniciarbot não implementado\n"));
        }
        else if (_tcscmp(comando, _T("acelerar")) == 0) {
            if (args->ritmo > 1) args->ritmo--;
            _tprintf(_T("Ritmo ajustado para %ld segundos\n"), args->ritmo);
        }
        else if (_tcscmp(comando, _T("travar")) == 0) {
            args->ritmo++;
            _tprintf(_T("Ritmo ajustado para %ld segundos\n"), args->ritmo);
        }
        else if (_tcscmp(comando, _T("encerrar")) == 0) {
            MESSAGE info = { _T("INFO"), _T(""), _T("Jogo encerrado pelo administrador.") };
            for (int i = 0; i < *args->totalJogadores; i++) {
                WriteFile(args->jogadores[i].hPipe, &info, sizeof(MESSAGE), &(DWORD){0}, NULL);
                CloseHandle(args->jogadores[i].hPipe);
            }
            *args->totalJogadores = 0;
            _tprintf(_T("[ARBITRO] Jogo encerrado\n"));
        }
        LeaveCriticalSection(args->csJogadores);
    }
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

BOOL CarregarDicionario(DICIONARIO* dicionario, const TCHAR* nomeArquivo) {
    InitializeCriticalSection(&dicionario->csDicionario);
    dicionario->palavras = NULL;
    dicionario->totalPalavras = 0;

    FILE* arquivo;
    if (_tfopen_s(&arquivo, nomeArquivo, _T("r")) != 0 || !arquivo) {
        _tprintf(_T("Erro ao abrir %s\n"), nomeArquivo);
        return FALSE;
    }

    TCHAR linha[MAX_WORD];
    DWORD capacidade = 100;
    dicionario->palavras = (TCHAR**)malloc(capacidade * sizeof(TCHAR*));
    if (!dicionario->palavras) {
        fclose(arquivo);
        _tprintf(_T("Erro ao alocar memória para dicionário\n"));
        return FALSE;
    }

    while (_fgetts(linha, MAX_WORD, arquivo)) {
        linha[_tcslen(linha) - 1] = _T('\0');
        if (_tcslen(linha) == 0) continue;

        if (dicionario->totalPalavras >= capacidade) {
            capacidade *= 2;
            TCHAR** temp = (TCHAR**)realloc(dicionario->palavras, capacidade * sizeof(TCHAR*));
            if (!temp) {
                fclose(arquivo);
                for (DWORD i = 0; i < dicionario->totalPalavras; i++) {
                    free(dicionario->palavras[i]);
                }
                free(dicionario->palavras);
                _tprintf(_T("Erro ao realocar memória para dicionário\n"));
                return FALSE;
            }
            dicionario->palavras = temp;
        }

        dicionario->palavras[dicionario->totalPalavras] = _tcsdup(linha);
        if (!dicionario->palavras[dicionario->totalPalavras]) {
            fclose(arquivo);
            for (DWORD i = 0; i < dicionario->totalPalavras; i++) {
                free(dicionario->palavras[i]);
            }
            free(dicionario->palavras);
            _tprintf(_T("Erro ao alocar memória para palavra\n"));
            return FALSE;
        }
        dicionario->totalPalavras++;
    }

    fclose(arquivo);
    _tprintf(_T("[ARBITRO] Dicionário carregado com %ld palavras\n"), dicionario->totalPalavras);
    return TRUE;
}

void LiberarDicionario(DICIONARIO* dicionario) {
    EnterCriticalSection(&dicionario->csDicionario);
    for (DWORD i = 0; i < dicionario->totalPalavras; i++) {
        free(dicionario->palavras[i]);
    }
    free(dicionario->palavras);
    dicionario->palavras = NULL;
    dicionario->totalPalavras = 0;
    LeaveCriticalSection(&dicionario->csDicionario);
    DeleteCriticalSection(&dicionario->csDicionario);
}

BOOL ValidarPalavra(TCHAR* palavra, TCHAR* letras, DWORD maxLetras, DICIONARIO* dicionario) {
    TCHAR palavraMaiuscula[MAX_WORD];
    _tcscpy_s(palavraMaiuscula, MAX_WORD, palavra);
    for (int i = 0; palavraMaiuscula[i]; i++) {
        palavraMaiuscula[i] = _totupper(palavraMaiuscula[i]);
    }

    BOOL noDicionario = FALSE;
    EnterCriticalSection(&dicionario->csDicionario);
    for (DWORD i = 0; i < dicionario->totalPalavras; i++) {
        if (_tcscmp(palavraMaiuscula, dicionario->palavras[i]) == 0) {
            noDicionario = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&dicionario->csDicionario);

    if (!noDicionario) {
        return FALSE;
    }

    TCHAR letrasCopia[MAX_WORD];
    _tcscpy_s(letrasCopia, MAX_WORD, letras);
    for (int i = 0; palavraMaiuscula[i]; i++) {
        BOOL encontrou = FALSE;
        for (int j = 0; j < maxLetras * 2; j += 2) {
            if (letrasCopia[j] == palavraMaiuscula[i]) {
                letrasCopia[j] = _T('_');
                encontrou = TRUE;
                break;
            }
        }
        if (!encontrou) {
            return FALSE;
        }
    }
    return TRUE;
}

void AtualizarLetras(TCHAR* sharedMem, DWORD maxLetras, HANDLE hMutex, HANDLE hEvent) {
    WaitForSingleObject(hMutex, INFINITE);

    for (int i = maxLetras - 1; i > 0; i--) {
        sharedMem[i * 2] = sharedMem[(i - 1) * 2];
    }

    TCHAR novaLetra = _T('A') + (rand() % 26);
    sharedMem[0] = novaLetra;

    ReleaseMutex(hMutex);
    SetEvent(hEvent);
}