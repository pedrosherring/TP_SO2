#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h> // Para malloc, free, realloc, rand, srand
#include <tchar.h>
#include <fcntl.h>    // Para _setmode
#include <io.h>       // Para _setmode, _fileno
#include <time.h>     // Para srand, time
#include <strsafe.h>  // Para StringCchPrintf, etc.

#include "../Comum/compartilhado.h" // Ficheiro revisto

// Definição manual de _countof se não estiver disponível (comum em alguns compiladores/configurações)
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array[0]))
#endif

// Definições de Timeout
#define IO_TIMEOUT 5000 
#define CONNECT_TIMEOUT 500 
#define READ_TIMEOUT_THREAD_JOGADOR 500 

// Estruturas internas do Árbitro
typedef struct {
    TCHAR** palavras;
    DWORD totalPalavras;
    CRITICAL_SECTION csDicionario;
} DICIONARIO_ARBITRO;

typedef struct {
    TCHAR username[MAX_USERNAME];
    float pontos;
    HANDLE hPipe;
    BOOL ativo;
    DWORD dwThreadIdCliente;
} JOGADOR_INFO_ARBITRO;

// Argumentos para as threads de cliente e letras
typedef struct {
    HANDLE hPipeCliente;

    JOGADOR_INFO_ARBITRO* jogadores;
    DWORD* pTotalJogadoresAtivos;
    CRITICAL_SECTION* pcsListaJogadores;

    DadosJogoCompartilhados* pDadosShm;
    HANDLE hEventoShmUpdate;
    HANDLE hMutexShm;

    DICIONARIO_ARBITRO* pDicionario;

    int* pMaxLetrasConfig;
    int* pRitmoConfigSegundos;

    volatile BOOL* pServidorEmExecucao;
    volatile BOOL* pJogoRealmenteAtivo;

} THREAD_ARGS_ARBITRO;

// ==========================================================================================
// PROTÓTIPOS DE FUNÇÕES INTERNAS
// ==========================================================================================
void Log(const TCHAR* format, ...);
void LogError(const TCHAR* format, ...);
void LogWarning(const TCHAR* format, ...);

BOOL InicializarServidor();
void EncerrarServidor();
void ConfigurarValoresRegistry(int* maxLetras, int* ritmoSegundos);
BOOL CarregarDicionarioServidor(DICIONARIO_ARBITRO* dict, const TCHAR* nomeArquivo);
void LiberarDicionarioServidor(DICIONARIO_ARBITRO* dict);
BOOL InicializarMemoriaPartilhadaArbitro(int maxLetras);
void LimparMemoriaPartilhadaArbitro();

DWORD WINAPI ThreadGestorLetras(LPVOID param);
DWORD WINAPI ThreadAdminArbitro(LPVOID param);
DWORD WINAPI ThreadClienteConectado(LPVOID param);

void RemoverJogador(const TCHAR* username, BOOL notificarClienteParaSair);
int EncontrarJogador(const TCHAR* username);
void NotificarTodosOsJogadores(const MESSAGE* msgAEnviar, const TCHAR* skipUsername); // Protótipo
BOOL ValidarPalavraJogo(const TCHAR* palavraSubmetida, const TCHAR* usernameJogador, THREAD_ARGS_ARBITRO* argsClienteThread);
void VerificarEstadoJogo();


// ==========================================================================================
// VARIÁVEIS GLOBAIS DO ÁRBITRO
// ==========================================================================================
JOGADOR_INFO_ARBITRO g_listaJogadores[MAX_JOGADORES];
DWORD g_totalJogadoresAtivos = 0;
CRITICAL_SECTION g_csListaJogadores;

DadosJogoCompartilhados* g_pDadosShm = NULL;
HANDLE g_hMapFileShm = NULL;
HANDLE g_hEventoShmUpdate = NULL;
HANDLE g_hMutexShm = NULL;

DICIONARIO_ARBITRO g_dicionario;

int g_maxLetrasConfig = DEFAULT_MAXLETRAS;
int g_ritmoConfigSegundos = DEFAULT_RITMO_SEGUNDOS;

volatile BOOL g_servidorEmExecucao = TRUE;
volatile BOOL g_jogoRealmenteAtivo = FALSE;

CRITICAL_SECTION g_csLog;

// ==========================================================================================
// FUNÇÃO PRINCIPAL - _tmain
// ==========================================================================================
int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    InitializeCriticalSection(&g_csLog);
    Log(_T("[ARBITRO] Iniciando Árbitro..."));

    if (!InicializarServidor()) {
        LogError(_T("[ARBITRO] Falha ao inicializar o servidor. Encerrando."));
        EncerrarServidor();
        DeleteCriticalSection(&g_csLog);
        return 1;
    }

    HANDLE hThreads[2] = { NULL, NULL };
    THREAD_ARGS_ARBITRO argsGlobais;
    ZeroMemory(&argsGlobais, sizeof(THREAD_ARGS_ARBITRO));
    argsGlobais.jogadores = g_listaJogadores;
    argsGlobais.pTotalJogadoresAtivos = &g_totalJogadoresAtivos;
    argsGlobais.pcsListaJogadores = &g_csListaJogadores;
    argsGlobais.pDadosShm = g_pDadosShm;
    argsGlobais.hEventoShmUpdate = g_hEventoShmUpdate;
    argsGlobais.hMutexShm = g_hMutexShm;
    argsGlobais.pDicionario = &g_dicionario;
    argsGlobais.pMaxLetrasConfig = &g_maxLetrasConfig;
    argsGlobais.pRitmoConfigSegundos = &g_ritmoConfigSegundos;
    argsGlobais.pServidorEmExecucao = &g_servidorEmExecucao;
    argsGlobais.pJogoRealmenteAtivo = &g_jogoRealmenteAtivo;


    hThreads[0] = CreateThread(NULL, 0, ThreadGestorLetras, &argsGlobais, 0, NULL);
    if (hThreads[0] == NULL) {
        LogError(_T("[ARBITRO] Falha ao criar ThreadGestorLetras. Encerrando."));
        g_servidorEmExecucao = FALSE;
        EncerrarServidor();
        DeleteCriticalSection(&g_csLog);
        return 1;
    }
    hThreads[1] = CreateThread(NULL, 0, ThreadAdminArbitro, &argsGlobais, 0, NULL);
    if (hThreads[1] == NULL) {
        LogError(_T("[ARBITRO] Falha ao criar ThreadAdminArbitro. Encerrando."));
        g_servidorEmExecucao = FALSE;
        if (hThreads[0] != NULL) { WaitForSingleObject(hThreads[0], INFINITE); CloseHandle(hThreads[0]); }
        EncerrarServidor();
        DeleteCriticalSection(&g_csLog);
        return 1;
    }

    Log(_T("[ARBITRO] Servidor pronto. Aguardando conexões de jogadores em %s"), PIPE_NAME);

    while (g_servidorEmExecucao) {
        HANDLE hPipe = CreateNamedPipe(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(MESSAGE) * 2,
            sizeof(MESSAGE) * 2,
            NMPWAIT_USE_DEFAULT_WAIT,
            NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            if (g_servidorEmExecucao) {
                LogError(_T("[ARBITRO] Falha ao criar Named Pipe (instância): %lu"), GetLastError());
                Sleep(1000);
            }
            continue;
        }

        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(OVERLAPPED));
        ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (ov.hEvent == NULL) {
            LogError(_T("[ARBITRO] Falha ao criar evento para ConnectNamedPipe: %lu"), GetLastError());
            CloseHandle(hPipe);
            continue;
        }

        BOOL fConnected = ConnectNamedPipe(hPipe, &ov);
        if (!fConnected && GetLastError() == ERROR_IO_PENDING) {
            DWORD dwWaitResult = WaitForSingleObject(ov.hEvent, CONNECT_TIMEOUT);
            if (dwWaitResult == WAIT_OBJECT_0) {
                DWORD dwDummy;
                fConnected = GetOverlappedResult(hPipe, &ov, &dwDummy, FALSE);
                if (!fConnected) LogError(_T("[ARBITRO] GetOverlappedResult falhou após evento: %lu"), GetLastError());
            }
            else if (dwWaitResult == WAIT_TIMEOUT) {
                //LogWarning(_T("[ARBITRO] Timeout (%dms) ao aguardar conexão no pipe %p. Cancelando."), CONNECT_TIMEOUT, hPipe);
                CancelIo(hPipe);
                fConnected = FALSE;
            }
            else {
                LogError(_T("[ARBITRO] Erro %lu ao aguardar conexão no pipe %p."), GetLastError(), hPipe);
                fConnected = FALSE;
            }
        }
        else if (!fConnected && GetLastError() == ERROR_PIPE_CONNECTED) {
            fConnected = TRUE;
        }
        else if (!fConnected) {
            LogError(_T("[ARBITRO] ConnectNamedPipe falhou imediatamente: %lu"), GetLastError());
        }

        CloseHandle(ov.hEvent);

        if (fConnected && g_servidorEmExecucao) {
            EnterCriticalSection(&g_csListaJogadores);
            if (g_totalJogadoresAtivos < MAX_JOGADORES) {
                LeaveCriticalSection(&g_csListaJogadores);

                THREAD_ARGS_ARBITRO* argsCliente = (THREAD_ARGS_ARBITRO*)malloc(sizeof(THREAD_ARGS_ARBITRO));
                if (argsCliente == NULL) {
                    LogError(_T("[ARBITRO] Falha ao alocar memória para THREAD_ARGS_ARBITRO."));
                    DisconnectNamedPipe(hPipe);
                    CloseHandle(hPipe);
                }
                else {
                    *argsCliente = argsGlobais;
                    argsCliente->hPipeCliente = hPipe;

                    HANDLE hThreadCliente = CreateThread(NULL, 0, ThreadClienteConectado, argsCliente, 0, NULL);
                    if (hThreadCliente == NULL) {
                        LogError(_T("[ARBITRO] Falha ao criar ThreadClienteConectado para pipe %p: %lu"), hPipe, GetLastError());
                        free(argsCliente);
                        DisconnectNamedPipe(hPipe);
                        CloseHandle(hPipe);
                    }
                    else {
                        CloseHandle(hThreadCliente);
                    }
                }
            }
            else {
                LeaveCriticalSection(&g_csListaJogadores);
                LogWarning(_T("[ARBITRO] Jogo cheio. Rejeitando conexão no pipe %p."), hPipe);
                MESSAGE msgCheio; ZeroMemory(&msgCheio, sizeof(MESSAGE));
                StringCchCopy(msgCheio.type, _countof(msgCheio.type), _T("JOIN_GAME_FULL"));
                StringCchCopy(msgCheio.username, _countof(msgCheio.username), _T("Arbitro"));
                StringCchCopy(msgCheio.data, _countof(msgCheio.data), _T("O jogo está cheio."));
                DWORD bytesEscritos;
                WriteFile(hPipe, &msgCheio, sizeof(MESSAGE), &bytesEscritos, NULL);
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
            }
        }
        else {
            if (hPipe != INVALID_HANDLE_VALUE) {
                //if (g_servidorEmExecucao) LogWarning(_T("[ARBITRO] Conexão não estabelecida ou servidor a encerrar para pipe %p. Fechando."), hPipe);
                CloseHandle(hPipe);
            }
        }
    }

    Log(_T("[ARBITRO] Loop principal de aceitação de conexões terminado."));
    if (hThreads[0] != NULL) { WaitForSingleObject(hThreads[0], INFINITE); CloseHandle(hThreads[0]); }
    if (hThreads[1] != NULL) { WaitForSingleObject(hThreads[1], INFINITE); CloseHandle(hThreads[1]); }

    EncerrarServidor();
    Log(_T("[ARBITRO] Servidor encerrado."));
    DeleteCriticalSection(&g_csLog);
    return 0;
}


// ==========================================================================================
// INICIALIZAÇÃO E ENCERRAMENTO DO SERVIDOR
// ==========================================================================================
BOOL InicializarServidor() {
    srand((unsigned)time(NULL));
    ConfigurarValoresRegistry(&g_maxLetrasConfig, &g_ritmoConfigSegundos);
    Log(_T("[INIT] Configurações: MAXLETRAS=%d, RITMO=%ds"), g_maxLetrasConfig, g_ritmoConfigSegundos);

    if (!CarregarDicionarioServidor(&g_dicionario, _T("dicionario.txt"))) {
        LogError(_T("[INIT] Falha ao carregar dicionário."));
        return FALSE;
    }

    if (!InicializarMemoriaPartilhadaArbitro(g_maxLetrasConfig)) {
        LogError(_T("[INIT] Falha ao inicializar memória partilhada."));
        LiberarDicionarioServidor(&g_dicionario);
        return FALSE;
    }

    InitializeCriticalSection(&g_csListaJogadores);

    ZeroMemory(g_listaJogadores, sizeof(g_listaJogadores));
    g_totalJogadoresAtivos = 0;
    g_servidorEmExecucao = TRUE;
    g_jogoRealmenteAtivo = FALSE;

    return TRUE;
}

void EncerrarServidor() {
    Log(_T("[ENCERRAR] Iniciando encerramento do servidor..."));

    if (g_servidorEmExecucao) {
        g_servidorEmExecucao = FALSE;

        MESSAGE msgShutdown; ZeroMemory(&msgShutdown, sizeof(MESSAGE));
        StringCchCopy(msgShutdown.type, _countof(msgShutdown.type), _T("SHUTDOWN"));
        StringCchCopy(msgShutdown.username, _countof(msgShutdown.username), _T("Arbitro"));
        StringCchCopy(msgShutdown.data, _countof(msgShutdown.data), _T("O servidor está a encerrar."));
        NotificarTodosOsJogadores(&msgShutdown, NULL);

        if (g_hEventoShmUpdate) SetEvent(g_hEventoShmUpdate);

        HANDLE hSelfConnect = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hSelfConnect != INVALID_HANDLE_VALUE) {
            CloseHandle(hSelfConnect);
        }
    }

    Log(_T("[ENCERRAR] Aguardando um momento para as threads de cliente... (1s)"));
    Sleep(1000);

    if (g_csListaJogadores.DebugInfo != NULL) {
        EnterCriticalSection(&g_csListaJogadores);
        for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
            if (g_listaJogadores[i].hPipe != INVALID_HANDLE_VALUE) {
                LogWarning(_T("[ENCERRAR] Pipe do jogador %s (idx %lu) ainda aberto. Fechando."), g_listaJogadores[i].username, i);
                CloseHandle(g_listaJogadores[i].hPipe);
                g_listaJogadores[i].hPipe = INVALID_HANDLE_VALUE;
            }
            g_listaJogadores[i].ativo = FALSE;
        }
        g_totalJogadoresAtivos = 0;
        LeaveCriticalSection(&g_csListaJogadores);
        DeleteCriticalSection(&g_csListaJogadores);
    }
    else {
        LogWarning(_T("[ENCERRAR] Critical section da lista de jogadores não inicializada ou já deletada."));
    }


    LimparMemoriaPartilhadaArbitro();
    LiberarDicionarioServidor(&g_dicionario);

    Log(_T("[ENCERRAR] Recursos principais do servidor libertados."));
}


// ==========================================================================================
// Funções de Log
// ==========================================================================================
void Log(const TCHAR* format, ...) {
    if (g_csLog.DebugInfo == NULL) {
        TCHAR fallbackBuffer[1024];
        va_list fbArgs;
        va_start(fbArgs, format);
        StringCchVPrintf(fallbackBuffer, _countof(fallbackBuffer), format, fbArgs);
        va_end(fbArgs);
        _tprintf(_T("[LOG-NO_CS] %s\n"), fallbackBuffer);
        fflush(stdout);
        return;
    }

    EnterCriticalSection(&g_csLog);
    TCHAR buffer[2048];
    va_list args;
    va_start(args, format);
    SYSTEMTIME st;
    GetLocalTime(&st);
    size_t prefixLen = 0;

    StringCchPrintf(buffer, _countof(buffer), _T("%02d:%02d:%02d.%03d "),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    (void)StringCchLength(buffer, _countof(buffer), &prefixLen);

    if (prefixLen < _countof(buffer) - 1) {
        StringCchVPrintf(buffer + prefixLen, _countof(buffer) - prefixLen, format, args);
    }

    StringCchCat(buffer, _countof(buffer), _T("\n"));
    _tprintf_s(buffer);
    fflush(stdout);
    va_end(args);
    LeaveCriticalSection(&g_csLog);
}
void LogError(const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    Log(_T("[ERRO] %s"), buffer);
}
void LogWarning(const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    Log(_T("[AVISO] %s"), buffer);
}


// ==========================================================================================
// Implementações das Funções Auxiliares e Threads
// ==========================================================================================

void ConfigurarValoresRegistry(int* maxLetras, int* ritmoSegundos) {
    HKEY hKey;
    DWORD dwValor;
    DWORD dwSize = sizeof(DWORD);
    LONG lResult;

    *maxLetras = DEFAULT_MAXLETRAS;
    *ritmoSegundos = DEFAULT_RITMO_SEGUNDOS;

    lResult = RegOpenKeyEx(HKEY_CURRENT_USER, REGISTRY_PATH_TP, 0, KEY_READ | KEY_WRITE, &hKey);
    if (lResult == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, REG_MAXLETRAS_NOME, NULL, NULL, (LPBYTE)&dwValor, &dwSize) == ERROR_SUCCESS) {
            if (dwValor > 0 && dwValor <= MAX_LETRAS_TABULEIRO) {
                *maxLetras = (int)dwValor;
            }
            else {
                LogWarning(_T("[REG] MAXLETRAS (%lu) inválido. Usando %d e atualizando registry."), dwValor, MAX_LETRAS_TABULEIRO);
                *maxLetras = MAX_LETRAS_TABULEIRO;
                RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)maxLetras, sizeof(DWORD));
            }
        }
        else {
            LogWarning(_T("[REG] Não leu MAXLETRAS. Usando padrão %d e criando/atualizando."), *maxLetras);
            RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)maxLetras, sizeof(DWORD));
        }
        dwSize = sizeof(DWORD);
        if (RegQueryValueEx(hKey, REG_RITMO_NOME, NULL, NULL, (LPBYTE)&dwValor, &dwSize) == ERROR_SUCCESS) {
            if (dwValor > 0 && dwValor < 300) {
                *ritmoSegundos = (int)dwValor;
            }
            else {
                LogWarning(_T("[REG] RITMO (%lu) inválido. Usando padrão %d e atualizando registry."), dwValor, DEFAULT_RITMO_SEGUNDOS);
                *ritmoSegundos = DEFAULT_RITMO_SEGUNDOS;
                RegSetValueEx(hKey, REG_RITMO_NOME, 0, REG_DWORD, (const BYTE*)ritmoSegundos, sizeof(DWORD));
            }
        }
        else {
            LogWarning(_T("[REG] Não leu RITMO. Usando padrão %d e criando/atualizando."), *ritmoSegundos);
            RegSetValueEx(hKey, REG_RITMO_NOME, 0, REG_DWORD, (const BYTE*)ritmoSegundos, sizeof(DWORD));
        }
        RegCloseKey(hKey);
    }
    else {
        Log(_T("[REG] Chave '%s' não encontrada/acessível. Criando com valores padrão."), REGISTRY_PATH_TP);
        if (RegCreateKeyEx(HKEY_CURRENT_USER, REGISTRY_PATH_TP, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)maxLetras, sizeof(DWORD));
            RegSetValueEx(hKey, REG_RITMO_NOME, 0, REG_DWORD, (const BYTE*)ritmoSegundos, sizeof(DWORD));
            RegCloseKey(hKey);
            Log(_T("[REG] Chave e valores padrão criados."));
        }
        else {
            LogError(_T("[REG] Falha ao criar chave do Registry '%s': %lu"), REGISTRY_PATH_TP, GetLastError());
        }
    }
    if (*maxLetras > MAX_LETRAS_TABULEIRO) *maxLetras = MAX_LETRAS_TABULEIRO;
    if (*maxLetras <= 0) *maxLetras = DEFAULT_MAXLETRAS;
    if (*ritmoSegundos <= 0) *ritmoSegundos = DEFAULT_RITMO_SEGUNDOS;
}


BOOL CarregarDicionarioServidor(DICIONARIO_ARBITRO* dict, const TCHAR* nomeArquivo) {
    InitializeCriticalSection(&dict->csDicionario);
    dict->palavras = NULL;
    dict->totalPalavras = 0;
    FILE* arquivo;

    if (_tfopen_s(&arquivo, nomeArquivo, _T("r, ccs=UTF-8")) != 0 || !arquivo) {
        LogError(_T("[DIC] Erro ao abrir ficheiro de dicionário '%s'."), nomeArquivo);
        DeleteCriticalSection(&dict->csDicionario);
        return FALSE;
    }

    TCHAR linha[MAX_WORD + 2];
    DWORD capacidade = 200;
    dict->palavras = (TCHAR**)malloc(capacidade * sizeof(TCHAR*));
    if (!dict->palavras) {
        fclose(arquivo);
        LogError(_T("[DIC] Falha ao alocar memória inicial para dicionário."));
        DeleteCriticalSection(&dict->csDicionario);
        return FALSE;
    }

    while (_fgetts(linha, MAX_WORD + 1, arquivo)) {
        size_t len = _tcslen(linha);
        while (len > 0 && (linha[len - 1] == _T('\n') || linha[len - 1] == _T('\r'))) {
            linha[len - 1] = _T('\0');
            len--;
        }

        if (len == 0) continue;

        if (dict->totalPalavras >= capacidade) {
            capacidade *= 2;
            TCHAR** temp = (TCHAR**)realloc(dict->palavras, capacidade * sizeof(TCHAR*));
            if (!temp) {
                LogError(_T("[DIC] Falha ao realocar memória para dicionário."));
                for (DWORD i = 0; i < dict->totalPalavras; i++) free(dict->palavras[i]);
                free(dict->palavras); dict->palavras = NULL;
                fclose(arquivo);
                DeleteCriticalSection(&dict->csDicionario);
                return FALSE;
            }
            dict->palavras = temp;
        }

        dict->palavras[dict->totalPalavras] = _tcsdup(linha);
        if (!dict->palavras[dict->totalPalavras]) {
            LogError(_T("[DIC] Falha ao alocar memória para a palavra '%s'."), linha);
            for (DWORD i = 0; i < dict->totalPalavras; i++) free(dict->palavras[i]);
            free(dict->palavras); dict->palavras = NULL;
            fclose(arquivo);
            DeleteCriticalSection(&dict->csDicionario);
            return FALSE;
        }
        for (TCHAR* p = dict->palavras[dict->totalPalavras]; *p; ++p) *p = _totupper(*p);

        dict->totalPalavras++;
    }

    fclose(arquivo);
    Log(_T("[DIC] Dicionário '%s' carregado com %lu palavras."), nomeArquivo, dict->totalPalavras);
    if (dict->totalPalavras == 0) {
        LogWarning(_T("[DIC] Dicionário carregado está vazio!"));
    }
    return TRUE;
}

void LiberarDicionarioServidor(DICIONARIO_ARBITRO* dict) {
    if (dict) {
        if (dict->csDicionario.DebugInfo != NULL) {
            EnterCriticalSection(&dict->csDicionario);
            if (dict->palavras) {
                for (DWORD i = 0; i < dict->totalPalavras; i++) {
                    free(dict->palavras[i]);
                }
                free(dict->palavras);
                dict->palavras = NULL;
            }
            dict->totalPalavras = 0;
            LeaveCriticalSection(&dict->csDicionario);
            DeleteCriticalSection(&dict->csDicionario);
        }
    }
}

BOOL InicializarMemoriaPartilhadaArbitro(int maxLetras) {
    if (maxLetras <= 0 || maxLetras > MAX_LETRAS_TABULEIRO) {
        LogError(_T("[SHM] maxLetras inválido (%d) para inicializar memória partilhada."), maxLetras);
        return FALSE;
    }

    g_hMapFileShm = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DadosJogoCompartilhados), SHM_NAME);
    if (g_hMapFileShm == NULL) {
        LogError(_T("[SHM] Erro ao criar FileMapping (%s): %lu"), SHM_NAME, GetLastError());
        return FALSE;
    }

    g_pDadosShm = (DadosJogoCompartilhados*)MapViewOfFile(g_hMapFileShm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DadosJogoCompartilhados));
    if (g_pDadosShm == NULL) {
        LogError(_T("[SHM] Erro ao mapear SHM (%s): %lu"), SHM_NAME, GetLastError());
        CloseHandle(g_hMapFileShm); g_hMapFileShm = NULL;
        return FALSE;
    }

    g_hEventoShmUpdate = CreateEvent(NULL, TRUE, FALSE, EVENT_SHM_UPDATE);
    if (g_hEventoShmUpdate == NULL) {
        LogError(_T("[SHM] Erro ao criar evento SHM (%s): %lu"), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(g_pDadosShm); g_pDadosShm = NULL;
        CloseHandle(g_hMapFileShm); g_hMapFileShm = NULL;
        return FALSE;
    }

    g_hMutexShm = CreateMutex(NULL, FALSE, MUTEX_SHARED_MEM);
    if (g_hMutexShm == NULL) {
        LogError(_T("[SHM] Erro ao criar mutex SHM (%s): %lu"), MUTEX_SHARED_MEM, GetLastError());
        CloseHandle(g_hEventoShmUpdate); g_hEventoShmUpdate = NULL;
        UnmapViewOfFile(g_pDadosShm); g_pDadosShm = NULL;
        CloseHandle(g_hMapFileShm); g_hMapFileShm = NULL;
        return FALSE;
    }

    WaitForSingleObject(g_hMutexShm, INFINITE);
    g_pDadosShm->numMaxLetrasAtual = maxLetras;
    for (int i = 0; i < MAX_LETRAS_TABULEIRO; i++) {
        g_pDadosShm->letrasVisiveis[i] = (i < maxLetras) ? _T('_') : _T('\0');
    }
    StringCchCopy(g_pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
    StringCchCopy(g_pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
    g_pDadosShm->pontuacaoUltimaPalavra = 0;
    g_pDadosShm->generationCount = 0;
    g_pDadosShm->jogoAtivo = FALSE;
    ReleaseMutex(g_hMutexShm);
    SetEvent(g_hEventoShmUpdate);

    Log(_T("[SHM] Memória partilhada '%s' e evento '%s' inicializados."), SHM_NAME, EVENT_SHM_UPDATE);
    return TRUE;
}

void LimparMemoriaPartilhadaArbitro() {
    if (g_pDadosShm != NULL) UnmapViewOfFile(g_pDadosShm);
    g_pDadosShm = NULL;
    if (g_hMapFileShm != NULL) CloseHandle(g_hMapFileShm);
    g_hMapFileShm = NULL;
    if (g_hEventoShmUpdate != NULL) CloseHandle(g_hEventoShmUpdate);
    g_hEventoShmUpdate = NULL;
    if (g_hMutexShm != NULL) CloseHandle(g_hMutexShm);
    g_hMutexShm = NULL;
    Log(_T("[SHM] Memória partilhada e objetos de sync limpos."));
}


DWORD WINAPI ThreadGestorLetras(LPVOID param) {
    THREAD_ARGS_ARBITRO* args = (THREAD_ARGS_ARBITRO*)param;
    int indiceLetraAntiga = 0;
    Log(_T("[LETRAS] ThreadGestorLetras iniciada."));

    while (*(args->pServidorEmExecucao)) {
        int ritmoAtual;
        WaitForSingleObject(args->hMutexShm, INFINITE);
        ritmoAtual = *(args->pRitmoConfigSegundos);
        ReleaseMutex(args->hMutexShm);

        for (int i = 0; i < ritmoAtual * 10 && *(args->pServidorEmExecucao); ++i) {
            Sleep(100);
        }
        if (!*(args->pServidorEmExecucao)) break;


        if (!*(args->pJogoRealmenteAtivo)) {
            continue;
        }

        WaitForSingleObject(args->hMutexShm, INFINITE);
        if (args->pDadosShm) {
            int maxLetras = args->pDadosShm->numMaxLetrasAtual;
            int posParaNovaLetra = -1;
            int letrasAtuaisNoTabuleiro = 0;

            for (int i = 0; i < maxLetras; i++) {
                if (args->pDadosShm->letrasVisiveis[i] != _T('_')) {
                    letrasAtuaisNoTabuleiro++;
                }
                else if (posParaNovaLetra == -1) {
                    posParaNovaLetra = i;
                }
            }

            if (posParaNovaLetra == -1 && letrasAtuaisNoTabuleiro >= maxLetras) {
                posParaNovaLetra = indiceLetraAntiga;
                indiceLetraAntiga = (indiceLetraAntiga + 1) % maxLetras;
            }
            else if (posParaNovaLetra == -1 && letrasAtuaisNoTabuleiro < maxLetras && maxLetras > 0) {
                posParaNovaLetra = 0;
            }


            if (posParaNovaLetra != -1 && maxLetras > 0) {
                TCHAR novaLetra = _T('A') + (rand() % 26);
                args->pDadosShm->letrasVisiveis[posParaNovaLetra] = novaLetra;
                args->pDadosShm->generationCount++;
                SetEvent(args->hEventoShmUpdate);
            }
        }
        ReleaseMutex(args->hMutexShm);
    }
    Log(_T("[LETRAS] ThreadGestorLetras a terminar."));
    return 0;
}

DWORD WINAPI ThreadAdminArbitro(LPVOID param) {
    THREAD_ARGS_ARBITRO* args = (THREAD_ARGS_ARBITRO*)param;
    TCHAR comando[100];
    TCHAR usernameParam[MAX_USERNAME];
    Log(_T("[ADMIN] ThreadAdminArbitro iniciada."));

    while (*(args->pServidorEmExecucao)) {
        Log(_T("Admin> "));
        if (_fgetts(comando, _countof(comando), stdin) == NULL) {
            if (*(args->pServidorEmExecucao)) {
                if (feof(stdin)) LogWarning(_T("[ADMIN] EOF no stdin. Encerrando thread admin."));
                else LogError(_T("[ADMIN] Erro ao ler comando do admin."));
            }
            break;
        }
        comando[_tcscspn(comando, _T("\r\n"))] = _T('\0');

        if (_tcslen(comando) == 0) continue;

        if (_tcscmp(comando, _T("listar")) == 0) {
            Log(_T("[ADMIN] Comando: listar"));
            EnterCriticalSection(args->pcsListaJogadores);
            Log(_T("--- Lista de Jogadores Ativos (%lu) ---"), *(args->pTotalJogadoresAtivos));
            BOOL encontrouAlgum = FALSE;
            for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
                if (args->jogadores[i].ativo) {
                    Log(_T("  - %s (Pontos: %.1f, Pipe: %p)"), args->jogadores[i].username, args->jogadores[i].pontos, args->jogadores[i].hPipe);
                    encontrouAlgum = TRUE;
                }
            }
            if (!encontrouAlgum) Log(_T("  (Nenhum jogador ativo no momento)"));
            Log(_T("------------------------------------"));
            LeaveCriticalSection(args->pcsListaJogadores);
        }
        else if (_tcsncmp(comando, _T("excluir "), 8) == 0) {
            ZeroMemory(usernameParam, sizeof(usernameParam));
            if (_stscanf_s(comando, _T("excluir %31s"), usernameParam, (unsigned)_countof(usernameParam) - 1) == 1) {
                usernameParam[MAX_USERNAME - 1] = _T('\0');
                Log(_T("[ADMIN] Comando: excluir %s"), usernameParam);
                RemoverJogador(usernameParam, TRUE);
            }
            else {
                LogWarning(_T("[ADMIN] Comando excluir mal formatado. Uso: excluir <username>"));
            }
        }
        else if (_tcscmp(comando, _T("acelerar")) == 0) {
            WaitForSingleObject(args->hMutexShm, INFINITE);
            if (*(args->pRitmoConfigSegundos) > 1) {
                (*(args->pRitmoConfigSegundos))--;
                Log(_T("[ADMIN] Ritmo alterado para %d segundos."), *(args->pRitmoConfigSegundos));
            }
            else {
                Log(_T("[ADMIN] Ritmo já está no mínimo (1 segundo)."));
            }
            ReleaseMutex(args->hMutexShm);
        }
        else if (_tcscmp(comando, _T("travar")) == 0) {
            WaitForSingleObject(args->hMutexShm, INFINITE);
            (*(args->pRitmoConfigSegundos))++;
            Log(_T("[ADMIN] Ritmo alterado para %d segundos."), *(args->pRitmoConfigSegundos));
            ReleaseMutex(args->hMutexShm);
        }
        else if (_tcscmp(comando, _T("encerrar")) == 0) {
            Log(_T("[ADMIN] Comando: encerrar. Servidor a terminar..."));
            *(args->pServidorEmExecucao) = FALSE;

            if (args->hEventoShmUpdate) SetEvent(args->hEventoShmUpdate);

            HANDLE hSelfConnect = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hSelfConnect != INVALID_HANDLE_VALUE) {
                CloseHandle(hSelfConnect);
            }
            break;
        }
        else {
            LogWarning(_T("[ADMIN] Comando desconhecido: '%s'"), comando);
        }
    }
    Log(_T("[ADMIN] ThreadAdminArbitro a terminar."));
    return 0;
}


DWORD WINAPI ThreadClienteConectado(LPVOID param) {
    THREAD_ARGS_ARBITRO* args = (THREAD_ARGS_ARBITRO*)param;
    HANDLE hPipe = args->hPipeCliente;
    MESSAGE msg;
    DWORD bytesLidos, bytesEscritos;
    JOGADOR_INFO_ARBITRO* meuJogadorInfo = NULL;
    TCHAR usernameEsteCliente[MAX_USERNAME];
    usernameEsteCliente[0] = _T('\0');
    BOOL esteClienteAtivo = FALSE;
    int meuIndiceNaLista = -1;

    Log(_T("[CLIENT_THREAD %p] Nova thread para cliente iniciada."), hPipe);

    OVERLAPPED olRead; ZeroMemory(&olRead, sizeof(OVERLAPPED));
    olRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (olRead.hEvent == NULL) { LogError(_T("[CLIENT_THREAD %p] Falha ao criar evento de leitura."), hPipe); free(args); return 1; }


    if (ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesLidos, &olRead) || GetLastError() == ERROR_IO_PENDING) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(olRead.hEvent, IO_TIMEOUT) != WAIT_OBJECT_0) {
                LogError(_T("[CLIENT_THREAD %p] Timeout/Erro JOIN ReadFile (%lu)."), hPipe, GetLastError());
                goto cleanup_cliente_thread;
            }
            if (!GetOverlappedResult(hPipe, &olRead, &bytesLidos, FALSE)) {
                LogError(_T("[CLIENT_THREAD %p] GetOverlappedResult JOIN falhou (%lu)."), hPipe, GetLastError());
                goto cleanup_cliente_thread;
            }
        }

        if (bytesLidos == sizeof(MESSAGE) && _tcscmp(msg.type, _T("JOIN")) == 0) {
            StringCchCopy(usernameEsteCliente, MAX_USERNAME, msg.username);
            Log(_T("[CLIENT_THREAD %p] Recebido JOIN de '%s'."), hPipe, usernameEsteCliente);

            EnterCriticalSection(args->pcsListaJogadores);
            int idxExistente = EncontrarJogador(usernameEsteCliente);
            if (idxExistente != -1) {
                LeaveCriticalSection(args->pcsListaJogadores);
                LogWarning(_T("[CLIENT_THREAD %p] Username '%s' já em uso."), hPipe, usernameEsteCliente);
                MESSAGE resp; ZeroMemory(&resp, sizeof(MESSAGE)); StringCchCopy(resp.type, _countof(resp.type), _T("JOIN_USER_EXISTS")); StringCchCopy(resp.data, _countof(resp.data), _T("Username em uso."));
                WriteFile(hPipe, &resp, sizeof(MESSAGE), &bytesEscritos, NULL);
                goto cleanup_cliente_thread;
            }
            if (*(args->pTotalJogadoresAtivos) >= MAX_JOGADORES) {
                LeaveCriticalSection(args->pcsListaJogadores);
                LogWarning(_T("[CLIENT_THREAD %p] Jogo cheio. Rejeitando '%s'."), hPipe, usernameEsteCliente);
                MESSAGE resp; ZeroMemory(&resp, sizeof(MESSAGE)); StringCchCopy(resp.type, _countof(resp.type), _T("JOIN_GAME_FULL")); StringCchCopy(resp.data, _countof(resp.data), _T("Jogo cheio."));
                WriteFile(hPipe, &resp, sizeof(MESSAGE), &bytesEscritos, NULL);
                goto cleanup_cliente_thread;
            }

            for (int i = 0; i < MAX_JOGADORES; ++i) {
                if (!args->jogadores[i].ativo) {
                    meuIndiceNaLista = i;
                    meuJogadorInfo = &args->jogadores[i];
                    StringCchCopy(meuJogadorInfo->username, MAX_USERNAME, usernameEsteCliente);
                    meuJogadorInfo->pontos = 0.0f;
                    meuJogadorInfo->hPipe = hPipe;
                    meuJogadorInfo->ativo = TRUE;
                    meuJogadorInfo->dwThreadIdCliente = GetCurrentThreadId();
                    (*(args->pTotalJogadoresAtivos))++;
                    esteClienteAtivo = TRUE;
                    Log(_T("[CLIENT_THREAD %p] Jogador '%s' adicionado (idx %d). Total: %lu"), hPipe, usernameEsteCliente, meuIndiceNaLista, *(args->pTotalJogadoresAtivos));

                    MESSAGE respOK; ZeroMemory(&respOK, sizeof(MESSAGE)); StringCchCopy(respOK.type, _countof(respOK.type), _T("JOIN_OK"));
                    StringCchPrintf(respOK.data, _countof(respOK.data), _T("Bem-vindo, %s!"), usernameEsteCliente);
                    WriteFile(hPipe, &respOK, sizeof(MESSAGE), &bytesEscritos, NULL);

                    MESSAGE notifJoin; ZeroMemory(&notifJoin, sizeof(MESSAGE)); StringCchCopy(notifJoin.type, _countof(notifJoin.type), _T("GAME_UPDATE"));
                    StringCchPrintf(notifJoin.data, _countof(notifJoin.data), _T("Jogador %s entrou no jogo."), usernameEsteCliente);
                    NotificarTodosOsJogadores(&notifJoin, usernameEsteCliente);

                    VerificarEstadoJogo();
                    break;
                }
            }
            LeaveCriticalSection(args->pcsListaJogadores);
            if (!esteClienteAtivo) { LogError(_T("[CLIENT_THREAD %p] Falha ao adicionar '%s'."), hPipe, usernameEsteCliente); goto cleanup_cliente_thread; }
        }
        else { LogError(_T("[CLIENT_THREAD %p] Primeira msg não foi JOIN ou bytes errados. Desconectando."), hPipe); goto cleanup_cliente_thread; }
    }
    else { LogError(_T("[CLIENT_THREAD %p] Falha ao ler 1ª msg: %lu."), hPipe, GetLastError()); goto cleanup_cliente_thread; }

    while (*(args->pServidorEmExecucao) && esteClienteAtivo) {
        ResetEvent(olRead.hEvent);
        BOOL sucessoLeitura = ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesLidos, &olRead);
        DWORD dwErrorRead = GetLastError();

        if (!sucessoLeitura && dwErrorRead == ERROR_IO_PENDING) {
            DWORD waitRes = WaitForSingleObject(olRead.hEvent, READ_TIMEOUT_THREAD_JOGADOR);
            if (waitRes == WAIT_TIMEOUT) {
                if (!*(args->pServidorEmExecucao)) break;
                continue;
            }
            else if (waitRes != WAIT_OBJECT_0) {
                LogError(_T("[CLIENT_THREAD %p] Erro %lu ao esperar ReadFile para '%s'."), hPipe, GetLastError(), usernameEsteCliente);
                esteClienteAtivo = FALSE; break;
            }
            if (!GetOverlappedResult(hPipe, &olRead, &bytesLidos, FALSE)) {
                LogError(_T("[CLIENT_THREAD %p] GOR falhou após ReadFile para '%s': %lu."), hPipe, usernameEsteCliente, GetLastError());
                esteClienteAtivo = FALSE; break;
            }
            sucessoLeitura = TRUE;
        }
        else if (!sucessoLeitura) {
            if (*(args->pServidorEmExecucao)) LogError(_T("[CLIENT_THREAD %p] ReadFile falhou para '%s': %lu."), hPipe, usernameEsteCliente, dwErrorRead);
            esteClienteAtivo = FALSE; break;
        }

        if (!g_servidorEmExecucao) break;

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {
            EnterCriticalSection(args->pcsListaJogadores);
            if (meuIndiceNaLista == -1 ||
                meuIndiceNaLista >= MAX_JOGADORES ||
                !args->jogadores[meuIndiceNaLista].ativo ||
                args->jogadores[meuIndiceNaLista].hPipe != hPipe) {
                LeaveCriticalSection(args->pcsListaJogadores);
                LogWarning(_T("[CLIENT_THREAD %p] Jogador '%s' tornou-se inválido. Encerrando."), hPipe, usernameEsteCliente);
                esteClienteAtivo = FALSE; break;
            }
            meuJogadorInfo = &args->jogadores[meuIndiceNaLista];
            LeaveCriticalSection(args->pcsListaJogadores);


            if (_tcscmp(msg.type, _T("WORD")) == 0) {
                Log(_T("[CLIENT_THREAD %p] '%s' submeteu: '%s'"), hPipe, usernameEsteCliente, msg.data);
                ValidarPalavraJogo(msg.data, usernameEsteCliente, args);
            }
            else if (_tcscmp(msg.type, _T("GET_SCORE")) == 0) {
                MESSAGE respScore; ZeroMemory(&respScore, sizeof(MESSAGE));
                StringCchCopy(respScore.type, _countof(respScore.type), _T("SCORE_UPDATE"));
                EnterCriticalSection(args->pcsListaJogadores);
                if (meuJogadorInfo && meuJogadorInfo->ativo) {
                    StringCchPrintf(respScore.data, _countof(respScore.data), _T("%.1f"), meuJogadorInfo->pontos);
                    respScore.pontos = (int)meuJogadorInfo->pontos;
                }
                else { StringCchCopy(respScore.data, _countof(respScore.data), _T("Erro.")); respScore.pontos = 0; }
                LeaveCriticalSection(args->pcsListaJogadores);
                WriteFile(hPipe, &respScore, sizeof(MESSAGE), &bytesEscritos, NULL);
            }
            else if (_tcscmp(msg.type, _T("GET_JOGS")) == 0) {
                MESSAGE respJogs; ZeroMemory(&respJogs, sizeof(MESSAGE));
                StringCchCopy(respJogs.type, _countof(respJogs.type), _T("PLAYER_LIST_UPDATE"));

                TCHAR listaBuffer[sizeof(respJogs.data)];
                listaBuffer[0] = _T('\0');
                size_t bufferRestante = _countof(listaBuffer);
                TCHAR* pBufferAtual = listaBuffer;
                size_t cchLinhaEscrita;

                StringCchCat(pBufferAtual, bufferRestante, _T("Jogadores Ativos:\n"));
                (void)StringCchLength(pBufferAtual, bufferRestante, &cchLinhaEscrita); pBufferAtual += cchLinhaEscrita; bufferRestante -= cchLinhaEscrita;

                EnterCriticalSection(args->pcsListaJogadores);
                for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
                    if (args->jogadores[i].ativo && bufferRestante > 1) {
                        TCHAR linhaJogador[100];
                        StringCchPrintf(linhaJogador, _countof(linhaJogador), _T(" - %s (%.1f pts)\n"),
                            args->jogadores[i].username, args->jogadores[i].pontos);

                        StringCchCat(pBufferAtual, bufferRestante, linhaJogador);
                        (void)StringCchLength(pBufferAtual, bufferRestante, &cchLinhaEscrita); pBufferAtual += cchLinhaEscrita; bufferRestante -= cchLinhaEscrita;
                    }
                }
                LeaveCriticalSection(args->pcsListaJogadores);
                StringCchCopy(respJogs.data, _countof(respJogs.data), listaBuffer);
                WriteFile(hPipe, &respJogs, sizeof(MESSAGE), &bytesEscritos, NULL);
            }
            else if (_tcscmp(msg.type, _T("EXIT")) == 0) {
                Log(_T("[CLIENT_THREAD %p] '%s' solicitou sair."), hPipe, usernameEsteCliente);
                esteClienteAtivo = FALSE;
                break;
            }
            else { LogWarning(_T("[CLIENT_THREAD %p] '%s' enviou msg desconhecida: '%s'"), hPipe, usernameEsteCliente, msg.type); }
        }
        else if (!sucessoLeitura) {
            if (*(args->pServidorEmExecucao)) LogError(_T("[CLIENT_THREAD %p] Erro final ReadFile para '%s': %lu."), hPipe, usernameEsteCliente, GetLastError());
            esteClienteAtivo = FALSE; break;
        }
        else if (bytesLidos == 0 && sucessoLeitura) {
            if (*(args->pServidorEmExecucao)) Log(_T("[CLIENT_THREAD %p] Cliente '%s' desconectou (EOF)."), hPipe, usernameEsteCliente);
            esteClienteAtivo = FALSE; break;
        }
        else if (bytesLidos != 0) {
            if (*(args->pServidorEmExecucao)) LogError(_T("[CLIENT_THREAD %p] Erro de framing para '%s'. Bytes: %lu."), hPipe, usernameEsteCliente, bytesLidos);
            esteClienteAtivo = FALSE; break;
        }
    }

cleanup_cliente_thread:
    Log(_T("[CLIENT_THREAD %p] Limpando para '%s'..."), hPipe, usernameEsteCliente[0] ? usernameEsteCliente : _T("Desconhecido"));
    if (usernameEsteCliente[0] != _T('\0')) {
        RemoverJogador(usernameEsteCliente, FALSE);
    }
    else if (meuIndiceNaLista != -1) {
        EnterCriticalSection(args->pcsListaJogadores);
        if (meuIndiceNaLista >= 0 && meuIndiceNaLista < MAX_JOGADORES &&
            args->jogadores[meuIndiceNaLista].ativo &&
            args->jogadores[meuIndiceNaLista].hPipe == hPipe) {
            args->jogadores[meuIndiceNaLista].ativo = FALSE;
            args->jogadores[meuIndiceNaLista].hPipe = INVALID_HANDLE_VALUE;
            (*(args->pTotalJogadoresAtivos))--;
            LogWarning(_T("[CLIENT_THREAD %p] Jogador parcialmente adicionado (idx %d) limpo."), hPipe, meuIndiceNaLista);
            VerificarEstadoJogo();
        }
        LeaveCriticalSection(args->pcsListaJogadores);
    }

    if (olRead.hEvent) CloseHandle(olRead.hEvent);

    if (hPipe != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(hPipe); CloseHandle(hPipe); }
    if (args != NULL) free(args);
    Log(_T("[CLIENT_THREAD %p] Thread para '%s' terminada."), hPipe, usernameEsteCliente[0] ? usernameEsteCliente : _T("Desconhecido"));
    return 0;
}


// Funções de gestão de jogadores e jogo 

void RemoverJogador(const TCHAR* username, BOOL notificarClienteParaSair) {
    Log(_T("[JOGADOR] Tentando remover '%s'..."), username);
    int idxRemovido = -1;
    HANDLE hPipeDoRemovido = INVALID_HANDLE_VALUE;

    EnterCriticalSection(&g_csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (g_listaJogadores[i].ativo && _tcscmp(g_listaJogadores[i].username, username) == 0) {
            Log(_T("[JOGADOR] '%s' (idx %lu) encontrado para remoção."), username, i);
            g_listaJogadores[i].ativo = FALSE;
            hPipeDoRemovido = g_listaJogadores[i].hPipe;
            g_listaJogadores[i].hPipe = INVALID_HANDLE_VALUE;
            if (g_totalJogadoresAtivos > 0) g_totalJogadoresAtivos--;
            idxRemovido = (int)i;
            Log(_T("[JOGADOR] '%s' removido. Total ativos: %lu"), username, g_totalJogadoresAtivos);
            break;
        }
    }
    LeaveCriticalSection(&g_csListaJogadores);

    if (idxRemovido != -1) {
        if (notificarClienteParaSair && hPipeDoRemovido != INVALID_HANDLE_VALUE) {
            MESSAGE msgKick; ZeroMemory(&msgKick, sizeof(MESSAGE));
            StringCchCopy(msgKick.type, _countof(msgKick.type), _T("SHUTDOWN"));
            StringCchCopy(msgKick.data, _countof(msgKick.data), _T("Você foi desconectado/excluído."));
            DWORD bw;
            WriteFile(hPipeDoRemovido, &msgKick, sizeof(MESSAGE), &bw, NULL);
        }

        MESSAGE msgNotificacaoSaida; ZeroMemory(&msgNotificacaoSaida, sizeof(MESSAGE));
        StringCchCopy(msgNotificacaoSaida.type, _countof(msgNotificacaoSaida.type), _T("GAME_UPDATE"));
        StringCchPrintf(msgNotificacaoSaida.data, _countof(msgNotificacaoSaida.data), _T("Jogador %s saiu do jogo."), username);
        NotificarTodosOsJogadores(&msgNotificacaoSaida, username);
        VerificarEstadoJogo();
    }
    else {
        LogWarning(_T("[JOGADOR] '%s' não encontrado para remoção ou já inativo."), username);
    }
}


int EncontrarJogador(const TCHAR* username) {
    // Assume que g_csListaJogadores JÁ ESTÁ DETIDO pelo chamador
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (g_listaJogadores[i].ativo && _tcscmp(g_listaJogadores[i].username, username) == 0) {
            return (int)i;
        }
    }
    return -1;
}

BOOL ValidarPalavraJogo(const TCHAR* palavraSubmetida, const TCHAR* usernameJogador, THREAD_ARGS_ARBITRO* argsClienteThread) {
    MESSAGE resposta; ZeroMemory(&resposta, sizeof(MESSAGE));
    StringCchCopy(resposta.username, _countof(resposta.username), usernameJogador);
    float pontosAlterados = 0.0f;

    if (!*(argsClienteThread->pJogoRealmenteAtivo)) {
        StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
        StringCchCopy(resposta.data, _countof(resposta.data), _T("O jogo não está ativo."));
    }
    else {
        TCHAR palavraUpper[MAX_WORD];
        StringCchCopy(palavraUpper, MAX_WORD, palavraSubmetida);
        _tcsupr_s(palavraUpper, MAX_WORD);

        BOOL existeNoDicionario = FALSE;
        EnterCriticalSection(&(argsClienteThread->pDicionario->csDicionario));
        for (DWORD i = 0; i < argsClienteThread->pDicionario->totalPalavras; ++i) {
            if (_tcscmp(argsClienteThread->pDicionario->palavras[i], palavraUpper) == 0) {
                existeNoDicionario = TRUE;
                break;
            }
        }
        LeaveCriticalSection(&(argsClienteThread->pDicionario->csDicionario));

        if (!existeNoDicionario) {
            StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
            StringCchPrintf(resposta.data, _countof(resposta.data), _T("'%s' não existe no dicionário."), palavraSubmetida);
            pontosAlterados = -((float)_tcslen(palavraSubmetida) * 0.5f);
        }
        else {
            WaitForSingleObject(argsClienteThread->hMutexShm, INFINITE);
            BOOL podeFormar = TRUE;
            TCHAR letrasDisponiveisCopia[MAX_LETRAS_TABULEIRO];
            for (int k = 0; k < argsClienteThread->pDadosShm->numMaxLetrasAtual; ++k) letrasDisponiveisCopia[k] = argsClienteThread->pDadosShm->letrasVisiveis[k];

            size_t lenPalavra = _tcslen(palavraUpper);
            for (size_t k = 0; k < lenPalavra; ++k) {
                BOOL encontrouLetra = FALSE;
                for (int m = 0; m < argsClienteThread->pDadosShm->numMaxLetrasAtual; ++m) {
                    if (letrasDisponiveisCopia[m] == palavraUpper[k]) {
                        letrasDisponiveisCopia[m] = _T(' ');
                        encontrouLetra = TRUE;
                        break;
                    }
                }
                if (!encontrouLetra) { podeFormar = FALSE; break; }
            }

            if (podeFormar) {
                StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_VALID"));
                pontosAlterados = (float)_tcslen(palavraSubmetida);
                StringCchPrintf(resposta.data, _countof(resposta.data), _T("'%s' válida! +%.1f pts."), palavraSubmetida, pontosAlterados);

                for (size_t k = 0; k < lenPalavra; ++k) {
                    for (int m = 0; m < argsClienteThread->pDadosShm->numMaxLetrasAtual; ++m) {
                        if (argsClienteThread->pDadosShm->letrasVisiveis[m] == palavraUpper[k]) {
                            argsClienteThread->pDadosShm->letrasVisiveis[m] = _T('_');
                            break;
                        }
                    }
                }
                argsClienteThread->pDadosShm->generationCount++;
                StringCchCopy(argsClienteThread->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, palavraSubmetida);
                StringCchCopy(argsClienteThread->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, usernameJogador);
                argsClienteThread->pDadosShm->pontuacaoUltimaPalavra = (int)pontosAlterados;
                SetEvent(argsClienteThread->hEventoShmUpdate);

                EnterCriticalSection(argsClienteThread->pcsListaJogadores);
                int idxJogador = EncontrarJogador(usernameJogador);
                if (idxJogador != -1) {
                    argsClienteThread->jogadores[idxJogador].pontos += pontosAlterados;
                    Log(_T("[JOGO] %s acertou! Pts: %.1f"), usernameJogador, argsClienteThread->jogadores[idxJogador].pontos);
                }
                LeaveCriticalSection(argsClienteThread->pcsListaJogadores);

                MESSAGE notifPalavra; ZeroMemory(&notifPalavra, sizeof(MESSAGE));
                StringCchCopy(notifPalavra.type, _countof(notifPalavra.type), _T("GAME_UPDATE"));
                StringCchPrintf(notifPalavra.data, _countof(notifPalavra.data), _T("%s acertou '%s' (+%.1f pts)!"), usernameJogador, palavraSubmetida, pontosAlterados);
                NotificarTodosOsJogadores(&notifPalavra, NULL);
            }
            else {
                StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
                StringCchPrintf(resposta.data, _countof(resposta.data), _T("'%s' não pode ser formada."), palavraSubmetida);
                pontosAlterados = -((float)_tcslen(palavraSubmetida) * 0.5f);
            }
            ReleaseMutex(argsClienteThread->hMutexShm);
        }
    }
    resposta.pontos = (int)pontosAlterados;

    if (pontosAlterados < 0) {
        EnterCriticalSection(argsClienteThread->pcsListaJogadores);
        int idxJogador = EncontrarJogador(usernameJogador);
        if (idxJogador != -1) {
            argsClienteThread->jogadores[idxJogador].pontos += pontosAlterados;
            Log(_T("[JOGO] %s errou. Pts: %.1f"), usernameJogador, argsClienteThread->jogadores[idxJogador].pontos);
        }
        LeaveCriticalSection(argsClienteThread->pcsListaJogadores);
    }

    DWORD bytesEscritos;
    WriteFile(argsClienteThread->hPipeCliente, &resposta, sizeof(MESSAGE), &bytesEscritos, NULL);
    return (_tcscmp(resposta.type, _T("WORD_VALID")) == 0);
}

void VerificarEstadoJogo() {
    EnterCriticalSection(&g_csListaJogadores);
    BOOL deveEstarAtivo = (g_totalJogadoresAtivos >= 2);
    LeaveCriticalSection(&g_csListaJogadores);

    WaitForSingleObject(g_hMutexShm, INFINITE);
    BOOL estavaAtivo = g_pDadosShm->jogoAtivo;

    if (deveEstarAtivo && !estavaAtivo) {
        g_pDadosShm->jogoAtivo = TRUE;
        g_jogoRealmenteAtivo = TRUE;
        Log(_T("[JOGO] O jogo começou/recomeçou! (%lu jogadores)"), g_totalJogadoresAtivos);
        for (int i = 0; i < g_pDadosShm->numMaxLetrasAtual; ++i) g_pDadosShm->letrasVisiveis[i] = _T('_');
        StringCchCopy(g_pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
        StringCchCopy(g_pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
        g_pDadosShm->pontuacaoUltimaPalavra = 0;
        g_pDadosShm->generationCount++;
        SetEvent(g_hEventoShmUpdate);
        ReleaseMutex(g_hMutexShm);

        MESSAGE msgJogoIniciou; ZeroMemory(&msgJogoIniciou, sizeof(MESSAGE));
        StringCchCopy(msgJogoIniciou.type, _countof(msgJogoIniciou.type), _T("GAME_UPDATE"));
        StringCchCopy(msgJogoIniciou.data, _countof(msgJogoIniciou.data), _T("O jogo começou! Boa sorte!"));
        NotificarTodosOsJogadores(&msgJogoIniciou, NULL);
    }
    else if (!deveEstarAtivo && estavaAtivo) {
        g_pDadosShm->jogoAtivo = FALSE;
        g_jogoRealmenteAtivo = FALSE;
        Log(_T("[JOGO] Jogo parado. Menos de 2 jogadores. (%lu jogadores)"), g_totalJogadoresAtivos);
        for (int i = 0; i < g_pDadosShm->numMaxLetrasAtual; ++i) g_pDadosShm->letrasVisiveis[i] = _T('_');
        StringCchCopy(g_pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
        StringCchCopy(g_pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
        g_pDadosShm->pontuacaoUltimaPalavra = 0;
        g_pDadosShm->generationCount++;
        SetEvent(g_hEventoShmUpdate);
        ReleaseMutex(g_hMutexShm);

        MESSAGE msgJogoParou; ZeroMemory(&msgJogoParou, sizeof(MESSAGE));
        StringCchCopy(msgJogoParou.type, _countof(msgJogoParou.type), _T("GAME_UPDATE"));
        StringCchCopy(msgJogoParou.data, _countof(msgJogoParou.data), _T("Jogo parado. Aguardando mais jogadores..."));
        NotificarTodosOsJogadores(&msgJogoParou, NULL);
    }
    else {
        ReleaseMutex(g_hMutexShm);
    }
}

// Definição da função NotificarTodosOsJogadores
void NotificarTodosOsJogadores(const MESSAGE* msgAEnviar, const TCHAR* skipUsername) {
    EnterCriticalSection(&g_csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (g_listaJogadores[i].ativo && g_listaJogadores[i].hPipe != INVALID_HANDLE_VALUE) {
            if (skipUsername == NULL || _tcscmp(g_listaJogadores[i].username, skipUsername) != 0) {
                DWORD bytesEscritos;
                OVERLAPPED olNotify; ZeroMemory(&olNotify, sizeof(OVERLAPPED));
                olNotify.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
                if (olNotify.hEvent == NULL) {
                    LogError(_T("[NOTIFICAR] Falha ao criar evento para notificação para %s."), g_listaJogadores[i].username);
                    continue;
                }

                if (!WriteFile(g_listaJogadores[i].hPipe, msgAEnviar, sizeof(MESSAGE), &bytesEscritos, &olNotify)) {
                    if (GetLastError() == ERROR_IO_PENDING) {
                        if (WaitForSingleObject(olNotify.hEvent, 1000) == WAIT_OBJECT_0) {
                            GetOverlappedResult(g_listaJogadores[i].hPipe, &olNotify, &bytesEscritos, FALSE);
                            if (bytesEscritos != sizeof(MESSAGE)) {
                                LogWarning(_T("[NOTIFICAR] WriteFile Overlapped enviou %lu bytes em vez de %zu para %s."), bytesEscritos, sizeof(MESSAGE), g_listaJogadores[i].username);
                            }
                        }
                        else {
                            LogWarning(_T("[NOTIFICAR] Timeout ao enviar notificação para %s. Cancelando IO."), g_listaJogadores[i].username);
                            CancelIoEx(g_listaJogadores[i].hPipe, &olNotify);
                        }
                    }
                    else {
                        LogWarning(_T("[NOTIFICAR] Falha ao enviar notificação para %s (pipe %p): %lu"), g_listaJogadores[i].username, g_listaJogadores[i].hPipe, GetLastError());
                    }
                }
                else {
                    GetOverlappedResult(g_listaJogadores[i].hPipe, &olNotify, &bytesEscritos, FALSE);
                    if (bytesEscritos != sizeof(MESSAGE)) {
                        LogWarning(_T("[NOTIFICAR] WriteFile Síncrono (com overlapped handle) enviou %lu bytes em vez de %zu para %s."), bytesEscritos, sizeof(MESSAGE), g_listaJogadores[i].username);
                    }
                }
                CloseHandle(olNotify.hEvent);
            }
        }
    }
    LeaveCriticalSection(&g_csListaJogadores);
}
