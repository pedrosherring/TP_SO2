#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h> // Para malloc, free, realloc, rand, srand, _tstoi
#include <tchar.h>
#include <fcntl.h>   // Para _setmode
#include <io.h>      // Para _setmode, _fileno
#include <time.h>    // Para srand, time
#include <strsafe.h> // Para StringCchPrintf, etc.


#include "../Comum/compartilhado.h" // Ficheiro revisto

// Definição manual de _countof se não estiver disponível
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array[0]))
#endif

// Definições de Timeout
#define IO_TIMEOUT 5000
#define CONNECT_TIMEOUT 500
#define READ_TIMEOUT_THREAD_JOGADOR 500


#define MIN_JOGADORES_PARA_INICIAR 2

// Estruturas internas do árbitro (mantidas, pois são usadas dentro do SERVER_CONTEXT)
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

// ESTRUTURA PRINCIPAL PARA O ESTADO DO SERVIDOR (NÃO MAIS GLOBAL)
typedef struct {
    // Gestão de Jogadores
    JOGADOR_INFO_ARBITRO listaJogadores[MAX_JOGADORES];
    DWORD totalJogadoresAtivos;
    CRITICAL_SECTION csListaJogadores;

    // Memória Partilhada
    DadosJogoCompartilhados* pDadosShm;
    HANDLE hMapFileShm; // Necessário para cleanup
    HANDLE hEventoShmUpdate;
    HANDLE hMutexShm;

    // Dicionário
    DICIONARIO_ARBITRO dicionario;

    // Configurações do Jogo
    int maxLetrasConfig;
    int ritmoConfigSegundos;

    // Controlo do Servidor e Jogo
    volatile BOOL servidorEmExecucao;
    volatile BOOL jogoRealmenteAtivo;

    // Controlo de Logs
    CRITICAL_SECTION csLog; // Esta será usada pelas funções de Log
} SERVER_CONTEXT;

// ESTRUTURA DE ARGUMENTOS PARA AS THREADS (permanece igual)
typedef struct {
    SERVER_CONTEXT* serverCtx;   // Ponteiro para o contexto do servidor (agora local em _tmain)
    HANDLE hPipeCliente;          // Específico para ThreadClienteConectado, INVALID_HANDLE_VALUE para outras
} THREAD_ARGS;

// ==========================================================================================
// PROTÓTIPOS DE FUNÇÕES INTERNAS (agora, Log* tamb�m recebem csLog)
// ==========================================================================================
void Log(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...);
void LogError(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...);
void LogWarning(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...);

BOOL InicializarServidor(SERVER_CONTEXT* ctx); // Recebe SERVER_CONTEXT*
void EncerrarServidor(SERVER_CONTEXT* ctx);    // Recebe SERVER_CONTEXT*
void ConfigurarValoresRegistry(SERVER_CONTEXT* ctx); // Recebe SERVER_CONTEXT*
BOOL CarregarDicionarioServidor(SERVER_CONTEXT* ctx, const TCHAR* nomeArquivo);
void LiberarDicionarioServidor(SERVER_CONTEXT* ctx);
BOOL InicializarMemoriaPartilhadaArbitro(SERVER_CONTEXT* ctx, int maxLetras);
void LimparMemoriaPartilhadaArbitro(SERVER_CONTEXT* ctx);

DWORD WINAPI ThreadGestorLetras(LPVOID param);
DWORD WINAPI ThreadAdminArbitro(LPVOID param);
DWORD WINAPI ThreadClienteConectado(LPVOID param);

void RemoverJogador(SERVER_CONTEXT* ctx, const TCHAR* username, BOOL notificarClienteParaSair);
int EncontrarJogador(SERVER_CONTEXT* ctx, const TCHAR* username); // Caller must hold csListaJogadores
void NotificarTodosOsJogadores(SERVER_CONTEXT* ctx, const MESSAGE* msgAEnviar, const TCHAR* skipUsername);
BOOL ValidarPalavraJogo(THREAD_ARGS* argsClienteThread, const TCHAR* palavraSubmetida, const TCHAR* usernameJogador);
void VerificarEstadoJogo(SERVER_CONTEXT* ctx);


// ==========================================================================================
// FUNÇÃO PRINCIPAL - _tmain
// ==========================================================================================
int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    SERVER_CONTEXT serverCtx; // Instância local do contexto do servidor
    ZeroMemory(&serverCtx, sizeof(SERVER_CONTEXT));

    // Inicializar csLog ANTES de qualquer chamada a Log()
    InitializeCriticalSection(&serverCtx.csLog);

    Log(&serverCtx.csLog, _T("[ARBITRO] Iniciando árbitro..."));

    if (!InicializarServidor(&serverCtx)) { // Passa o ponteiro para o contexto local
        LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao inicializar o servidor. Encerrando."));
        EncerrarServidor(&serverCtx); // Passa o contexto
        DeleteCriticalSection(&serverCtx.csLog);
        return 1;
    }

    HANDLE hThreads[2] = { NULL, NULL };
    THREAD_ARGS argsServico; // Argumentos para threads de serviço (GestorLetras, Admin)
    argsServico.serverCtx = &serverCtx; // Aponta para o contexto local
    argsServico.hPipeCliente = INVALID_HANDLE_VALUE;

    hThreads[0] = CreateThread(NULL, 0, ThreadGestorLetras, &argsServico, 0, NULL);
    if (hThreads[0] == NULL) {
        LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao criar ThreadGestorLetras. Encerrando."));
        serverCtx.servidorEmExecucao = FALSE;
        EncerrarServidor(&serverCtx);
        DeleteCriticalSection(&serverCtx.csLog);
        return 1;
    }
    hThreads[1] = CreateThread(NULL, 0, ThreadAdminArbitro, &argsServico, 0, NULL);
    if (hThreads[1] == NULL) {
        LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao criar ThreadAdminArbitro. Encerrando."));
        serverCtx.servidorEmExecucao = FALSE;
        if (hThreads[0] != NULL) { WaitForSingleObject(hThreads[0], INFINITE); CloseHandle(hThreads[0]); }
        EncerrarServidor(&serverCtx);
        DeleteCriticalSection(&serverCtx.csLog);
        return 1;
    }

    Log(&serverCtx.csLog, _T("[ARBITRO] Servidor pronto. Aguardando conexões de jogadores em %s"), PIPE_NAME);

    while (serverCtx.servidorEmExecucao) {
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
            if (serverCtx.servidorEmExecucao) {
                LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao criar Named Pipe (instância): %lu"), GetLastError());
                Sleep(1000);
            }
            continue;
        }

        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(OVERLAPPED));
        ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (ov.hEvent == NULL) {
            LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao criar evento para ConnectNamedPipe: %lu"), GetLastError());
            CloseHandle(hPipe);
            continue;
        }

        BOOL fConnected = ConnectNamedPipe(hPipe, &ov);
        if (!fConnected && GetLastError() == ERROR_IO_PENDING) {
            DWORD dwWaitResult = WaitForSingleObject(ov.hEvent, CONNECT_TIMEOUT);
            if (dwWaitResult == WAIT_OBJECT_0) {
                DWORD dwDummy;
                fConnected = GetOverlappedResult(hPipe, &ov, &dwDummy, FALSE);
                if (!fConnected) LogError(&serverCtx.csLog, _T("[ARBITRO] GetOverlappedResult falhou após evento: %lu"), GetLastError());
            }
            else if (dwWaitResult == WAIT_TIMEOUT) {
                CancelIo(hPipe);
                fConnected = FALSE;
            }
            else {
                LogError(&serverCtx.csLog, _T("[ARBITRO] Erro %lu ao aguardar conexão no pipe %p."), GetLastError(), hPipe);
                fConnected = FALSE;
            }
        }
        else if (!fConnected && GetLastError() == ERROR_PIPE_CONNECTED) {
            fConnected = TRUE;
        }
        else if (!fConnected) {
            LogError(&serverCtx.csLog, _T("[ARBITRO] ConnectNamedPipe falhou imediatamente: %lu"), GetLastError());
        }
        CloseHandle(ov.hEvent);

        if (fConnected && serverCtx.servidorEmExecucao) {
            EnterCriticalSection(&serverCtx.csListaJogadores);
            if (serverCtx.totalJogadoresAtivos < MAX_JOGADORES) {
                LeaveCriticalSection(&serverCtx.csListaJogadores);

                THREAD_ARGS* argsCliente = (THREAD_ARGS*)malloc(sizeof(THREAD_ARGS));
                if (argsCliente == NULL) {
                    LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao alocar memória para THREAD_ARGS."));
                    DisconnectNamedPipe(hPipe);
                    CloseHandle(hPipe);
                }
                else {
                    argsCliente->serverCtx = &serverCtx; // Passa o contexto do servidor
                    argsCliente->hPipeCliente = hPipe;   // Passa o pipe específico do cliente

                    HANDLE hThreadCliente = CreateThread(NULL, 0, ThreadClienteConectado, argsCliente, 0, NULL);
                    if (hThreadCliente == NULL) {
                        LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao criar ThreadClienteConectado para pipe %p: %lu"), hPipe, GetLastError());
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
                LeaveCriticalSection(&serverCtx.csListaJogadores);
                LogWarning(&serverCtx.csLog, _T("[ARBITRO] Jogo cheio. Rejeitando conex�o no pipe %p."), hPipe);
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
                CloseHandle(hPipe);
            }
        }
    }

    Log(&serverCtx.csLog, _T("[ARBITRO] Loop principal de aceitação de conexões terminado."));
    if (hThreads[0] != NULL) { WaitForSingleObject(hThreads[0], INFINITE); CloseHandle(hThreads[0]); }
    if (hThreads[1] != NULL) { WaitForSingleObject(hThreads[1], INFINITE); CloseHandle(hThreads[1]); }

    EncerrarServidor(&serverCtx);
    Log(&serverCtx.csLog, _T("[ARBITRO] Servidor encerrado."));
    DeleteCriticalSection(&serverCtx.csLog);
    return 0;
}

// ==========================================================================================
// INICIALIZAÇÃO E ENCERRAMENTO DO SERVIDOR
// ==========================================================================================
BOOL InicializarServidor(SERVER_CONTEXT* ctx) {
    srand((unsigned)time(NULL));

    // csLog já deve estar inicializada em _tmain ANTES desta função ser chamada.
    // ConfigurarValoresRegistry e CarregarDicionarioServidor usarão Log, por isso csLog deve estar pronta.

    ConfigurarValoresRegistry(ctx);
    Log(&ctx->csLog, _T("[INIT] Configurações: MAXLETRAS=%d, RITMO=%ds"), ctx->maxLetrasConfig, ctx->ritmoConfigSegundos);

    if (!CarregarDicionarioServidor(ctx, _T("D:\\SO2\\TP_SO2\\TP\\Comum\\dicionario.txt"))) {
        LogError(&ctx->csLog, _T("[INIT] Falha ao carregar dicionário."));
        return FALSE;
    }

    if (!InicializarMemoriaPartilhadaArbitro(ctx, ctx->maxLetrasConfig)) {
        LogError(&ctx->csLog, _T("[INIT] Falha ao inicializar memória partilhada."));
        LiberarDicionarioServidor(ctx);
        return FALSE;
    }

    InitializeCriticalSection(&ctx->csListaJogadores);

    ZeroMemory(ctx->listaJogadores, sizeof(ctx->listaJogadores));
    ctx->totalJogadoresAtivos = 0;
    ctx->servidorEmExecucao = TRUE;
    ctx->jogoRealmenteAtivo = FALSE;

    return TRUE;
}

void EncerrarServidor(SERVER_CONTEXT* ctx) {
    Log(&ctx->csLog, _T("[ENCERRAR] Iniciando encerramento do servidor..."));

    if (ctx->servidorEmExecucao) {
        ctx->servidorEmExecucao = FALSE;

        MESSAGE msgShutdown; ZeroMemory(&msgShutdown, sizeof(MESSAGE));
        StringCchCopy(msgShutdown.type, _countof(msgShutdown.type), _T("SHUTDOWN"));
        StringCchCopy(msgShutdown.username, _countof(msgShutdown.username), _T("Arbitro"));
        StringCchCopy(msgShutdown.data, _countof(msgShutdown.data), _T("O servidor está a encerrar."));
        NotificarTodosOsJogadores(ctx, &msgShutdown, NULL);

        if (ctx->hEventoShmUpdate) SetEvent(ctx->hEventoShmUpdate);

        // Try to connect to self to unblock the main ConnectNamedPipe loop
        HANDLE hSelfConnect = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hSelfConnect != INVALID_HANDLE_VALUE) {
            CloseHandle(hSelfConnect);
        }
    }

    Log(&ctx->csLog, _T("[ENCERRAR] Aguardando um momento para as threads de cliente... (1s)"));
    Sleep(1000); // Give client threads a moment to process shutdown

    if (ctx->csListaJogadores.DebugInfo != NULL) { // Check if initialized
        EnterCriticalSection(&ctx->csListaJogadores);
        for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
            if (ctx->listaJogadores[i].hPipe != INVALID_HANDLE_VALUE) {
                LogWarning(&ctx->csLog, _T("[ENCERRAR] Pipe do jogador %s (idx %lu) ainda aberto. Fechando."), ctx->listaJogadores[i].username, i);
                CloseHandle(ctx->listaJogadores[i].hPipe);
                ctx->listaJogadores[i].hPipe = INVALID_HANDLE_VALUE;
            }
            ctx->listaJogadores[i].ativo = FALSE;
        }
        ctx->totalJogadoresAtivos = 0;
        LeaveCriticalSection(&ctx->csListaJogadores);
        DeleteCriticalSection(&ctx->csListaJogadores);
    }
    else {
        LogWarning(&ctx->csLog, _T("[ENCERRAR] Critical section da lista de jogadores não inicializada ou já deletada."));
    }


    LimparMemoriaPartilhadaArbitro(ctx);
    LiberarDicionarioServidor(ctx);

    Log(&ctx->csLog, _T("[ENCERRAR] Recursos principais do servidor libertados."));
    // csLog é deletada em _tmain
}


// ==========================================================================================
// Funções de Log (agora recebem csLog explicitamente)
// ==========================================================================================
void Log(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...) {
    // csLogParam DEVE ser inicializado pelo chamador antes da primeira chamada.
    if (csLogParam == NULL || csLogParam->DebugInfo == NULL) { // Fallback se csLogParam não estiver pronto/válido
        TCHAR fallbackBuffer[1024];
        va_list fbArgs;
        va_start(fbArgs, format);
        StringCchVPrintf(fallbackBuffer, _countof(fallbackBuffer), format, fbArgs);
        va_end(fbArgs);
        _tprintf(_T("[LOG-NO_CS_PARAM] %s\n"), fallbackBuffer);
        fflush(stdout);
        return;
    }

    EnterCriticalSection(csLogParam);
    TCHAR buffer[2048];
    va_list args;
    va_start(args, format);
    SYSTEMTIME st;
    GetLocalTime(&st);
    size_t prefixLen = 0;

    StringCchPrintf(buffer, _countof(buffer), _T("%02d:%02d:%02d.%03d "),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    (void)StringCchLength(buffer, _countof(buffer), &prefixLen);

    if (prefixLen < _countof(buffer) - 1) { // Check if there's space for the message itself
        StringCchVPrintf(buffer + prefixLen, _countof(buffer) - prefixLen, format, args);
    }
    // No need to manually add newline if StringCchVPrintf already handles it or if format string contains it
    // but to be safe, ensure a newline if not already ending with one and space permits
    // For now, the original StringCchCat is kept.
    StringCchCat(buffer, _countof(buffer), _T("\n")); // Ensure it ends with a newline

    _tprintf_s(buffer); // Use _tprintf_s for safety if available/intended
    fflush(stdout); // Ensure output is flushed, especially if redirected
    va_end(args);
    LeaveCriticalSection(csLogParam);
}

void LogError(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    Log(csLogParam, _T("[ERRO] %s"), buffer); // Passa csLogParam para Log
}

void LogWarning(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    Log(csLogParam, _T("[AVISO] %s"), buffer); // Passa csLogParam para Log
}

// ==========================================================================================
// Implementações das Funções Auxiliares e Threads
// (Todas as funções que precisam de log agora usam ctx->csLog)
// ==========================================================================================

void ConfigurarValoresRegistry(SERVER_CONTEXT* ctx) {
    HKEY hKey;
    DWORD dwValor;
    DWORD dwSize = sizeof(DWORD);
    LONG lResult;

    ctx->maxLetrasConfig = DEFAULT_MAXLETRAS;
    ctx->ritmoConfigSegundos = DEFAULT_RITMO_SEGUNDOS;

    lResult = RegOpenKeyEx(HKEY_CURRENT_USER, REGISTRY_PATH_TP, 0, KEY_READ | KEY_WRITE, &hKey);
    if (lResult == ERROR_SUCCESS) {
        if (RegQueryValueEx(hKey, REG_MAXLETRAS_NOME, NULL, NULL, (LPBYTE)&dwValor, &dwSize) == ERROR_SUCCESS) {
            if (dwValor > 0 && dwValor <= MAX_LETRAS_TABULEIRO) {
                ctx->maxLetrasConfig = (int)dwValor;
            }
            else {
                LogWarning(&ctx->csLog, _T("[REG] MAXLETRAS (%lu) inválido. Usando %d e atualizando registry."), dwValor, MAX_LETRAS_TABULEIRO);
                ctx->maxLetrasConfig = MAX_LETRAS_TABULEIRO; // Default to max allowed
                RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)&ctx->maxLetrasConfig, sizeof(DWORD));
            }
        }
        else {
            LogWarning(&ctx->csLog, _T("[REG] Não leu MAXLETRAS. Usando padrão %d e criando/atualizando."), ctx->maxLetrasConfig);
            RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)&ctx->maxLetrasConfig, sizeof(DWORD));
        }
        dwSize = sizeof(DWORD); // Reset size for next query
        if (RegQueryValueEx(hKey, REG_RITMO_NOME, NULL, NULL, (LPBYTE)&dwValor, &dwSize) == ERROR_SUCCESS) {
            if (dwValor > 0 && dwValor < 300) { // Some reasonable upper limit for rhythm
                ctx->ritmoConfigSegundos = (int)dwValor;
            }
            else {
                LogWarning(&ctx->csLog, _T("[REG] RITMO (%lu) inválido. Usando padrão %d e atualizando registry."), dwValor, DEFAULT_RITMO_SEGUNDOS);
                RegSetValueEx(hKey, REG_RITMO_NOME, 0, REG_DWORD, (const BYTE*)&ctx->ritmoConfigSegundos, sizeof(DWORD));
            }
        }
        else {
            LogWarning(&ctx->csLog, _T("[REG] Não leu RITMO. Usando padrão %d e criando/atualizando."), ctx->ritmoConfigSegundos);
            RegSetValueEx(hKey, REG_RITMO_NOME, 0, REG_DWORD, (const BYTE*)&ctx->ritmoConfigSegundos, sizeof(DWORD));
        }
        RegCloseKey(hKey);
    }
    else {
        Log(&ctx->csLog, _T("[REG] Chave '%s' não encontrada/acessível. Criando com valores padrão."), REGISTRY_PATH_TP);
        if (RegCreateKeyEx(HKEY_CURRENT_USER, REGISTRY_PATH_TP, 0, NULL, REG_OPTION_NON_VOLATILE,
            KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
            RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)&ctx->maxLetrasConfig, sizeof(DWORD));
            RegSetValueEx(hKey, REG_RITMO_NOME, 0, REG_DWORD, (const BYTE*)&ctx->ritmoConfigSegundos, sizeof(DWORD));
            RegCloseKey(hKey);
            Log(&ctx->csLog, _T("[REG] Chave e valores padrão criados."));
        }
        else {
            LogError(&ctx->csLog, _T("[REG] Falha ao criar chave do Registry '%s': %lu"), REGISTRY_PATH_TP, GetLastError());
        }
    }
    // Final validation
    if (ctx->maxLetrasConfig > MAX_LETRAS_TABULEIRO) ctx->maxLetrasConfig = MAX_LETRAS_TABULEIRO;
    if (ctx->maxLetrasConfig <= 0) ctx->maxLetrasConfig = DEFAULT_MAXLETRAS;
    if (ctx->ritmoConfigSegundos <= 0) ctx->ritmoConfigSegundos = DEFAULT_RITMO_SEGUNDOS;
}


BOOL CarregarDicionarioServidor(SERVER_CONTEXT* ctx, const TCHAR* nomeArquivo) {
    DICIONARIO_ARBITRO* dict = &ctx->dicionario;

    InitializeCriticalSection(&dict->csDicionario);
    dict->palavras = NULL;
    dict->totalPalavras = 0;
    FILE* arquivo;

    if (_tfopen_s(&arquivo, nomeArquivo, _T("r, ccs=UTF-8")) != 0 || !arquivo) {
        LogError(&ctx->csLog, _T("[DIC] Erro ao abrir ficheiro de dicionário '%s'."), nomeArquivo);
        DeleteCriticalSection(&dict->csDicionario); // Clean up CS if dictionary load fails early
        return FALSE;
    }

    TCHAR linha[MAX_WORD + 2]; // +2 for \n and \0 or \r\n and \0
    DWORD capacidade = 200; // Initial capacity
    dict->palavras = (TCHAR**)malloc(capacidade * sizeof(TCHAR*));
    if (!dict->palavras) {
        fclose(arquivo);
        LogError(&ctx->csLog, _T("[DIC] Falha ao alocar memória inicial para dicionário."));
        DeleteCriticalSection(&dict->csDicionario);
        return FALSE;
    }

    while (_fgetts(linha, MAX_WORD + 1, arquivo)) { // Read MAX_WORD characters + null terminator
        size_t len = _tcslen(linha);
        // Remove newline characters
        while (len > 0 && (linha[len - 1] == _T('\n') || linha[len - 1] == _T('\r'))) {
            linha[len - 1] = _T('\0');
            len--;
        }

        if (len == 0) continue; // Skip empty lines

        if (dict->totalPalavras >= capacidade) {
            capacidade *= 2;
            TCHAR** temp = (TCHAR**)realloc(dict->palavras, capacidade * sizeof(TCHAR*));
            if (!temp) {
                LogError(&ctx->csLog, _T("[DIC] Falha ao realocar memória para dicionário."));
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
            LogError(&ctx->csLog, _T("[DIC] Falha ao alocar memória para a palavra '%s'."), linha);
            // Free already allocated words before failing
            for (DWORD i = 0; i < dict->totalPalavras; i++) free(dict->palavras[i]);
            free(dict->palavras); dict->palavras = NULL;
            fclose(arquivo);
            DeleteCriticalSection(&dict->csDicionario);
            return FALSE;
        }
        // Convert word to uppercase
        for (TCHAR* p = dict->palavras[dict->totalPalavras]; *p; ++p) *p = _totupper(*p);

        dict->totalPalavras++;
    }

    fclose(arquivo);
    Log(&ctx->csLog, _T("[DIC] Dicionário '%s' carregado com %lu palavras."), nomeArquivo, dict->totalPalavras);
    if (dict->totalPalavras == 0) {
        LogWarning(&ctx->csLog, _T("[DIC] Dicionário carregado está vazio!"));
        // It might be acceptable for the server to run with an empty dictionary,
        // but players won't be able to score. Consider if this should be a fatal error.
    }
    return TRUE;
}


void LiberarDicionarioServidor(SERVER_CONTEXT* ctx) {
    DICIONARIO_ARBITRO* dict = &ctx->dicionario;
    if (dict->csDicionario.DebugInfo != NULL) { // Check if initialized
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


BOOL InicializarMemoriaPartilhadaArbitro(SERVER_CONTEXT* ctx, int maxLetras) {
    if (maxLetras <= 0 || maxLetras > MAX_LETRAS_TABULEIRO) {
        LogError(&ctx->csLog, _T("[SHM] maxLetras inválido (%d) para inicializar memória partilhada."), maxLetras);
        return FALSE;
    }

    ctx->hMapFileShm = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DadosJogoCompartilhados), SHM_NAME);
    if (ctx->hMapFileShm == NULL) {
        LogError(&ctx->csLog, _T("[SHM] Erro ao criar FileMapping (%s): %lu"), SHM_NAME, GetLastError());
        return FALSE;
    }

    ctx->pDadosShm = (DadosJogoCompartilhados*)MapViewOfFile(ctx->hMapFileShm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DadosJogoCompartilhados));
    if (ctx->pDadosShm == NULL) {
        LogError(&ctx->csLog, _T("[SHM] Erro ao mapear SHM (%s): %lu"), SHM_NAME, GetLastError());
        CloseHandle(ctx->hMapFileShm); ctx->hMapFileShm = NULL;
        return FALSE;
    }

    ctx->hEventoShmUpdate = CreateEvent(NULL, TRUE, FALSE, EVENT_SHM_UPDATE); // Manual reset, initially non-signaled
    if (ctx->hEventoShmUpdate == NULL) {
        LogError(&ctx->csLog, _T("[SHM] Erro ao criar evento SHM (%s): %lu"), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(ctx->pDadosShm); ctx->pDadosShm = NULL;
        CloseHandle(ctx->hMapFileShm); ctx->hMapFileShm = NULL;
        return FALSE;
    }

    ctx->hMutexShm = CreateMutex(NULL, FALSE, MUTEX_SHARED_MEM); // Initially not owned
    if (ctx->hMutexShm == NULL) {
        LogError(&ctx->csLog, _T("[SHM] Erro ao criar mutex SHM (%s): %lu"), MUTEX_SHARED_MEM, GetLastError());
        CloseHandle(ctx->hEventoShmUpdate); ctx->hEventoShmUpdate = NULL;
        UnmapViewOfFile(ctx->pDadosShm); ctx->pDadosShm = NULL;
        CloseHandle(ctx->hMapFileShm); ctx->hMapFileShm = NULL;
        return FALSE;
    }

    // Initialize shared memory data
    WaitForSingleObject(ctx->hMutexShm, INFINITE);
    ctx->pDadosShm->numMaxLetrasAtual = maxLetras;
    for (int i = 0; i < MAX_LETRAS_TABULEIRO; i++) {
        ctx->pDadosShm->letrasVisiveis[i] = (i < maxLetras) ? _T('_') : _T('\0'); // Initialize visible letters
    }
    StringCchCopy(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
    StringCchCopy(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
    ctx->pDadosShm->pontuacaoUltimaPalavra = 0;
    ctx->pDadosShm->generationCount = 0; // Initial generation
    ctx->pDadosShm->jogoAtivo = FALSE;   // Game is not active until enough players join
    ReleaseMutex(ctx->hMutexShm);
    SetEvent(ctx->hEventoShmUpdate); // Signal that SHM is updated

    Log(&ctx->csLog, _T("[SHM] Memória partilhada '%s' e evento '%s' inicializados."), SHM_NAME, EVENT_SHM_UPDATE);
    return TRUE;
}

void LimparMemoriaPartilhadaArbitro(SERVER_CONTEXT* ctx) {
    if (ctx->pDadosShm != NULL) UnmapViewOfFile(ctx->pDadosShm);
    ctx->pDadosShm = NULL;
    if (ctx->hMapFileShm != NULL) CloseHandle(ctx->hMapFileShm);
    ctx->hMapFileShm = NULL;
    if (ctx->hEventoShmUpdate != NULL) CloseHandle(ctx->hEventoShmUpdate);
    ctx->hEventoShmUpdate = NULL;
    if (ctx->hMutexShm != NULL) CloseHandle(ctx->hMutexShm);
    ctx->hMutexShm = NULL;
    Log(&ctx->csLog, _T("[SHM] Memória partilhada e objetos de sync limpos."));
}


DWORD WINAPI ThreadGestorLetras(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    SERVER_CONTEXT* ctx = args->serverCtx;
    int indiceLetraAntiga = 0; // To cycle through positions if board is full
    Log(&ctx->csLog, _T("[LETRAS] ThreadGestorLetras iniciada."));

    while (ctx->servidorEmExecucao) {
        int ritmoAtualSegundos;
        // Safely read rhythm configuration
        // No need for SHM mutex for ritmoConfigSegundos, it's server-local
        // but if it were in SHM, mutex would be needed.
        // For now, let's assume ritmoConfigSegundos is stable or updated safely elsewhere.
        ritmoAtualSegundos = ctx->ritmoConfigSegundos;


        // Sleep for the configured rhythm, but breakable by server shutdown
        for (int i = 0; i < ritmoAtualSegundos * 10 && ctx->servidorEmExecucao; ++i) { // Sleep in 100ms chunks
            Sleep(100);
        }
        if (!ctx->servidorEmExecucao) break;

        if (!ctx->jogoRealmenteAtivo) { // Only generate letters if the game is active
            continue;
        }

        WaitForSingleObject(ctx->hMutexShm, INFINITE);
        if (ctx->pDadosShm) { // Ensure SHM pointer is valid
            int maxLetras = ctx->pDadosShm->numMaxLetrasAtual;
            int posParaNovaLetra = -1;
            int letrasAtuaisNoTabuleiro = 0;

            // Count current letters and find first empty slot
            for (int i = 0; i < maxLetras; i++) {
                if (ctx->pDadosShm->letrasVisiveis[i] != _T('_')) {
                    letrasAtuaisNoTabuleiro++;
                }
                else if (posParaNovaLetra == -1) { // Found an empty slot
                    posParaNovaLetra = i;
                }
            }

            // If board is full, overwrite the oldest letter
            if (posParaNovaLetra == -1 && letrasAtuaisNoTabuleiro >= maxLetras && maxLetras > 0) {
                posParaNovaLetra = indiceLetraAntiga;
                indiceLetraAntiga = (indiceLetraAntiga + 1) % maxLetras;
            }
            // This case should ideally be covered by the first loop if posParaNovaLetra remains -1 and board isn't full
            else if (posParaNovaLetra == -1 && letrasAtuaisNoTabuleiro < maxLetras && maxLetras > 0) {
                // This should ideally not happen if logic above is correct, but as a fallback:
                for (int i = 0; i < maxLetras; ++i) { if (ctx->pDadosShm->letrasVisiveis[i] == _T('_')) { posParaNovaLetra = i; break; } }
                if (posParaNovaLetra == -1 && maxLetras > 0) posParaNovaLetra = 0; // Fallback to first if still not found (unlikely)
            }


            if (posParaNovaLetra != -1 && maxLetras > 0) { // If a position was found
                TCHAR novaLetra = _T('A') + (rand() % 26); // Generate a random uppercase letter
                ctx->pDadosShm->letrasVisiveis[posParaNovaLetra] = novaLetra;
                ctx->pDadosShm->generationCount++; // Increment generation count
                SetEvent(ctx->hEventoShmUpdate);   // Signal clients that SHM has been updated
                // Log(&ctx->csLog, _T("[LETRAS] Nova letra '%c' na pos %d. Gen: %ld"), novaLetra, posParaNovaLetra, ctx->pDadosShm->generationCount);
            }
        }
        ReleaseMutex(ctx->hMutexShm);
    }
    Log(&ctx->csLog, _T("[LETRAS] ThreadGestorLetras a terminar."));
    return 0;
}

DWORD WINAPI ThreadAdminArbitro(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    SERVER_CONTEXT* ctx = args->serverCtx;
    TCHAR comando[100];
    TCHAR usernameParam[MAX_USERNAME];
    Log(&ctx->csLog, _T("[ADMIN] ThreadAdminArbitro iniciada. Digite 'ajuda' para lista de comandos."));

    while (ctx->servidorEmExecucao) {
        Log(&ctx->csLog, _T("Admin> "));
        if (_fgetts(comando, _countof(comando), stdin) == NULL) {
            if (ctx->servidorEmExecucao) { // Check if still running to avoid logs during normal shutdown
                if (feof(stdin)) LogWarning(&ctx->csLog, _T("[ADMIN] EOF no stdin. Encerrando thread admin."));
                else LogError(&ctx->csLog, _T("[ADMIN] Erro ao ler comando do admin."));
            }
            break; // Exit if input fails (e.g. EOF or error)
        }
        comando[_tcscspn(comando, _T("\r\n"))] = _T('\0'); // Remove trailing newline

        if (_tcslen(comando) == 0) continue; // Skip empty commands

        if (_tcscmp(comando, _T("listar")) == 0) {
            Log(&ctx->csLog, _T("[ADMIN] Comando: listar"));
            EnterCriticalSection(&ctx->csListaJogadores);
            Log(&ctx->csLog, _T("--- Lista de Jogadores Ativos (%lu) ---"), ctx->totalJogadoresAtivos);
            BOOL encontrouAlgum = FALSE;
            for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
                if (ctx->listaJogadores[i].ativo) {
                    Log(&ctx->csLog, _T("  - %s (Pontos: %.1f, Pipe: %p)"), ctx->listaJogadores[i].username, ctx->listaJogadores[i].pontos, ctx->listaJogadores[i].hPipe);
                    encontrouAlgum = TRUE;
                }
            }
            if (!encontrouAlgum) Log(&ctx->csLog, _T("  (Nenhum jogador ativo no momento)"));
            Log(&ctx->csLog, _T("------------------------------------"));
            LeaveCriticalSection(&ctx->csListaJogadores);
        }
        // ***** START OF NEW COMMAND: iniciarbot *****
        else if (_tcsnicmp(comando, _T("iniciarbot"), 10) == 0) {
            TCHAR botUsername[MAX_USERNAME];
            TCHAR botReactionTimeStr[10];
            int botReactionTime;

            Log(&ctx->csLog, _T("[ADMIN] Comando: iniciarbot"));

            // Prompt for Bot Username
            Log(&ctx->csLog, _T("Admin> Digite o nome para o novo bot: "));
            fflush(stdout); // Ensure prompt is displayed before input
            if (_fgetts(botUsername, _countof(botUsername), stdin) == NULL) {
                LogWarning(&ctx->csLog, _T("[ADMIN] Erro ao ler nome do bot ou entrada vazia."));
                continue;
            }
            botUsername[_tcscspn(botUsername, _T("\r\n"))] = _T('\0');

            if (_tcslen(botUsername) == 0) {
                LogWarning(&ctx->csLog, _T("[ADMIN] Nome do bot não pode ser vazio."));
                continue;
            }
            if (_tcslen(botUsername) >= MAX_USERNAME) {
                LogWarning(&ctx->csLog, _T("[ADMIN] Nome do bot é muito longo. Máximo %d caracteres."), MAX_USERNAME - 1);
                continue;
            }


            EnterCriticalSection(&ctx->csListaJogadores);
            if (EncontrarJogador(ctx, botUsername) != -1) { // EncontrarJogador needs csListaJogadores held
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[ADMIN] Username '%s' já está em uso."), botUsername);
                continue;
            }
            LeaveCriticalSection(&ctx->csListaJogadores);

            // Prompt for Bot Reaction Time
            Log(&ctx->csLog, _T("Admin> Digite o tempo de reação para o bot '%s' (segundos, [5,30]): "), botUsername);
            fflush(stdout);
            if (_fgetts(botReactionTimeStr, _countof(botReactionTimeStr), stdin) == NULL) {
                LogWarning(&ctx->csLog, _T("[ADMIN] Erro ao ler tempo de reação do bot ou entrada vazia."));
                continue;
            }
            botReactionTimeStr[_tcscspn(botReactionTimeStr, _T("\r\n"))] = _T('\0');
            botReactionTime = _tstoi(botReactionTimeStr);

            if (botReactionTime < 5 || botReactionTime > 30) {
                LogWarning(&ctx->csLog, _T("[ADMIN] Tempo de reação inválido: %d. Deve ser entre 5 e 30."), botReactionTime);
                continue;
            }

            STARTUPINFO si;
            PROCESS_INFORMATION pi;
            TCHAR commandLine[MAX_PATH + MAX_USERNAME + 20]; // Ample space for "bot.exe username time"

            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));


            StringCchPrintf(commandLine, _countof(commandLine), _T("bot.exe %s %d"), botUsername, botReactionTime);

            Log(&ctx->csLog, _T("[ADMIN] Tentando iniciar bot com comando: %s"), commandLine);

            if (!CreateProcess(
                NULL,           // Application name (use NULL if commandLine includes it)
                commandLine,    // Command line to execute
                NULL,           // Process security attributes
                NULL,           // Thread security attributes
                FALSE,          // Inherit handles
                CREATE_NEW_CONSOLE, // Creation flags (each bot gets its own console)
                NULL,           // Environment block (inherit from parent)
                NULL,           // Current directory (inherit from parent)
                &si,            // Pointer to STARTUPINFO structure
                &pi)            // Pointer to PROCESS_INFORMATION structure
                ) {
                LogError(&ctx->csLog, _T("[ADMIN] Falha ao criar processo do bot (%s): %lu. Verifique se bot.exe está no PATH ou no mesmo diretório."), botUsername, GetLastError());
            }
            else {
                Log(&ctx->csLog, _T("[ADMIN] Processo do bot '%s' iniciado com PID %lu. O bot deverá conectar-se em breve."), botUsername, pi.dwProcessId);
                CloseHandle(pi.hProcess); // Close handles as we don't wait for the bot.
                CloseHandle(pi.hThread);  // The bot will run independently.
            }
        }
        // ***** END OF NEW COMMAND: iniciarbot *****
        else if (_tcsncmp(comando, _T("excluir "), 8) == 0) {
            ZeroMemory(usernameParam, sizeof(usernameParam)); // Clear buffer
            // Use _stscanf_s for parsing the command string itself
            if (_stscanf_s(comando, _T("excluir %31s"), usernameParam, (unsigned)_countof(usernameParam) - 1) == 1) {
                usernameParam[MAX_USERNAME - 1] = _T('\0'); // Ensure null termination
                Log(&ctx->csLog, _T("[ADMIN] Comando: excluir %s"), usernameParam);
                RemoverJogador(ctx, usernameParam, TRUE); // TRUE to notify client to exit
            }
            else {
                LogWarning(&ctx->csLog, _T("[ADMIN] Comando excluir mal formatado. Uso: excluir <username>"));
            }
        }
        else if (_tcscmp(comando, _T("acelerar")) == 0) {
            // This modifies a server-local config, not directly SHM data related to game state
            // If ritmoConfigSegundos was in SHM, it would need ctx->hMutexShm
            if (ctx->ritmoConfigSegundos > 1) {
                (ctx->ritmoConfigSegundos)--;
                Log(&ctx->csLog, _T("[ADMIN] Ritmo alterado para %d segundos."), ctx->ritmoConfigSegundos);
            }
            else {
                Log(&ctx->csLog, _T("[ADMIN] Ritmo já está no mínimo (1 segundo)."));
            }
        }
        else if (_tcscmp(comando, _T("travar")) == 0) {
            (ctx->ritmoConfigSegundos)++;
            Log(&ctx->csLog, _T("[ADMIN] Ritmo alterado para %d segundos."), ctx->ritmoConfigSegundos);
        }
        else if (_tcscmp(comando, _T("encerrar")) == 0) {
            Log(&ctx->csLog, _T("[ADMIN] Comando: encerrar. Servidor a terminar..."));
            ctx->servidorEmExecucao = FALSE; // Signal all loops to stop

            if (ctx->hEventoShmUpdate) SetEvent(ctx->hEventoShmUpdate); // Wake up SHM waiters

            // Attempt to connect to self to unblock the main pipe listening loop
            // This helps the CreateNamedPipe -> ConnectNamedPipe sequence in _tmain to break
            HANDLE hSelfConnect = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hSelfConnect != INVALID_HANDLE_VALUE) {
                CloseHandle(hSelfConnect);
            }
            // The main loop in _tmain will detect servidorEmExecucao = FALSE and exit.
            break; // Exit admin command loop
        }
        else if (_tcscmp(comando, _T("ajuda")) == 0) {
            Log(&ctx->csLog, _T("[ADMIN] Comandos disponíveis:"));
            Log(&ctx->csLog, _T("  listar        - Lista todos os jogadores ativos."));
            Log(&ctx->csLog, _T("  iniciarbot    - Inicia uma nova instância de bot (pede nome e tempo de reação)."));
            Log(&ctx->csLog, _T("  excluir <user>- Remove o jogador <user> do jogo."));
            Log(&ctx->csLog, _T("  acelerar      - Diminui o intervalo de geração de letras (mais rápido)."));
            Log(&ctx->csLog, _T("  travar        - Aumenta o intervalo de geração de letras (mais lento)."));
            Log(&ctx->csLog, _T("  encerrar      - Termina o servidor do árbitro."));
            Log(&ctx->csLog, _T("  ajuda         - Mostra esta lista de comandos."));
        }
        else {
            LogWarning(&ctx->csLog, _T("[ADMIN] Comando desconhecido: '%s'. Digite 'ajuda' para lista."), comando);
        }
    }
    Log(&ctx->csLog, _T("[ADMIN] ThreadAdminArbitro a terminar."));
    return 0;
}

DWORD WINAPI ThreadClienteConectado(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    SERVER_CONTEXT* ctx = args->serverCtx;
    HANDLE hPipe = args->hPipeCliente;

    MESSAGE msg;
    DWORD bytesLidos, bytesEscritos;
    JOGADOR_INFO_ARBITRO* meuJogadorInfo = NULL; // Pointer to this client's entry in server's player list
    TCHAR usernameEsteCliente[MAX_USERNAME];
    usernameEsteCliente[0] = _T('\0');
    BOOL esteClienteAtivoNaThread = FALSE; // Local flag for thread's view of client state
    int meuIndiceNaLista = -1;

    Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Nova thread para cliente iniciada."), hPipe);

    OVERLAPPED olRead; ZeroMemory(&olRead, sizeof(OVERLAPPED));
    olRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Manual-reset, initially non-signaled
    if (olRead.hEvent == NULL) {
        LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Falha ao criar evento de leitura."), hPipe);
        // Pipe handle hPipe will be closed in cleanup_cliente_thread if it reaches there
        // Or else the caller of CreateThread should handle it if this thread fails very early.
        // Here, we go to cleanup.
        goto cleanup_cliente_thread; // Ensure args is freed if allocated
    }

    // Initial JOIN message read
    ResetEvent(olRead.hEvent);
    if (ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesLidos, &olRead) || GetLastError() == ERROR_IO_PENDING) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(olRead.hEvent, IO_TIMEOUT) != WAIT_OBJECT_0) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Timeout/Erro (%lu) ReadFile JOIN."), hPipe, GetLastError());
                goto cleanup_cliente_thread;
            }
            if (!GetOverlappedResult(hPipe, &olRead, &bytesLidos, FALSE)) { // FALSE = do not wait
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] GetOverlappedResult JOIN falhou (%lu)."), hPipe, GetLastError());
                goto cleanup_cliente_thread;
            }
        }
        // If ReadFile completed synchronously, GOR is still needed to get final status for overlapped I/O.
        // However, if it completed synchronously and successfully, bytesLidos is already set.
        // For simplicity, GetOverlappedResult is often called anyway if olRead was used.
        // If ReadFile completed synchronously without error, GetOverlappedResult should also succeed immediately.

        if (bytesLidos == sizeof(MESSAGE) && _tcscmp(msg.type, _T("JOIN")) == 0) {
            StringCchCopy(usernameEsteCliente, MAX_USERNAME, msg.username);
            Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Recebido JOIN de '%s'."), hPipe, usernameEsteCliente);

            EnterCriticalSection(&ctx->csListaJogadores);
            int idxExistente = EncontrarJogador(ctx, usernameEsteCliente); // Needs csListaJogadores
            if (idxExistente != -1) {
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Username '%s' já em uso."), hPipe, usernameEsteCliente);
                MESSAGE resp; ZeroMemory(&resp, sizeof(MESSAGE)); StringCchCopy(resp.type, _countof(resp.type), _T("JOIN_USER_EXISTS")); StringCchCopy(resp.data, _countof(resp.data), _T("Username em uso."));
                WriteFile(hPipe, &resp, sizeof(MESSAGE), &bytesEscritos, NULL); // Response can be synchronous for simplicity here
                goto cleanup_cliente_thread;
            }
            if (ctx->totalJogadoresAtivos >= MAX_JOGADORES) {
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Jogo cheio. Rejeitando '%s'."), hPipe, usernameEsteCliente);
                MESSAGE resp; ZeroMemory(&resp, sizeof(MESSAGE)); StringCchCopy(resp.type, _countof(resp.type), _T("JOIN_GAME_FULL")); StringCchCopy(resp.data, _countof(resp.data), _T("Jogo cheio."));
                WriteFile(hPipe, &resp, sizeof(MESSAGE), &bytesEscritos, NULL);
                goto cleanup_cliente_thread;
            }

            // Find an empty slot for the new player
            for (int i = 0; i < MAX_JOGADORES; ++i) {
                if (!ctx->listaJogadores[i].ativo) {
                    meuIndiceNaLista = i;
                    meuJogadorInfo = &ctx->listaJogadores[i]; // Get pointer to the slot

                    StringCchCopy(meuJogadorInfo->username, MAX_USERNAME, usernameEsteCliente);
                    meuJogadorInfo->pontos = 0.0f;
                    meuJogadorInfo->hPipe = hPipe; // Store this client's pipe
                    meuJogadorInfo->ativo = TRUE;
                    meuJogadorInfo->dwThreadIdCliente = GetCurrentThreadId();
                    ctx->totalJogadoresAtivos++;
                    esteClienteAtivoNaThread = TRUE; // Mark this thread as handling an active client

                    Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Jogador '%s' adicionado (idx %d). Total: %lu"), hPipe, usernameEsteCliente, meuIndiceNaLista, ctx->totalJogadoresAtivos);

                    MESSAGE respOK; ZeroMemory(&respOK, sizeof(MESSAGE)); StringCchCopy(respOK.type, _countof(respOK.type), _T("JOIN_OK"));
                    StringCchPrintf(respOK.data, _countof(respOK.data), _T("Bem-vindo, %s!"), usernameEsteCliente);
                    WriteFile(hPipe, &respOK, sizeof(MESSAGE), &bytesEscritos, NULL); // Send JOIN_OK

                    MESSAGE notifJoin; ZeroMemory(&notifJoin, sizeof(MESSAGE)); StringCchCopy(notifJoin.type, _countof(notifJoin.type), _T("GAME_UPDATE"));
                    StringCchPrintf(notifJoin.data, _countof(notifJoin.data), _T("Jogador %s entrou no jogo."), usernameEsteCliente);
                    NotificarTodosOsJogadores(ctx, &notifJoin, usernameEsteCliente); // Notify others (except self)

                    VerificarEstadoJogo(ctx); // Check if game should start/stop
                    break; // Found slot, break from loop
                }
            }
            LeaveCriticalSection(&ctx->csListaJogadores);
            if (!esteClienteAtivoNaThread) { // Should not happen if MAX_JOGADORES check was correct
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Falha ao adicionar '%s' (sem slot disponível?)."), hPipe, usernameEsteCliente);
                goto cleanup_cliente_thread;
            }
        }
        else {
            LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Primeira msg não foi JOIN ou bytes errados (%lu). Desconectando."), hPipe, bytesLidos);
            goto cleanup_cliente_thread;
        }
    }
    else { // ReadFile failed immediately (not ERROR_IO_PENDING)
        LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Falha ao ler 1ª msg (JOIN): %lu."), hPipe, GetLastError());
        goto cleanup_cliente_thread;
    }

    // Main message loop for this client
    while (ctx->servidorEmExecucao && esteClienteAtivoNaThread) {
        ResetEvent(olRead.hEvent);
        BOOL sucessoLeitura = ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesLidos, &olRead);
        DWORD dwErrorRead = GetLastError();

        if (!sucessoLeitura && dwErrorRead == ERROR_IO_PENDING) {
            // Wait for read operation or server shutdown signal (not implemented here, simple timeout)
            HANDLE handles[1] = { olRead.hEvent };
            // Consider adding an event that signals server shutdown to make this wait more responsive
            DWORD waitRes = WaitForMultipleObjects(1, handles, FALSE, READ_TIMEOUT_THREAD_JOGADOR);

            if (!ctx->servidorEmExecucao) break; // Server shutting down

            if (waitRes == WAIT_TIMEOUT) {
                // Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Timeout ReadFile para '%s'. Verificando pipe..."), hPipe, usernameEsteCliente);
                // PeekNamedPipe to check if client is still connected or pipe is broken.
                // If pipe is fine, just continue to try reading again.
                // If client disconnected, PeekNamedPipe might show an error.
                DWORD pkBytesRead, pkTotalBytesAvail, pkBytesLeftThisMessage;
                if (!PeekNamedPipe(hPipe, NULL, 0, &pkBytesRead, &pkTotalBytesAvail, &pkBytesLeftThisMessage)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) {
                        LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Pipe para '%s' quebrou durante timeout. %lu"), hPipe, usernameEsteCliente, GetLastError());
                        esteClienteAtivoNaThread = FALSE; break;
                    }
                }
                continue; // No data yet, or server not shutting down, loop back to ReadFile
            }
            else if (waitRes != WAIT_OBJECT_0) { // Some other error
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Erro %lu ao esperar ReadFile para '%s'."), hPipe, GetLastError(), usernameEsteCliente);
                esteClienteAtivoNaThread = FALSE; break;
            }
            // Read completed (event signaled)
            if (!GetOverlappedResult(hPipe, &olRead, &bytesLidos, FALSE)) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] GOR falhou após ReadFile para '%s': %lu."), hPipe, usernameEsteCliente, GetLastError());
                esteClienteAtivoNaThread = FALSE; break;
            }
            sucessoLeitura = TRUE; // Mark as successful for processing
        }
        else if (!sucessoLeitura) { // ReadFile failed immediately, not pending
            if (ctx->servidorEmExecucao) LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] ReadFile falhou para '%s': %lu."), hPipe, usernameEsteCliente, dwErrorRead);
            esteClienteAtivoNaThread = FALSE; break;
        }

        if (!ctx->servidorEmExecucao) break; // Check again after potential blocking calls

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {
            // Verify client's identity and state before processing message
            EnterCriticalSection(&ctx->csListaJogadores);
            // Check if the current client (identified by hPipe and its stored index) is still valid
            if (meuIndiceNaLista == -1 || meuIndiceNaLista >= MAX_JOGADORES ||
                !ctx->listaJogadores[meuIndiceNaLista].ativo ||
                ctx->listaJogadores[meuIndiceNaLista].hPipe != hPipe || // Pipe handle mismatch
                _tcscmp(ctx->listaJogadores[meuIndiceNaLista].username, usernameEsteCliente) != 0) { // Username mismatch
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Jogador '%s' tornou-se inválido (idx %d) ou pipe/username desatualizado. Encerrando thread."), hPipe, usernameEsteCliente, meuIndiceNaLista);
                esteClienteAtivoNaThread = FALSE; break;
            }
            // meuJogadorInfo should still point to ctx->listaJogadores[meuIndiceNaLista]
            // No need to re-assign if index and list structure is stable, but can re-validate:
            meuJogadorInfo = &ctx->listaJogadores[meuIndiceNaLista];
            LeaveCriticalSection(&ctx->csListaJogadores);


            if (_tcscmp(msg.type, _T("WORD")) == 0) {
                Log(&ctx->csLog, _T("[CLIENT_THREAD %p] '%s' submeteu: '%s'"), hPipe, usernameEsteCliente, msg.data);
                ValidarPalavraJogo(args, msg.data, usernameEsteCliente); // args has serverCtx and this client's hPipe
            }
            else if (_tcscmp(msg.type, _T("GET_SCORE")) == 0) {
                MESSAGE respScore; ZeroMemory(&respScore, sizeof(MESSAGE));
                StringCchCopy(respScore.type, _countof(respScore.type), _T("SCORE_UPDATE"));
                StringCchCopy(respScore.username, _countof(respScore.username), usernameEsteCliente); // Echo username

                EnterCriticalSection(&ctx->csListaJogadores);
                // Re-verify player by index, ensuring it's still this client
                if (meuJogadorInfo && meuJogadorInfo->ativo && meuJogadorInfo->hPipe == hPipe && _tcscmp(meuJogadorInfo->username, usernameEsteCliente) == 0) {
                    StringCchPrintf(respScore.data, _countof(respScore.data), _T("%.1f"), meuJogadorInfo->pontos);
                    respScore.pontos = (int)meuJogadorInfo->pontos; // Send score as int too if needed by client
                }
                else {
                    StringCchCopy(respScore.data, _countof(respScore.data), _T("Erro/Inativo"));
                    respScore.pontos = 0;
                }
                LeaveCriticalSection(&ctx->csListaJogadores);
                WriteFile(hPipe, &respScore, sizeof(MESSAGE), &bytesEscritos, NULL); // Simple sync write for this
            }
            else if (_tcscmp(msg.type, _T("GET_JOGS")) == 0) {
                MESSAGE respJogs; ZeroMemory(&respJogs, sizeof(MESSAGE));
                StringCchCopy(respJogs.type, _countof(respJogs.type), _T("PLAYER_LIST_UPDATE"));
                StringCchCopy(respJogs.username, _countof(respJogs.username), _T("Arbitro")); // Server provides list

                TCHAR listaBuffer[sizeof(respJogs.data)]; // Use the data field of MESSAGE as buffer
                listaBuffer[0] = _T('\0');
                size_t bufferRestante = _countof(listaBuffer);
                TCHAR* pBufferAtual = listaBuffer;
                HRESULT hr = S_OK;

                hr = StringCchCatEx(pBufferAtual, bufferRestante, _T("Jogadores Ativos:\n"), &pBufferAtual, &bufferRestante, 0);

                EnterCriticalSection(&ctx->csListaJogadores);
                for (DWORD i = 0; i < MAX_JOGADORES && SUCCEEDED(hr); ++i) {
                    if (ctx->listaJogadores[i].ativo) {
                        TCHAR linhaJogador[100];
                        StringCchPrintf(linhaJogador, _countof(linhaJogador), _T(" - %s (%.1f pts)\n"),
                            ctx->listaJogadores[i].username, ctx->listaJogadores[i].pontos);
                        hr = StringCchCatEx(pBufferAtual, bufferRestante, linhaJogador, &pBufferAtual, &bufferRestante, 0);
                    }
                }
                LeaveCriticalSection(&ctx->csListaJogadores);

                if (SUCCEEDED(hr)) {
                    StringCchCopy(respJogs.data, _countof(respJogs.data), listaBuffer);
                }
                else {
                    StringCchCopy(respJogs.data, _countof(respJogs.data), _T("Erro ao gerar lista de jogadores (buffer pequeno?)."));
                }
                WriteFile(hPipe, &respJogs, sizeof(MESSAGE), &bytesEscritos, NULL);
            }
            else if (_tcscmp(msg.type, _T("EXIT")) == 0) {
                Log(&ctx->csLog, _T("[CLIENT_THREAD %p] '%s' solicitou sair."), hPipe, usernameEsteCliente);
                esteClienteAtivoNaThread = FALSE; // Signal loop to terminate
                break; // Exit loop, will proceed to cleanup
            }
            else {
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] '%s' enviou msg desconhecida: '%s'"), hPipe, usernameEsteCliente, msg.type);
                // Optionally send an "UNKNOWN_COMMAND" response
            }
        }
        else if (!sucessoLeitura) { // Should have been caught by earlier checks, but for safety
            esteClienteAtivoNaThread = FALSE; break;
        }
        else if (bytesLidos == 0 && sucessoLeitura) { // Graceful disconnect (EOF)
            if (ctx->servidorEmExecucao) Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Cliente '%s' desconectou (EOF)."), hPipe, usernameEsteCliente);
            esteClienteAtivoNaThread = FALSE; break;
        }
        else if (bytesLidos != 0) { // Partial message or framing error
            if (ctx->servidorEmExecucao) LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Erro de framing para '%s'. Bytes: %lu. Esperado: %zu."), hPipe, usernameEsteCliente, bytesLidos, sizeof(MESSAGE));
            esteClienteAtivoNaThread = FALSE; break;
        }
    } // End of client message loop

cleanup_cliente_thread:
    Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Limpando para '%s'..."), hPipe, usernameEsteCliente[0] ? usernameEsteCliente : _T("(desconhecido/não juntou)"));

    if (usernameEsteCliente[0] != _T('\0') && meuIndiceNaLista != -1) { // If client had successfully joined
        // Use RemoverJogador which handles notifications and game state checks
        RemoverJogador(ctx, usernameEsteCliente, FALSE); // FALSE = do not try to send SHUTDOWN msg, as client is already disconnecting
    }
    // If client never fully joined but slot was partially set up (unlikely with current logic if meuIndiceNaLista used)
    else if (meuIndiceNaLista != -1) {
        EnterCriticalSection(&ctx->csListaJogadores);
        if (meuIndiceNaLista >= 0 && meuIndiceNaLista < MAX_JOGADORES &&
            ctx->listaJogadores[meuIndiceNaLista].hPipe == hPipe) { // Check if it's indeed this thread's pipe
            // This case handles if client thread ends before full registration or if usernameEsteCliente was not set
            if (ctx->listaJogadores[meuIndiceNaLista].ativo) { // If it was marked active
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Limpando jogador parcialmente adicionado (idx %d, user '%s') que não tinha usernameEsteCliente setado na thread."), hPipe, meuIndiceNaLista, ctx->listaJogadores[meuIndiceNaLista].username);
                ctx->listaJogadores[meuIndiceNaLista].ativo = FALSE;
                if (ctx->totalJogadoresAtivos > 0) ctx->totalJogadoresAtivos--;
                VerificarEstadoJogo(ctx); // Update game state if a player drops
            }
            ctx->listaJogadores[meuIndiceNaLista].hPipe = INVALID_HANDLE_VALUE; // Clear pipe from slot
        }
        LeaveCriticalSection(&ctx->csListaJogadores);
    }


    if (olRead.hEvent) CloseHandle(olRead.hEvent);

    if (hPipe != INVALID_HANDLE_VALUE) {
        // Check if this pipe is still associated with any player entry, just in case cleanup logic was bypassed.
        // Usually, RemoverJogador or the block above should have set hPipe to INVALID_HANDLE_VALUE in listaJogadores.
        EnterCriticalSection(&ctx->csListaJogadores);
        for (int i = 0; i < MAX_JOGADORES; ++i) {
            if (ctx->listaJogadores[i].hPipe == hPipe) {
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Pipe ainda estava na lista de jogadores para '%s' no cleanup. Marcando como inválido."), hPipe, ctx->listaJogadores[i].username);
                ctx->listaJogadores[i].hPipe = INVALID_HANDLE_VALUE;
                // if (ctx->listaJogadores[i].ativo) { // Should already be handled by RemoverJogador
                //    ctx->listaJogadores[i].ativo = FALSE;
                //    if(ctx->totalJogadoresAtivos > 0) ctx->totalJogadoresAtivos--;
                //    VerificarEstadoJogo(ctx);
                // }
                break;
            }
        }
        LeaveCriticalSection(&ctx->csListaJogadores);

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    if (args != NULL) free(args); // Free the THREAD_ARGS structure allocated in _tmain

    Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Thread para '%s' terminada."), hPipe, usernameEsteCliente[0] ? usernameEsteCliente : _T("(desconhecido/não juntou)"));
    return 0;
}


void RemoverJogador(SERVER_CONTEXT* ctx, const TCHAR* username, BOOL notificarClienteParaSair) {
    Log(&ctx->csLog, _T("[JOGADOR] Tentando remover '%s'... (Notificar: %s)"), username, notificarClienteParaSair ? _T("SIM") : _T("NÃO"));
    int idxRemovido = -1;
    HANDLE hPipeDoRemovido = INVALID_HANDLE_VALUE;
    TCHAR nomeDoRemovido[MAX_USERNAME]; // To store username for logging after CS
    nomeDoRemovido[0] = _T('\0');

    EnterCriticalSection(&ctx->csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo && _tcscmp(ctx->listaJogadores[i].username, username) == 0) {
            StringCchCopy(nomeDoRemovido, MAX_USERNAME, ctx->listaJogadores[i].username);
            Log(&ctx->csLog, _T("[JOGADOR] '%s' (idx %lu, pipe %p) encontrado para remoção."), nomeDoRemovido, i, ctx->listaJogadores[i].hPipe);

            ctx->listaJogadores[i].ativo = FALSE;
            hPipeDoRemovido = ctx->listaJogadores[i].hPipe; // Store pipe handle to use after releasing CS
            ctx->listaJogadores[i].hPipe = INVALID_HANDLE_VALUE; // Invalidate pipe in list
            // ZeroMemory(ctx->listaJogadores[i].username, sizeof(ctx->listaJogadores[i].username)); // Optional: clear username
            // ctx->listaJogadores[i].pontos = 0; // Optional: reset points

            if (ctx->totalJogadoresAtivos > 0) ctx->totalJogadoresAtivos--;
            idxRemovido = (int)i;
            Log(&ctx->csLog, _T("[JOGADOR] '%s' marcado como inativo. Total ativos agora: %lu"), nomeDoRemovido, ctx->totalJogadoresAtivos);
            break;
        }
    }
    LeaveCriticalSection(&ctx->csListaJogadores);

    if (idxRemovido != -1) { // If player was found and marked inactive
        if (notificarClienteParaSair && hPipeDoRemovido != INVALID_HANDLE_VALUE) {
            MESSAGE msgKick; ZeroMemory(&msgKick, sizeof(MESSAGE));
            StringCchCopy(msgKick.type, _countof(msgKick.type), _T("SHUTDOWN")); // Or a specific KICKED message type
            StringCchCopy(msgKick.username, _countof(msgKick.username), _T("Arbitro"));
            StringCchCopy(msgKick.data, _countof(msgKick.data), _T("Você foi desconectado/excluído pelo administrador."));
            DWORD bw;
            // This WriteFile might fail if client already disconnected, which is fine.
            // Use overlapped I/O for this too if strict non-blocking is required, but for a kick, sync is often acceptable.
            WriteFile(hPipeDoRemovido, &msgKick, sizeof(MESSAGE), &bw, NULL);
            // The client thread itself will close its end of the pipe upon receiving SHUTDOWN or error.
            // The server should only close its handle for this pipe if it's certain the client thread for it has also exited.
            // However, since hPipeDoRemovido is a copy, and the actual handle is closed in ThreadClienteConectado cleanup, this is okay.
            // But generally, avoid closing a pipe handle here that another thread might still be using for writes.
            // The current design: ThreadClienteConectado owns its hPipe for reading/writing and closes it.
            // Here, we are just sending a final message.
        }

        // Notify all *other* players that this player left
        MESSAGE msgNotificacaoSaida; ZeroMemory(&msgNotificacaoSaida, sizeof(MESSAGE));
        StringCchCopy(msgNotificacaoSaida.type, _countof(msgNotificacaoSaida.type), _T("GAME_UPDATE"));
        StringCchPrintf(msgNotificacaoSaida.data, _countof(msgNotificacaoSaida.data), _T("Jogador %s saiu do jogo."), nomeDoRemovido);
        NotificarTodosOsJogadores(ctx, &msgNotificacaoSaida, nomeDoRemovido); // Skip the player who just left

        VerificarEstadoJogo(ctx); // Check if game needs to stop due to insufficient players
    }
    else {
        LogWarning(&ctx->csLog, _T("[JOGADOR] '%s' não encontrado para remoção ou já estava inativo."), username);
    }
    // Note: The pipe handle stored in hPipeDoRemovido is not explicitly closed here.
    // It is expected that the corresponding ThreadClienteConectado will perform the DisconnectNamedPipe and CloseHandle.
    // This function (RemoverJogador) primarily updates the shared player list and notifies.
}


// EncontrarJogador: Caller MUST hold ctx->csListaJogadores
int EncontrarJogador(SERVER_CONTEXT* ctx, const TCHAR* username) {
    // This function assumes csListaJogadores is already held by the caller
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo && _tcscmp(ctx->listaJogadores[i].username, username) == 0) {
            return (int)i; // Player found at index i
        }
    }
    return -1; // Player not found
}


BOOL ValidarPalavraJogo(THREAD_ARGS* argsClienteThread, const TCHAR* palavraSubmetida, const TCHAR* usernameJogador) {
    SERVER_CONTEXT* ctx = argsClienteThread->serverCtx;
    HANDLE hPipeCliente = argsClienteThread->hPipeCliente; // Pipe for responding to this specific client

    MESSAGE resposta; ZeroMemory(&resposta, sizeof(MESSAGE));
    StringCchCopy(resposta.username, _countof(resposta.username), usernameJogador); // For server logs, client knows its username
    float pontosAlterados = 0.0f;

    if (!ctx->jogoRealmenteAtivo) {
        StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
        StringCchCopy(resposta.data, _countof(resposta.data), _T("O jogo não está ativo."));
        // No points change if game not active
    }
    else {
        TCHAR palavraUpper[MAX_WORD];
        StringCchCopy(palavraUpper, MAX_WORD, palavraSubmetida);
        _tcsupr_s(palavraUpper, MAX_WORD); // Convert to uppercase for dictionary and board comparison

        BOOL existeNoDicionario = FALSE;
        EnterCriticalSection(&ctx->dicionario.csDicionario);
        if (ctx->dicionario.palavras && ctx->dicionario.totalPalavras > 0) { // Check if dictionary is loaded
            for (DWORD i = 0; i < ctx->dicionario.totalPalavras; ++i) {
                if (ctx->dicionario.palavras[i] && _tcscmp(ctx->dicionario.palavras[i], palavraUpper) == 0) {
                    existeNoDicionario = TRUE;
                    break;
                }
            }
        }
        LeaveCriticalSection(&ctx->dicionario.csDicionario);

        if (!existeNoDicionario) {
            StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
            StringCchPrintf(resposta.data, _countof(resposta.data), _T("'%s' não existe no dicionário."), palavraSubmetida);
            pontosAlterados = -((float)_tcslen(palavraSubmetida) * 0.5f); // Penalty for invalid word
        }
        else {
            // Word is in dictionary, now check if it can be formed from letters on board
            WaitForSingleObject(ctx->hMutexShm, INFINITE);
            BOOL podeFormar = TRUE;
            TCHAR letrasDisponiveisCopia[MAX_LETRAS_TABULEIRO + 1]; // Make a mutable copy of board letters
            int numMaxLetrasAtual = 0;
            if (ctx->pDadosShm) { // Check if SHM is valid
                numMaxLetrasAtual = ctx->pDadosShm->numMaxLetrasAtual;
                for (int k = 0; k < numMaxLetrasAtual && k < MAX_LETRAS_TABULEIRO; ++k) {
                    letrasDisponiveisCopia[k] = ctx->pDadosShm->letrasVisiveis[k];
                }
                letrasDisponiveisCopia[numMaxLetrasAtual < MAX_LETRAS_TABULEIRO ? numMaxLetrasAtual : MAX_LETRAS_TABULEIRO] = _T('\0');
            }
            else {
                podeFormar = FALSE; // Cannot check if SHM is gone
            }


            size_t lenPalavra = _tcslen(palavraUpper);
            if (lenPalavra == 0) podeFormar = FALSE; // Empty word cannot be formed

            if (podeFormar) { // Only proceed if SHM was accessible and word is not empty
                for (size_t k = 0; k < lenPalavra; ++k) { // For each char in the submitted word
                    BOOL encontrouLetra = FALSE;
                    for (int m = 0; m < numMaxLetrasAtual; ++m) { // Check against available letters
                        if (letrasDisponiveisCopia[m] == palavraUpper[k]) {
                            letrasDisponiveisCopia[m] = _T(' '); // Mark letter as used (use a placeholder)
                            encontrouLetra = TRUE;
                            break;
                        }
                    }
                    if (!encontrouLetra) {
                        podeFormar = FALSE; // Required letter not found on board
                        break;
                    }
                }
            }


            if (podeFormar && ctx->pDadosShm) {
                StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_VALID"));
                pontosAlterados = (float)lenPalavra; // Points equal to word length
                StringCchPrintf(resposta.data, _countof(resposta.data), _T("'%s' válida! +%.1f pts."), palavraSubmetida, pontosAlterados);

                // Remove used letters from actual shared memory board
                for (size_t k = 0; k < lenPalavra; ++k) {
                    for (int m = 0; m < numMaxLetrasAtual; ++m) {
                        if (ctx->pDadosShm->letrasVisiveis[m] == palavraUpper[k]) {
                            ctx->pDadosShm->letrasVisiveis[m] = _T('_'); // Replace with blank
                            break; // Move to next char of the word
                        }
                    }
                }
                ctx->pDadosShm->generationCount++; // Increment generation due to letter removal
                StringCchCopy(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, palavraSubmetida);
                StringCchCopy(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, usernameJogador);
                ctx->pDadosShm->pontuacaoUltimaPalavra = (int)pontosAlterados;
                SetEvent(ctx->hEventoShmUpdate); // Signal SHM update

                // Notify all players about the successful word
                MESSAGE notifPalavra; ZeroMemory(&notifPalavra, sizeof(MESSAGE));
                StringCchCopy(notifPalavra.type, _countof(notifPalavra.type), _T("GAME_UPDATE"));
                StringCchPrintf(notifPalavra.data, _countof(notifPalavra.data), _T("%s acertou '%s' (+%.1f pts)!"), usernameJogador, palavraSubmetida, pontosAlterados);
                NotificarTodosOsJogadores(ctx, &notifPalavra, NULL); // Send to all, including self

            }
            else { // Cannot be formed or SHM error
                StringCchCopy(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
                if (ctx->pDadosShm) {
                    StringCchPrintf(resposta.data, _countof(resposta.data), _T("'%s' não pode ser formada com as letras atuais."), palavraSubmetida);
                }
                else {
                    StringCchPrintf(resposta.data, _countof(resposta.data), _T("Erro de servidor ao validar '%s'."), palavraSubmetida);
                }
                pontosAlterados = -((float)lenPalavra * 0.5f); // Penalty
            }
            ReleaseMutex(ctx->hMutexShm);
        }
    }
    resposta.pontos = (int)floor(pontosAlterados + 0.5f); // Store points change (rounded for int if needed)

    // Update player's score in the server's list
    if (pontosAlterados != 0.0f) {
        EnterCriticalSection(&ctx->csListaJogadores);
        int idxJogador = EncontrarJogador(ctx, usernameJogador); // csListaJogadores is held
        if (idxJogador != -1) {
            ctx->listaJogadores[idxJogador].pontos += pontosAlterados;
            Log(&ctx->csLog, _T("[JOGO] %s (%s '%s'): %+.1f pts. Total: %.1f"), usernameJogador,
                (_tcscmp(resposta.type, _T("WORD_VALID")) == 0 ? _T("acertou") : _T("errou/inválida")),
                palavraSubmetida,
                pontosAlterados, ctx->listaJogadores[idxJogador].pontos);
        }
        LeaveCriticalSection(&ctx->csListaJogadores);
    }

    // Send response (WORD_VALID or WORD_INVALID) back to the client who submitted the word
    DWORD bytesEscritos;
    // Consider using overlapped I/O for this write as well if hPipeCliente supports it
    // and if ThreadClienteConectado uses a single event for all its overlapped operations.
    // For simplicity, a blocking write is used here.
    WriteFile(hPipeCliente, &resposta, sizeof(MESSAGE), &bytesEscritos, NULL);
    return (_tcscmp(resposta.type, _T("WORD_VALID")) == 0);
}

void VerificarEstadoJogo(SERVER_CONTEXT* ctx) {
    EnterCriticalSection(&ctx->csListaJogadores);
    BOOL deveEstarAtivo = (ctx->totalJogadoresAtivos >= MIN_JOGADORES_PARA_INICIAR);
    DWORD totalJogadoresAtivosSnapshot = ctx->totalJogadoresAtivos; // For logging outside CS
    LeaveCriticalSection(&ctx->csListaJogadores);

    WaitForSingleObject(ctx->hMutexShm, INFINITE);
    if (!ctx->pDadosShm) { // Safety check
        ReleaseMutex(ctx->hMutexShm);
        LogError(&ctx->csLog, _T("[JOGO] SHM nula em VerificarEstadoJogo."));
        return;
    }
    BOOL estavaAtivo = ctx->pDadosShm->jogoAtivo; // Current state in SHM

    if (deveEstarAtivo && !estavaAtivo) { // Game should start
        ctx->pDadosShm->jogoAtivo = TRUE;
        ctx->jogoRealmenteAtivo = TRUE; // Server's internal flag
        Log(&ctx->csLog, _T("[JOGO] O jogo começou/recomeçou! (%lu jogadores)"), totalJogadoresAtivosSnapshot);

        // Reset board and last word info
        for (int i = 0; i < ctx->pDadosShm->numMaxLetrasAtual; ++i) ctx->pDadosShm->letrasVisiveis[i] = _T('_');
        StringCchCopy(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
        StringCchCopy(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
        ctx->pDadosShm->pontuacaoUltimaPalavra = 0;
        ctx->pDadosShm->generationCount++; // New generation state
        SetEvent(ctx->hEventoShmUpdate);   // Notify clients of SHM change
        ReleaseMutex(ctx->hMutexShm);

        MESSAGE msgJogoIniciou; ZeroMemory(&msgJogoIniciou, sizeof(MESSAGE));
        StringCchCopy(msgJogoIniciou.type, _countof(msgJogoIniciou.type), _T("GAME_UPDATE"));
        StringCchCopy(msgJogoIniciou.username, _countof(msgJogoIniciou.username), _T("Arbitro"));
        StringCchCopy(msgJogoIniciou.data, _countof(msgJogoIniciou.data), _T("O jogo começou! Boa sorte!"));
        NotificarTodosOsJogadores(ctx, &msgJogoIniciou, NULL);

    }
    else if (!deveEstarAtivo && estavaAtivo) { // Game should stop
        ctx->pDadosShm->jogoAtivo = FALSE;
        ctx->jogoRealmenteAtivo = FALSE; // Server's internal flag
        Log(&ctx->csLog, _T("[JOGO] Jogo parado. Menos de %d jogadores. (%lu jogadores)"), MIN_JOGADORES_PARA_INICIAR, totalJogadoresAtivosSnapshot);

        // Reset board and last word info
        for (int i = 0; i < ctx->pDadosShm->numMaxLetrasAtual; ++i) ctx->pDadosShm->letrasVisiveis[i] = _T('_');
        StringCchCopy(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
        StringCchCopy(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
        ctx->pDadosShm->pontuacaoUltimaPalavra = 0;
        ctx->pDadosShm->generationCount++; // New generation state
        SetEvent(ctx->hEventoShmUpdate);   // Notify clients of SHM change
        ReleaseMutex(ctx->hMutexShm);

        MESSAGE msgJogoParou; ZeroMemory(&msgJogoParou, sizeof(MESSAGE));
        StringCchCopy(msgJogoParou.type, _countof(msgJogoParou.type), _T("GAME_UPDATE"));
        StringCchCopy(msgJogoParou.username, _countof(msgJogoParou.username), _T("Arbitro"));
        StringCchCopy(msgJogoParou.data, _countof(msgJogoParou.data), _T("Jogo parado. Aguardando mais jogadores..."));
        NotificarTodosOsJogadores(ctx, &msgJogoParou, NULL);
    }
    else { // No change in game state needed
        ReleaseMutex(ctx->hMutexShm);
    }
}


void NotificarTodosOsJogadores(SERVER_CONTEXT* ctx, const MESSAGE* msgAEnviar, const TCHAR* skipUsername) {
    EnterCriticalSection(&ctx->csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo && ctx->listaJogadores[i].hPipe != INVALID_HANDLE_VALUE) {
            if (skipUsername == NULL || _tcscmp(ctx->listaJogadores[i].username, skipUsername) != 0) {
                DWORD bytesEscritos;
                // Using overlapped I/O for notifications is safer to prevent server stalls
                // Each notification should use its own OVERLAPPED structure or a pool.
                // For simplicity, if pipes are created with FILE_FLAG_OVERLAPPED, WriteFile needs it.
                // If not, a blocking WriteFile is fine but can stall if a client is unresponsive.
                // Assuming pipes are overlapped as per CreateNamedPipe in _tmain.

                OVERLAPPED olNotify; ZeroMemory(&olNotify, sizeof(OVERLAPPED));
                olNotify.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL); // Manual-reset for this operation
                if (olNotify.hEvent == NULL) {
                    LogError(&ctx->csLog, _T("[NOTIFICAR] Falha ao criar evento para notificação para %s."), ctx->listaJogadores[i].username);
                    continue; // Try next player
                }

                BOOL fWriteSuccess = WriteFile(ctx->listaJogadores[i].hPipe, msgAEnviar, sizeof(MESSAGE), &bytesEscritos, &olNotify);
                if (!fWriteSuccess && GetLastError() == ERROR_IO_PENDING) {
                    // Wait for a short timeout. If it fails, the client might be unresponsive.
                    if (WaitForSingleObject(olNotify.hEvent, 1000) == WAIT_OBJECT_0) { // 1 sec timeout
                        GetOverlappedResult(ctx->listaJogadores[i].hPipe, &olNotify, &bytesEscritos, FALSE); // Get result, don't wait further
                        if (bytesEscritos != sizeof(MESSAGE)) {
                            // LogWarning(&ctx->csLog, _T("[NOTIFICAR] WriteFile Overlapped enviou %lu bytes em vez de %zu para %s."), bytesEscritos, sizeof(MESSAGE), ctx->listaJogadores[i].username);
                        }
                    }
                    else {
                        LogWarning(&ctx->csLog, _T("[NOTIFICAR] Timeout/Erro (%lu) ao enviar notificação para %s. Cancelando IO."), GetLastError(), ctx->listaJogadores[i].username);
                        CancelIoEx(ctx->listaJogadores[i].hPipe, &olNotify); // Cancel the pending I/O
                        // Consider this player problematic, might need more robust error handling (e.g., mark for disconnect)
                    }
                }
                else if (!fWriteSuccess) { // Write failed immediately
                    LogWarning(&ctx->csLog, _T("[NOTIFICAR] Falha imediata ao enviar notificação para %s (pipe %p): %lu"), ctx->listaJogadores[i].username, ctx->listaJogadores[i].hPipe, GetLastError());
                }
                // If fWriteSuccess is TRUE (completed synchronously with OVERLAPPED struct)
                // GetOverlappedResult is still typically called to get the final status and bytes written.
                // else { // Synchronous success
                //    GetOverlappedResult(ctx->listaJogadores[i].hPipe, &olNotify, &bytesEscritos, FALSE);
                // }
                CloseHandle(olNotify.hEvent);
            }
        }
    }
    LeaveCriticalSection(&ctx->csListaJogadores);
}