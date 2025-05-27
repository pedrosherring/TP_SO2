#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <fcntl.h> // Para _setmode
#include <io.h>    // Para _setmode, _fileno
#include <strsafe.h>

#include "../Comum/compartilhado.h" // Ficheiro revisto

// Definição manual de _countof se não estiver disponível
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array[0]))
#endif

// Definição de Timeout
#define IO_TIMEOUT 5000

// --- JOGOUI Context Structure ---
typedef struct {
    TCHAR meuUsername[MAX_USERNAME];
    HANDLE hPipeServidor;

    DadosJogoCompartilhados* pDadosShmCliente;
    HANDLE hMapFileShmCliente;
    HANDLE hEventoShmUpdateCliente;
    HANDLE hMutexShmCliente;

    volatile BOOL clienteRodando;
    long ultimaGeracaoConhecida;

    CRITICAL_SECTION csConsoleCliente;

    HANDLE hThreadReceptorPipe;
    HANDLE hThreadMonitorShm;
} JOGOUI_CONTEXT;

// ==========================================================================================
// PROTÓTIPOS DE FUNÇÕES INTERNAS (FORWARD DECLARATIONS, taking JOGOUI_CONTEXT*)
// ==========================================================================================
void LogCliente(JOGOUI_CONTEXT* ctx, const TCHAR* format, ...);
void LogErrorCliente(JOGOUI_CONTEXT* ctx, const TCHAR* format, ...);
void LogWarningCliente(JOGOUI_CONTEXT* ctx, const TCHAR* format, ...);

BOOL ConectarAoServidorJogo(JOGOUI_CONTEXT* ctx);
BOOL AbrirRecursosCompartilhadosCliente(JOGOUI_CONTEXT* ctx);
void LimparRecursosCliente(JOGOUI_CONTEXT* ctx);
void EnviarMensagemAoServidor(JOGOUI_CONTEXT* ctx, const MESSAGE* msg);
void MostrarEstadoJogoCliente(JOGOUI_CONTEXT* ctx);
void ProcessarInputUtilizador(JOGOUI_CONTEXT* ctx, const TCHAR* input);
DWORD WINAPI ThreadReceptorMensagensServidor(LPVOID param);
DWORD WINAPI ThreadMonitorSharedMemoryCliente(LPVOID param);

// ==========================================================================================
// FUNÇÃO PRINCIPAL - _tmain
// ==========================================================================================
int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    JOGOUI_CONTEXT uiCtx; // Instance of the JogoUI context
    ZeroMemory(&uiCtx, sizeof(JOGOUI_CONTEXT));
    // Initialize handles to ensure they are invalid before use
    uiCtx.hPipeServidor = INVALID_HANDLE_VALUE;
    uiCtx.hMapFileShmCliente = NULL;
    uiCtx.pDadosShmCliente = NULL;
    uiCtx.hEventoShmUpdateCliente = NULL;
    uiCtx.hMutexShmCliente = NULL;
    uiCtx.hThreadReceptorPipe = NULL;
    uiCtx.hThreadMonitorShm = NULL;
    uiCtx.clienteRodando = TRUE;
    uiCtx.ultimaGeracaoConhecida = -1;


    if (argc != 2) {
        _tprintf(_T("Uso: jogoui.exe <username>\n"));
        return 1;
    }
    StringCchCopy(uiCtx.meuUsername, MAX_USERNAME, argv[1]);
    InitializeCriticalSection(&uiCtx.csConsoleCliente);

    LogCliente(&uiCtx, _T("JogoUI para '%s' a iniciar..."), uiCtx.meuUsername);

    if (!ConectarAoServidorJogo(&uiCtx)) {
        LogErrorCliente(&uiCtx, _T("Falha ao conectar ao servidor. Encerrando."));
        LimparRecursosCliente(&uiCtx);
        DeleteCriticalSection(&uiCtx.csConsoleCliente);
        _tprintf(_T("Pressione Enter para sair...\n")); (void)getchar();
        return 1;
    }

    if (!AbrirRecursosCompartilhadosCliente(&uiCtx)) {
        LogErrorCliente(&uiCtx, _T("Falha ao abrir recursos compartilhados. Encerrando."));
        LimparRecursosCliente(&uiCtx);
        DeleteCriticalSection(&uiCtx.csConsoleCliente);
        _tprintf(_T("Pressione Enter para sair...\n")); (void)getchar();
        return 1;
    }

    MESSAGE msgJoin; ZeroMemory(&msgJoin, sizeof(MESSAGE));
    StringCchCopy(msgJoin.type, _countof(msgJoin.type), _T("JOIN"));
    StringCchCopy(msgJoin.username, _countof(msgJoin.username), uiCtx.meuUsername);
    EnviarMensagemAoServidor(&uiCtx, &msgJoin);

    uiCtx.hThreadReceptorPipe = CreateThread(NULL, 0, ThreadReceptorMensagensServidor, &uiCtx, 0, NULL);
    if (uiCtx.hThreadReceptorPipe == NULL) {
        LogErrorCliente(&uiCtx, _T("Falha ao criar thread receptora de pipe. Encerrando."));
        uiCtx.clienteRodando = FALSE;
        LimparRecursosCliente(&uiCtx);
        DeleteCriticalSection(&uiCtx.csConsoleCliente);
        _tprintf(_T("Pressione Enter para sair...\n")); (void)getchar();
        return 1;
    }

    uiCtx.hThreadMonitorShm = CreateThread(NULL, 0, ThreadMonitorSharedMemoryCliente, &uiCtx, 0, NULL);
    if (uiCtx.hThreadMonitorShm == NULL) {
        LogErrorCliente(&uiCtx, _T("Falha ao criar thread monitora de SHM. Atualizações do tabuleiro podem não funcionar."));
        // Not necessarily fatal, but the board won't auto-update via this thread.
    }

    TCHAR inputBuffer[MAX_WORD + 20]; // Local to _tmain

    EnterCriticalSection(&uiCtx.csConsoleCliente);
    MostrarEstadoJogoCliente(&uiCtx);
    _tprintf(_T("%s> "), uiCtx.meuUsername);
    fflush(stdout);
    LeaveCriticalSection(&uiCtx.csConsoleCliente);

    while (uiCtx.clienteRodando) {
        if (_fgetts(inputBuffer, _countof(inputBuffer), stdin) != NULL) {
            inputBuffer[_tcscspn(inputBuffer, _T("\r\n"))] = _T('\0');
            if (!uiCtx.clienteRodando) break;

            ProcessarInputUtilizador(&uiCtx, inputBuffer);

            if (uiCtx.clienteRodando) {
                EnterCriticalSection(&uiCtx.csConsoleCliente);
                _tprintf(_T("\n%s> "), uiCtx.meuUsername);
                fflush(stdout);
                LeaveCriticalSection(&uiCtx.csConsoleCliente);
            }
        }
        else {
            if (uiCtx.clienteRodando) {
                LogErrorCliente(&uiCtx, _T("Erro ou EOF ao ler input. Encerrando..."));
                uiCtx.clienteRodando = FALSE;
            }
            break;
        }
    }

    LogCliente(&uiCtx, _T("Loop principal de input terminado. Aguardando threads..."));
    if (uiCtx.hEventoShmUpdateCliente) SetEvent(uiCtx.hEventoShmUpdateCliente); // Signal threads to wake up and check clienteRodando

    if (uiCtx.hThreadReceptorPipe != NULL) {
        if (WaitForSingleObject(uiCtx.hThreadReceptorPipe, 3000) == WAIT_TIMEOUT) {
            LogWarningCliente(&uiCtx, _T("Timeout ao aguardar thread receptora de pipe."));
            // TerminateThread(uiCtx.hThreadReceptorPipe, 0); // Consider if necessary
        }
        CloseHandle(uiCtx.hThreadReceptorPipe);
        uiCtx.hThreadReceptorPipe = NULL;
    }
    if (uiCtx.hThreadMonitorShm != NULL) {
        if (WaitForSingleObject(uiCtx.hThreadMonitorShm, 2000) == WAIT_TIMEOUT) {
            LogWarningCliente(&uiCtx, _T("Timeout ao aguardar thread monitora de SHM."));
            // TerminateThread(uiCtx.hThreadMonitorShm, 0); // Consider if necessary
        }
        CloseHandle(uiCtx.hThreadMonitorShm);
        uiCtx.hThreadMonitorShm = NULL;
    }

    LimparRecursosCliente(&uiCtx);
    LogCliente(&uiCtx, _T("JogoUI para '%s' encerrado."), uiCtx.meuUsername);
    DeleteCriticalSection(&uiCtx.csConsoleCliente);
    _tprintf(_T("\nDesconectado. Pressione Enter para sair...\n"));
    (void)getchar();
    return 0;
}

// ==========================================================================================
// FUNÇÕES AUXILIARES E THREADS DO JOGOUI
// ==========================================================================================
void LogCliente(JOGOUI_CONTEXT* ctx, const TCHAR* format, ...) {
    if (ctx == NULL || ctx->csConsoleCliente.DebugInfo == NULL) { // Check if CS is initialized
        TCHAR fbBuffer[1024];
        va_list fbArgs;
        va_start(fbArgs, format);
        StringCchVPrintf(fbBuffer, _countof(fbBuffer), format, fbArgs);
        va_end(fbArgs);
        // Fallback if username in ctx is not yet set or ctx is null
        _tprintf_s(_T("[JOGOUI-NO_CS_CTX] %s\n"), fbBuffer);
        fflush(stdout);
        return;
    }

    EnterCriticalSection(&ctx->csConsoleCliente);
    TCHAR buffer[2048];
    va_list args;
    va_start(args, format);
    SYSTEMTIME st;
    GetLocalTime(&st);
    size_t prefixLen = 0;

    StringCchPrintf(buffer, _countof(buffer), _T("\n%02d:%02d:%02d.%03d [%s-JOGOUI] "),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ctx->meuUsername[0] ? ctx->meuUsername : _T("CLIENT"));

    (void)StringCchLength(buffer, _countof(buffer), &prefixLen);

    if (prefixLen < _countof(buffer) - 1) {
        StringCchVPrintf(buffer + prefixLen, _countof(buffer) - prefixLen, format, args);
    }

    StringCchCat(buffer, _countof(buffer), _T("\n"));
    _tprintf_s(buffer);
    fflush(stdout);
    va_end(args);
    LeaveCriticalSection(&ctx->csConsoleCliente);
}

void LogErrorCliente(JOGOUI_CONTEXT* ctx, const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    LogCliente(ctx, _T("[ERRO] %s"), buffer);
}

void LogWarningCliente(JOGOUI_CONTEXT* ctx, const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    LogCliente(ctx, _T("[AVISO] %s"), buffer);
}

BOOL ConectarAoServidorJogo(JOGOUI_CONTEXT* ctx) {
    int tentativas = 0;
    const int MAX_TENTATIVAS_PIPE = 5;
    LogCliente(ctx, _T("Tentando conectar ao pipe do servidor: %s"), PIPE_NAME);
    while (tentativas < MAX_TENTATIVAS_PIPE && ctx->clienteRodando) {
        ctx->hPipeServidor = CreateFile(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        if (ctx->hPipeServidor != INVALID_HANDLE_VALUE) {
            DWORD dwMode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(ctx->hPipeServidor, &dwMode, NULL, NULL)) {
                LogErrorCliente(ctx, _T("Falha ao definir modo do pipe para mensagem: %lu"), GetLastError());
                CloseHandle(ctx->hPipeServidor);
                ctx->hPipeServidor = INVALID_HANDLE_VALUE;
                return FALSE;
            }
            LogCliente(ctx, _T("Conectado ao servidor com sucesso (Pipe: %p)."), ctx->hPipeServidor);
            return TRUE;
        }

        DWORD dwError = GetLastError();
        if (dwError != ERROR_PIPE_BUSY && dwError != ERROR_FILE_NOT_FOUND) {
            LogErrorCliente(ctx, _T("Erro não esperado ao conectar ao pipe: %lu"), dwError);
            return FALSE;
        }
        LogWarningCliente(ctx, _T("Pipe ocupado ou não encontrado (tentativa %d/%d). Tentando novamente em 1s..."), tentativas + 1, MAX_TENTATIVAS_PIPE);
        Sleep(1000);
        tentativas++;
    }
    if (!ctx->clienteRodando) LogCliente(ctx, _T("Conexão cancelada durante tentativas."));
    else LogErrorCliente(ctx, _T("Não foi possível conectar ao servidor após %d tentativas."), MAX_TENTATIVAS_PIPE);
    return FALSE;
}

BOOL AbrirRecursosCompartilhadosCliente(JOGOUI_CONTEXT* ctx) {
    ctx->hMapFileShmCliente = OpenFileMapping(FILE_MAP_READ, FALSE, SHM_NAME);
    if (ctx->hMapFileShmCliente == NULL) {
        LogErrorCliente(ctx, _T("Falha ao abrir FileMapping '%s': %lu"), SHM_NAME, GetLastError());
        return FALSE;
    }
    ctx->pDadosShmCliente = (DadosJogoCompartilhados*)MapViewOfFile(ctx->hMapFileShmCliente, FILE_MAP_READ, 0, 0, sizeof(DadosJogoCompartilhados));
    if (ctx->pDadosShmCliente == NULL) {
        LogErrorCliente(ctx, _T("Falha ao mapear SHM '%s': %lu"), SHM_NAME, GetLastError());
        CloseHandle(ctx->hMapFileShmCliente); ctx->hMapFileShmCliente = NULL;
        return FALSE;
    }
    ctx->hEventoShmUpdateCliente = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EVENT_SHM_UPDATE);
    if (ctx->hEventoShmUpdateCliente == NULL) {
        LogErrorCliente(ctx, _T("Falha ao abrir evento SHM '%s': %lu."), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(ctx->pDadosShmCliente); ctx->pDadosShmCliente = NULL;
        CloseHandle(ctx->hMapFileShmCliente); ctx->hMapFileShmCliente = NULL;
        return FALSE;
    }
    ctx->hMutexShmCliente = OpenMutex(SYNCHRONIZE, FALSE, MUTEX_SHARED_MEM);
    if (ctx->hMutexShmCliente == NULL) {
        // This might not be fatal for a read-only client, but data might be inconsistent.
        LogWarningCliente(ctx, _T("Falha ao abrir mutex da SHM '%s': %lu. Leitura pode ter pequenas inconsistências visuais."), MUTEX_SHARED_MEM, GetLastError());
    }

    LogCliente(ctx, _T("Recursos compartilhados abertos com sucesso."));
    if (ctx->pDadosShmCliente) { // Initialize last known generation
        if (ctx->hMutexShmCliente) WaitForSingleObject(ctx->hMutexShmCliente, INFINITE);
        ctx->ultimaGeracaoConhecida = ctx->pDadosShmCliente->generationCount;
        if (ctx->hMutexShmCliente) ReleaseMutex(ctx->hMutexShmCliente);
    }
    return TRUE;
}

void LimparRecursosCliente(JOGOUI_CONTEXT* ctx) {
    LogCliente(ctx, _T("Limpando recursos do cliente..."));
    if (ctx->hPipeServidor != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->hPipeServidor);
        ctx->hPipeServidor = INVALID_HANDLE_VALUE;
    }
    if (ctx->pDadosShmCliente != NULL) {
        UnmapViewOfFile(ctx->pDadosShmCliente);
        ctx->pDadosShmCliente = NULL;
    }
    if (ctx->hMapFileShmCliente != NULL) {
        CloseHandle(ctx->hMapFileShmCliente);
        ctx->hMapFileShmCliente = NULL;
    }
    if (ctx->hEventoShmUpdateCliente != NULL) {
        CloseHandle(ctx->hEventoShmUpdateCliente);
        ctx->hEventoShmUpdateCliente = NULL;
    }
    if (ctx->hMutexShmCliente != NULL) {
        CloseHandle(ctx->hMutexShmCliente);
        ctx->hMutexShmCliente = NULL;
    }
}

void EnviarMensagemAoServidor(JOGOUI_CONTEXT* ctx, const MESSAGE* msg) {
    if (ctx->hPipeServidor == INVALID_HANDLE_VALUE || !ctx->clienteRodando) {
        LogErrorCliente(ctx, _T("Não é possível enviar mensagem: pipe inválido ou cliente não está rodando."));
        return;
    }
    DWORD bytesEscritos;
    OVERLAPPED ovWrite; ZeroMemory(&ovWrite, sizeof(OVERLAPPED));
    ovWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ovWrite.hEvent == NULL) {
        LogErrorCliente(ctx, _T("Falha ao criar evento para WriteFile. Mensagem tipo '%s' não enviada."), msg->type);
        return;
    }

    if (!WriteFile(ctx->hPipeServidor, msg, sizeof(MESSAGE), &bytesEscritos, &ovWrite)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ovWrite.hEvent, IO_TIMEOUT) == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(ctx->hPipeServidor, &ovWrite, &bytesEscritos, FALSE) || bytesEscritos != sizeof(MESSAGE)) {
                    if (ctx->clienteRodando) LogErrorCliente(ctx, _T("GetOverlappedResult falhou ou bytes incorretos (%lu) para msg tipo '%s'."), bytesEscritos, msg->type);
                }
            }
            else {
                if (ctx->clienteRodando) LogErrorCliente(ctx, _T("Timeout ao enviar mensagem tipo '%s'. Cancelando IO."), msg->type);
                CancelIoEx(ctx->hPipeServidor, &ovWrite);
                if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) ctx->clienteRodando = FALSE;
            }
        }
        else {
            if (ctx->clienteRodando) LogErrorCliente(ctx, _T("Falha ao enviar mensagem tipo '%s' para o servidor: %lu"), msg->type, GetLastError());
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) ctx->clienteRodando = FALSE;
        }
    }
    else {
        if (!GetOverlappedResult(ctx->hPipeServidor, &ovWrite, &bytesEscritos, FALSE) || bytesEscritos != sizeof(MESSAGE)) {
            if (ctx->clienteRodando) LogErrorCliente(ctx, _T("WriteFile síncrono (com overlapped) falhou ou bytes incorretos (%lu) para msg tipo '%s'."), bytesEscritos, msg->type);
        }
    }
    CloseHandle(ovWrite.hEvent);
}

void MostrarEstadoJogoCliente(JOGOUI_CONTEXT* ctx) {
    // Note: This function is called from ThreadMonitorSharedMemoryCliente (which has the CS)
    // and from _tmain (which also takes the CS before calling).
    // So, the caller should handle Enter/LeaveCriticalSection for g_csConsoleCliente.
    // The original code also had the CS inside, which could lead to deadlocks if called from a thread already holding it.
    // For simplicity, assume caller handles CS for console output.

    if (ctx->pDadosShmCliente != NULL) {
        BOOL gotMutex = FALSE;
        if (ctx->hMutexShmCliente) {
            if (WaitForSingleObject(ctx->hMutexShmCliente, 100) == WAIT_OBJECT_0) { // Short timeout
                gotMutex = TRUE;
            }
            else {
                LogWarningCliente(ctx, _T("Timeout ao obter mutex da SHM para mostrar estado. Dados podem estar ligeiramente desatualizados."));
            }
        }

        _tprintf(_T("\n====================================\n"));
        _tprintf(_T("Letras: "));
        for (int i = 0; i < ctx->pDadosShmCliente->numMaxLetrasAtual; ++i) {
            _tprintf(_T("%c "), ctx->pDadosShmCliente->letrasVisiveis[i]);
        }
        _tprintf(_T("\n"));

        if (ctx->pDadosShmCliente->ultimaPalavraIdentificada[0] != _T('\0')) {
            _tprintf(_T("Última palavra por %s: %s (+%d pts)\n"),
                ctx->pDadosShmCliente->usernameUltimaPalavra,
                ctx->pDadosShmCliente->ultimaPalavraIdentificada,
                ctx->pDadosShmCliente->pontuacaoUltimaPalavra);
        }
        _tprintf(_T("Jogo Ativo: %s\n"), ctx->pDadosShmCliente->jogoAtivo ? _T("Sim") : _T("Não"));
        _tprintf(_T("====================================\n"));

        if (gotMutex && ctx->hMutexShmCliente) ReleaseMutex(ctx->hMutexShmCliente);
    }
    else {
        _tprintf(_T("\n[AVISO] Memória partilhada não disponível.\n"));
    }
    fflush(stdout);
}

void ProcessarInputUtilizador(JOGOUI_CONTEXT* ctx, const TCHAR* input) {
    MESSAGE msgParaServidor;
    ZeroMemory(&msgParaServidor, sizeof(MESSAGE));
    StringCchCopy(msgParaServidor.username, MAX_USERNAME, ctx->meuUsername);

    if (_tcslen(input) == 0) {
        return;
    }

    if (_tcscmp(input, _T(":sair")) == 0) {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("EXIT"));
        EnviarMensagemAoServidor(ctx, &msgParaServidor);
        ctx->clienteRodando = FALSE;
    }
    else if (_tcscmp(input, _T(":pont")) == 0) {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("GET_SCORE"));
        EnviarMensagemAoServidor(ctx, &msgParaServidor);
    }
    else if (_tcscmp(input, _T(":jogs")) == 0) {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("GET_JOGS"));
        EnviarMensagemAoServidor(ctx, &msgParaServidor);
    }
    else if (input[0] == _T(':')) {
        LogWarningCliente(ctx, _T("Comando desconhecido: '%s'"), input);
    }
    else {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("WORD"));
        StringCchCopy(msgParaServidor.data, MAX_WORD, input);
        EnviarMensagemAoServidor(ctx, &msgParaServidor);
    }
}

DWORD WINAPI ThreadReceptorMensagensServidor(LPVOID param) {
    JOGOUI_CONTEXT* ctx = (JOGOUI_CONTEXT*)param;
    MESSAGE msgDoServidor;
    DWORD bytesLidos;
    OVERLAPPED ovReadPipe; ZeroMemory(&ovReadPipe, sizeof(OVERLAPPED));
    ovReadPipe.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (ovReadPipe.hEvent == NULL) {
        LogErrorCliente(ctx, _T("TRA: Falha ao criar evento de leitura do pipe. Encerrando thread."));
        if (ctx) ctx->clienteRodando = FALSE; // Signal main loop to stop
        return 1;
    }
    LogCliente(ctx, _T("TRA: Thread Receptora de Mensagens iniciada."));

    while (ctx->clienteRodando) {
        ResetEvent(ovReadPipe.hEvent);
        BOOL sucessoLeitura = ReadFile(ctx->hPipeServidor, &msgDoServidor, sizeof(MESSAGE), &bytesLidos, &ovReadPipe);
        DWORD dwError = GetLastError();

        if (!sucessoLeitura && dwError == ERROR_IO_PENDING) {
            HANDLE handles[1] = { ovReadPipe.hEvent }; // Can add more handles here if needed (e.g. shutdown event)
            DWORD waitRes = WaitForMultipleObjects(1, handles, FALSE, 500); // Timeout of 500ms

            if (!ctx->clienteRodando) break; // Check if client was signaled to stop during wait

            if (waitRes == WAIT_TIMEOUT) {
                continue; // Loop again to check clienteRodando and ReadFile
            }
            else if (waitRes != WAIT_OBJECT_0) { // Error or other handle signaled
                if (ctx->clienteRodando) LogErrorCliente(ctx, _T("TRA: Erro %lu ao esperar ReadFile do pipe."), GetLastError());
                ctx->clienteRodando = FALSE; break;
            }
            // I/O completed (event was signaled)
            if (!GetOverlappedResult(ctx->hPipeServidor, &ovReadPipe, &bytesLidos, FALSE)) {
                if (ctx->clienteRodando) LogErrorCliente(ctx, _T("TRA: GOR falhou após ReadFile do pipe: %lu."), GetLastError());
                ctx->clienteRodando = FALSE; break;
            }
            sucessoLeitura = TRUE;
        }
        else if (!sucessoLeitura) {
            if (ctx->clienteRodando) {
                if (dwError == ERROR_BROKEN_PIPE) LogWarningCliente(ctx, _T("TRA: Pipe quebrado (servidor desconectou?)."));
                else LogErrorCliente(ctx, _T("TRA: ReadFile falhou imediatamente: %lu."), dwError);
            }
            ctx->clienteRodando = FALSE; break;
        }

        if (!ctx->clienteRodando) break;

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {
            EnterCriticalSection(&ctx->csConsoleCliente);
            _tprintf(_T("\n[SERVIDOR->%s] %s: %s (Pontos na msg: %d)\n"),
                ctx->meuUsername, msgDoServidor.type, msgDoServidor.data, msgDoServidor.pontos);

            if (_tcscmp(msgDoServidor.type, _T("JOIN_USER_EXISTS")) == 0 ||
                _tcscmp(msgDoServidor.type, _T("JOIN_GAME_FULL")) == 0 ||
                _tcscmp(msgDoServidor.type, _T("SHUTDOWN")) == 0) {
                LogWarningCliente(ctx, _T("Recebida mensagem de terminação/erro do servidor: %s"), msgDoServidor.type);
                ctx->clienteRodando = FALSE; // Signal main loop and this loop to stop
            }
            // Could add prompt re-display logic here if desired after server message
            // _tprintf(_T("%s> "), ctx->meuUsername); 
            fflush(stdout);
            LeaveCriticalSection(&ctx->csConsoleCliente);
            if (!ctx->clienteRodando) break; // Check again if processing the message caused termination

        }
        else if (sucessoLeitura && bytesLidos == 0) { // Pipe closed by server gracefully
            if (ctx->clienteRodando) { LogWarningCliente(ctx, _T("TRA: Servidor fechou a conexão (EOF).")); ctx->clienteRodando = FALSE; }
            break;
        }
        else if (bytesLidos != 0) { // Framing error or partial read (should not happen with PIPE_READMODE_MESSAGE)
            if (ctx->clienteRodando) { LogErrorCliente(ctx, _T("TRA: Mensagem incompleta/errada do servidor (%lu bytes)."), bytesLidos); ctx->clienteRodando = FALSE; }
            break;
        }
    }

    if (ovReadPipe.hEvent) CloseHandle(ovReadPipe.hEvent);
    LogCliente(ctx, _T("TRA: Thread Receptora de Mensagens a terminar."));
    return 0;
}

DWORD WINAPI ThreadMonitorSharedMemoryCliente(LPVOID param) {
    JOGOUI_CONTEXT* ctx = (JOGOUI_CONTEXT*)param;
    LogCliente(ctx, _T("TSM: Thread Monitora de SHM iniciada."));

    if (ctx->hEventoShmUpdateCliente == NULL || ctx->pDadosShmCliente == NULL) {
        LogErrorCliente(ctx, _T("TSM: Evento ou SHM não inicializados na thread. Encerrando thread."));
        return 1;
    }

    while (ctx->clienteRodando) {
        // Wait for the SHM update event or a timeout
        DWORD waitResult = WaitForSingleObject(ctx->hEventoShmUpdateCliente, 1000); // Wait for 1 sec or event

        if (!ctx->clienteRodando) break; // Exit if client is stopping

        if (waitResult == WAIT_OBJECT_0) { // Event signaled
            BOOL needsDisplay = FALSE;
            if (ctx->hMutexShmCliente) WaitForSingleObject(ctx->hMutexShmCliente, INFINITE);
            if (ctx->pDadosShmCliente->generationCount != ctx->ultimaGeracaoConhecida) {
                ctx->ultimaGeracaoConhecida = ctx->pDadosShmCliente->generationCount;
                needsDisplay = TRUE;
            }
            if (ctx->hMutexShmCliente) ReleaseMutex(ctx->hMutexShmCliente);

            if (needsDisplay) {
                EnterCriticalSection(&ctx->csConsoleCliente);
                MostrarEstadoJogoCliente(ctx); // This already handles its own mutex for SHM access for display
                _tprintf(_T("%s> "), ctx->meuUsername); // Re-display prompt
                fflush(stdout);
                LeaveCriticalSection(&ctx->csConsoleCliente);
            }

            if (!ResetEvent(ctx->hEventoShmUpdateCliente)) { // Reset the event
                // This error might occur if the event is closed while waiting, which shouldn't happen if cleanup is ordered.
                if (ctx->clienteRodando) LogErrorCliente(ctx, _T("TSM: Falha ao resetar evento SHM: %lu"), GetLastError());
            }
        }
        else if (waitResult == WAIT_TIMEOUT) {
            // Timeout occurred, could still check generation count just in case event was missed or not set
            BOOL needsDisplayOnTimeout = FALSE;
            if (ctx->hMutexShmCliente) WaitForSingleObject(ctx->hMutexShmCliente, INFINITE);
            if (ctx->pDadosShmCliente && ctx->pDadosShmCliente->generationCount != ctx->ultimaGeracaoConhecida) {
                ctx->ultimaGeracaoConhecida = ctx->pDadosShmCliente->generationCount;
                needsDisplayOnTimeout = TRUE;
            }
            if (ctx->hMutexShmCliente) ReleaseMutex(ctx->hMutexShmCliente);

            if (needsDisplayOnTimeout) {
                EnterCriticalSection(&ctx->csConsoleCliente);
                MostrarEstadoJogoCliente(ctx);
                _tprintf(_T("%s> "), ctx->meuUsername);
                fflush(stdout);
                LeaveCriticalSection(&ctx->csConsoleCliente);
            }
        }
        else { // Error waiting for event
            if (ctx->clienteRodando) LogErrorCliente(ctx, _T("TSM: Erro %lu ao esperar evento SHM."), GetLastError());
            Sleep(1000); // Prevent busy loop on persistent error
        }
    }
    LogCliente(ctx, _T("TSM: Thread Monitora de SHM a terminar."));
    return 0;
}