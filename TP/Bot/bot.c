#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h> // For malloc, free, rand, srand, _tstoi
#include <tchar.h>
#include <fcntl.h>  // For _setmode
#include <io.h>     // For _setmode, _fileno
#include <time.h>   // For srand, time

#include "../Comum/compartilhado.h" // Shared structures and IPC names

// Bot's dictionary settings
#define MAX_BOT_DICT_WORDS 20000 // Max words bot can load

// Definition of _countof if not available (e.g., older compilers)
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array[0]))
#endif

// --- Bot Context Structure ---
typedef struct {
    TCHAR botUsername[MAX_USERNAME];
    int reactionTimeSeconds;
    HANDLE hPipeServidorBot;

    DadosJogoCompartilhados* pDadosShmBot;
    HANDLE hMapFileShmBot;
    HANDLE hEventoShmUpdateBot;
    HANDLE hMutexShmBot;

    volatile BOOL botRodando;
    long botUltimaGeracaoConhecidaShm;
    float botScore;

    TCHAR* botDicionario[MAX_BOT_DICT_WORDS];
    DWORD totalPalavrasBotDicionario;

    CRITICAL_SECTION csBotConsole;
    CRITICAL_SECTION csBotData;

    HANDLE hThreadReceptorPipeBot;
} BOT_CONTEXT;

// --- Function Prototypes (taking BOT_CONTEXT*) ---
void LogBot(BOT_CONTEXT* ctx, const TCHAR* format, ...);
void LogErrorBot(BOT_CONTEXT* ctx, const TCHAR* format, ...);
void LogWarningBot(BOT_CONTEXT* ctx, const TCHAR* format, ...);

BOOL ProcessarArgumentosBot(BOT_CONTEXT* ctx, int argc, TCHAR* argv[]);
BOOL CarregarDicionarioBot(BOT_CONTEXT* ctx, const TCHAR* nomeArquivo);
void LiberarDicionarioBot(BOT_CONTEXT* ctx);
BOOL ConectarAoServidorBot(BOT_CONTEXT* ctx);
BOOL AbrirRecursosCompartilhadosBot(BOT_CONTEXT* ctx);
void LimparRecursosBot(BOT_CONTEXT* ctx);
void EnviarMensagemAoServidorBot(BOT_CONTEXT* ctx, const MESSAGE* msg);

void BotLoopPrincipal(BOT_CONTEXT* ctx);
BOOL TentarEncontrarEEnviarPalavra(BOT_CONTEXT* ctx);
BOOL PodeFormarPalavra(const TCHAR* palavra, const TCHAR* letrasDisponiveisNoTabuleiro, int numMaxLetrasNoTabuleiro);

DWORD WINAPI ThreadReceptorMensagensServidorBot(LPVOID param);


// --- Main Function ---
int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    srand((unsigned)time(NULL));

    BOT_CONTEXT botCtx; // Instance of the bot's context
    ZeroMemory(&botCtx, sizeof(BOT_CONTEXT));
    // Set default values that might not be set by ProcessarArgumentosBot or other initializers
    botCtx.hPipeServidorBot = INVALID_HANDLE_VALUE;
    botCtx.reactionTimeSeconds = 10; // Default if not set by args, though ProcessArgs should handle it
    botCtx.botRodando = TRUE;
    botCtx.botUltimaGeracaoConhecidaShm = -1;


    if (!ProcessarArgumentosBot(&botCtx, argc, argv)) {
        _tprintf(_T("Uso: bot.exe <username> <reaction_time_segundos>\n"));
        _tprintf(_T("Exemplo: bot.exe BotJogador 5\n"));
        return 1;
    }

    // Initialize critical sections AFTER they are part of botCtx
    InitializeCriticalSection(&botCtx.csBotConsole);
    InitializeCriticalSection(&botCtx.csBotData);

    LogBot(&botCtx, _T("Bot '%s' a iniciar com tempo de reação de %d segundos..."), botCtx.botUsername, botCtx.reactionTimeSeconds);

    if (!CarregarDicionarioBot(&botCtx, _T("..\\Comum\\dicionario.txt"))) {
        LogErrorBot(&botCtx, _T("Falha ao carregar dicionário do bot. Encerrando."));
        DeleteCriticalSection(&botCtx.csBotData);
        DeleteCriticalSection(&botCtx.csBotConsole);
        return 1;
    }

    if (!ConectarAoServidorBot(&botCtx)) {
        LogErrorBot(&botCtx, _T("Falha ao conectar ao servidor. Encerrando."));
        LimparRecursosBot(&botCtx);
        LiberarDicionarioBot(&botCtx);
        DeleteCriticalSection(&botCtx.csBotData);
        DeleteCriticalSection(&botCtx.csBotConsole);
        return 1;
    }

    if (!AbrirRecursosCompartilhadosBot(&botCtx)) {
        LogErrorBot(&botCtx, _T("Falha ao abrir recursos compartilhados. Encerrando."));
        LimparRecursosBot(&botCtx);
        LiberarDicionarioBot(&botCtx);
        DeleteCriticalSection(&botCtx.csBotData);
        DeleteCriticalSection(&botCtx.csBotConsole);
        return 1;
    }

    MESSAGE msgJoin;
    ZeroMemory(&msgJoin, sizeof(MESSAGE));
    _tcscpy_s(msgJoin.type, _countof(msgJoin.type), _T("JOIN"));
    _tcscpy_s(msgJoin.username, _countof(msgJoin.username), botCtx.botUsername);
    EnviarMensagemAoServidorBot(&botCtx, &msgJoin);

    botCtx.hThreadReceptorPipeBot = CreateThread(NULL, 0, ThreadReceptorMensagensServidorBot, &botCtx, 0, NULL);
    if (botCtx.hThreadReceptorPipeBot == NULL) {
        LogErrorBot(&botCtx, _T("Falha ao criar thread receptora de pipe: %lu. Encerrando."), GetLastError());
        botCtx.botRodando = FALSE;
        LimparRecursosBot(&botCtx);
        LiberarDicionarioBot(&botCtx);
        DeleteCriticalSection(&botCtx.csBotData);
        DeleteCriticalSection(&botCtx.csBotConsole);
        return 1;
    }

    LogBot(&botCtx, _T("Bot conectado e thread receptora iniciada. Entrando no loop principal..."));
    BotLoopPrincipal(&botCtx);

    LogBot(&botCtx, _T("Loop principal do bot terminado. Aguardando thread receptora..."));

    if (botCtx.hEventoShmUpdateBot) {
        SetEvent(botCtx.hEventoShmUpdateBot);
    }

    if (botCtx.hThreadReceptorPipeBot != NULL) {
        if (WaitForSingleObject(botCtx.hThreadReceptorPipeBot, 3000) == WAIT_TIMEOUT) {
            LogWarningBot(&botCtx, _T("Timeout ao aguardar thread receptora de pipe. Forçando terminação da thread."));
            TerminateThread(botCtx.hThreadReceptorPipeBot, 0);
        }
        CloseHandle(botCtx.hThreadReceptorPipeBot);
        botCtx.hThreadReceptorPipeBot = NULL;
    }

    LimparRecursosBot(&botCtx);
    LiberarDicionarioBot(&botCtx);
    LogBot(&botCtx, _T("Bot '%s' encerrado."), botCtx.botUsername);

    DeleteCriticalSection(&botCtx.csBotData);
    DeleteCriticalSection(&botCtx.csBotConsole);
    return 0;
}

// --- Logging Functions ---
void LogBot(BOT_CONTEXT* ctx, const TCHAR* format, ...) {
    if (ctx == NULL || ctx->csBotConsole.DebugInfo == NULL) return;

    EnterCriticalSection(&ctx->csBotConsole);
    TCHAR buffer[1024];
    TCHAR finalBuffer[1280];
    va_list args;
    va_start(args, format);
    _vstprintf_s(buffer, _countof(buffer), format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    _stprintf_s(finalBuffer, _countof(finalBuffer), _T("%02d:%02d:%02d.%03d [BOT-%s] %s\n"),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ctx->botUsername, buffer);
    _tprintf_s(finalBuffer);
    fflush(stdout);
    LeaveCriticalSection(&ctx->csBotConsole);
}

void LogErrorBot(BOT_CONTEXT* ctx, const TCHAR* format, ...) {
    if (ctx == NULL || ctx->csBotConsole.DebugInfo == NULL) return;

    EnterCriticalSection(&ctx->csBotConsole);
    TCHAR buffer[1024];
    TCHAR finalBuffer[1280];
    va_list args;
    va_start(args, format);
    _vstprintf_s(buffer, _countof(buffer), format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    _stprintf_s(finalBuffer, _countof(finalBuffer), _T("%02d:%02d:%02d.%03d [BOT-ERRO-%s] %s\n"),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ctx->botUsername, buffer);
    _tprintf_s(finalBuffer);
    fflush(stdout);
    LeaveCriticalSection(&ctx->csBotConsole);
}

void LogWarningBot(BOT_CONTEXT* ctx, const TCHAR* format, ...) {
    if (ctx == NULL || ctx->csBotConsole.DebugInfo == NULL) return;

    EnterCriticalSection(&ctx->csBotConsole);
    TCHAR buffer[1024];
    TCHAR finalBuffer[1280];
    va_list args;
    va_start(args, format);
    _vstprintf_s(buffer, _countof(buffer), format, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    _stprintf_s(finalBuffer, _countof(finalBuffer), _T("%02d:%02d:%02d.%03d [BOT-AVISO-%s] %s\n"),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ctx->botUsername, buffer);
    _tprintf_s(finalBuffer);
    fflush(stdout);
    LeaveCriticalSection(&ctx->csBotConsole);
}

// --- Initialization and Argument Processing ---
BOOL ProcessarArgumentosBot(BOT_CONTEXT* ctx, int argc, TCHAR* argv[]) {
    if (argc != 3) {
        return FALSE;
    }
    _tcscpy_s(ctx->botUsername, MAX_USERNAME, argv[1]);
    ctx->reactionTimeSeconds = _tstoi(argv[2]);

    if (ctx->reactionTimeSeconds <= 0) {
        _tprintf(_T("Tempo de reação inválido. Deve ser > 0.\n")); // Log before full context for logger is ready
        return FALSE;
    }
    return TRUE;
}

BOOL CarregarDicionarioBot(BOT_CONTEXT* ctx, const TCHAR* nomeArquivo) {
    FILE* arquivo;
    LogBot(ctx, _T("A carregar dicionário do bot de '%s'..."), nomeArquivo);

    if (_tfopen_s(&arquivo, nomeArquivo, _T("r, ccs=UTF-8")) != 0 || arquivo == NULL) {
        LogErrorBot(ctx, _T("Erro ao abrir ficheiro de dicionário do bot '%s'. Verifique se o arquivo existe."), nomeArquivo);
        return FALSE;
    }

    TCHAR linha[MAX_WORD + 2];
    ctx->totalPalavrasBotDicionario = 0;

    while (ctx->totalPalavrasBotDicionario < MAX_BOT_DICT_WORDS && _fgetts(linha, _countof(linha), arquivo)) {
        size_t len = _tcslen(linha);
        while (len > 0 && (linha[len - 1] == _T('\n') || linha[len - 1] == _T('\r'))) {
            linha[len - 1] = _T('\0');
            len--;
        }

        if (len == 0 || len > MAX_WORD) continue;

        ctx->botDicionario[ctx->totalPalavrasBotDicionario] = _tcsdup(linha);
        if (!ctx->botDicionario[ctx->totalPalavrasBotDicionario]) {
            LogErrorBot(ctx, _T("Falha ao alocar memória para palavra do dicionário: '%s'"), linha);
            fclose(arquivo);
            for (DWORD i = 0; i < ctx->totalPalavrasBotDicionario; i++) free(ctx->botDicionario[i]);
            ctx->totalPalavrasBotDicionario = 0;
            return FALSE;
        }
        _tcsupr_s(ctx->botDicionario[ctx->totalPalavrasBotDicionario], len + 1);
        ctx->totalPalavrasBotDicionario++;
    }

    fclose(arquivo);
    LogBot(ctx, _T("Dicionário do bot carregado com %lu palavras."), ctx->totalPalavrasBotDicionario);
    if (ctx->totalPalavrasBotDicionario == 0) {
        LogWarningBot(ctx, _T("O dicionário do bot está vazio! O bot não poderá jogar."));
    }
    return TRUE;
}

void LiberarDicionarioBot(BOT_CONTEXT* ctx) {
    for (DWORD i = 0; i < ctx->totalPalavrasBotDicionario; i++) {
        free(ctx->botDicionario[i]);
        ctx->botDicionario[i] = NULL;
    }
    ctx->totalPalavrasBotDicionario = 0;
    LogBot(ctx, _T("Dicionário do bot libertado."));
}

// --- IPC Client Functions ---
BOOL ConectarAoServidorBot(BOT_CONTEXT* ctx) {
    int tentativas = 0;
    const int MAX_TENTATIVAS_PIPE = 5;
    LogBot(ctx, _T("Tentando conectar ao pipe do servidor: %s"), PIPE_NAME);

    while (tentativas < MAX_TENTATIVAS_PIPE && ctx->botRodando) {
        ctx->hPipeServidorBot = CreateFile(
            PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

        if (ctx->hPipeServidorBot != INVALID_HANDLE_VALUE) {
            DWORD dwMode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(ctx->hPipeServidorBot, &dwMode, NULL, NULL)) {
                LogErrorBot(ctx, _T("Falha ao definir modo do pipe para mensagem: %lu"), GetLastError());
                CloseHandle(ctx->hPipeServidorBot);
                ctx->hPipeServidorBot = INVALID_HANDLE_VALUE;
                return FALSE;
            }
            LogBot(ctx, _T("Conectado ao servidor com sucesso (Pipe: %p)."), ctx->hPipeServidorBot);
            return TRUE;
        }

        DWORD dwError = GetLastError();
        if (dwError != ERROR_PIPE_BUSY && dwError != ERROR_FILE_NOT_FOUND) {
            LogErrorBot(ctx, _T("Erro não esperado ao conectar ao pipe: %lu"), dwError);
            return FALSE;
        }
        LogWarningBot(ctx, _T("Pipe ocupado ou não encontrado (tentativa %d/%d). Tentando novamente em 1s..."), tentativas + 1, MAX_TENTATIVAS_PIPE);
        Sleep(1000);
        tentativas++;
    }
    if (!ctx->botRodando) LogBot(ctx, _T("Conexão cancelada durante tentativas."));
    else LogErrorBot(ctx, _T("Não foi possível conectar ao servidor após %d tentativas."), MAX_TENTATIVAS_PIPE);
    return FALSE;
}

BOOL AbrirRecursosCompartilhadosBot(BOT_CONTEXT* ctx) {
    LogBot(ctx, _T("Tentando abrir recursos compartilhados..."));
    ctx->hMapFileShmBot = OpenFileMapping(FILE_MAP_READ, FALSE, SHM_NAME);
    if (ctx->hMapFileShmBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao abrir FileMapping '%s': %lu"), SHM_NAME, GetLastError());
        return FALSE;
    }
    ctx->pDadosShmBot = (DadosJogoCompartilhados*)MapViewOfFile(ctx->hMapFileShmBot, FILE_MAP_READ, 0, 0, sizeof(DadosJogoCompartilhados));
    if (ctx->pDadosShmBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao mapear SHM '%s': %lu"), SHM_NAME, GetLastError());
        CloseHandle(ctx->hMapFileShmBot); ctx->hMapFileShmBot = NULL;
        return FALSE;
    }
    ctx->hEventoShmUpdateBot = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EVENT_SHM_UPDATE);
    if (ctx->hEventoShmUpdateBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao abrir evento SHM '%s': %lu."), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(ctx->pDadosShmBot); ctx->pDadosShmBot = NULL;
        CloseHandle(ctx->hMapFileShmBot); ctx->hMapFileShmBot = NULL;
        return FALSE;
    }
    ctx->hMutexShmBot = OpenMutex(SYNCHRONIZE, FALSE, MUTEX_SHARED_MEM);
    if (ctx->hMutexShmBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao abrir mutex da SHM '%s': %lu. Isso é crítico para o bot."), MUTEX_SHARED_MEM, GetLastError());
        CloseHandle(ctx->hEventoShmUpdateBot); ctx->hEventoShmUpdateBot = NULL;
        UnmapViewOfFile(ctx->pDadosShmBot); ctx->pDadosShmBot = NULL;
        CloseHandle(ctx->hMapFileShmBot); ctx->hMapFileShmBot = NULL;
        return FALSE;
    }

    LogBot(ctx, _T("Recursos compartilhados abertos com sucesso."));
    if (ctx->pDadosShmBot) {
        WaitForSingleObject(ctx->hMutexShmBot, INFINITE);
        ctx->botUltimaGeracaoConhecidaShm = ctx->pDadosShmBot->generationCount;
        ReleaseMutex(ctx->hMutexShmBot);
    }
    return TRUE;
}

void LimparRecursosBot(BOT_CONTEXT* ctx) {
    LogBot(ctx, _T("Limpando recursos do bot..."));
    if (ctx->hPipeServidorBot != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->hPipeServidorBot);
        ctx->hPipeServidorBot = INVALID_HANDLE_VALUE;
    }
    if (ctx->pDadosShmBot != NULL) {
        UnmapViewOfFile(ctx->pDadosShmBot);
        ctx->pDadosShmBot = NULL;
    }
    if (ctx->hMapFileShmBot != NULL) {
        CloseHandle(ctx->hMapFileShmBot);
        ctx->hMapFileShmBot = NULL;
    }
    if (ctx->hEventoShmUpdateBot != NULL) {
        CloseHandle(ctx->hEventoShmUpdateBot);
        ctx->hEventoShmUpdateBot = NULL;
    }
    if (ctx->hMutexShmBot != NULL) {
        CloseHandle(ctx->hMutexShmBot);
        ctx->hMutexShmBot = NULL;
    }
    LogBot(ctx, _T("Recursos do bot limpos."));
}

void EnviarMensagemAoServidorBot(BOT_CONTEXT* ctx, const MESSAGE* msg) {
    if (ctx->hPipeServidorBot == INVALID_HANDLE_VALUE || !ctx->botRodando) {
        return;
    }
    DWORD bytesEscritos;
    OVERLAPPED ovWrite; ZeroMemory(&ovWrite, sizeof(OVERLAPPED));
    ovWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ovWrite.hEvent == NULL) {
        LogErrorBot(ctx, _T("Falha ao criar evento para WriteFile. Mensagem tipo '%s' não enviada."), msg->type);
        return;
    }

    if (!WriteFile(ctx->hPipeServidorBot, msg, sizeof(MESSAGE), &bytesEscritos, &ovWrite)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ovWrite.hEvent, 5000) == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(ctx->hPipeServidorBot, &ovWrite, &bytesEscritos, FALSE) || bytesEscritos != sizeof(MESSAGE)) {
                    if (ctx->botRodando) LogErrorBot(ctx, _T("GetOverlappedResult falhou ou bytes incorretos (%lu) para msg tipo '%s': %lu"), bytesEscritos, msg->type, GetLastError());
                }
            }
            else {
                if (ctx->botRodando) LogErrorBot(ctx, _T("Timeout ao enviar mensagem tipo '%s'. Cancelando IO."), msg->type);
                CancelIoEx(ctx->hPipeServidorBot, &ovWrite);
                if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) {
                    if (ctx->botRodando) LogErrorBot(ctx, _T("Pipe quebrou durante envio. Encerrando bot."));
                    ctx->botRodando = FALSE;
                }
            }
        }
        else {
            if (ctx->botRodando) LogErrorBot(ctx, _T("Falha ao enviar mensagem tipo '%s' para o servidor: %lu"), msg->type, GetLastError());
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) {
                if (ctx->botRodando) LogErrorBot(ctx, _T("Pipe quebrou. Encerrando bot."));
                ctx->botRodando = FALSE;
            }
        }
    }
    else {
        if (!GetOverlappedResult(ctx->hPipeServidorBot, &ovWrite, &bytesEscritos, FALSE) || bytesEscritos != sizeof(MESSAGE)) {
            if (ctx->botRodando) LogErrorBot(ctx, _T("WriteFile síncrono (com overlapped) falhou ou bytes incorretos (%lu) para msg tipo '%s': %lu"), bytesEscritos, msg->type, GetLastError());
        }
    }
    CloseHandle(ovWrite.hEvent);
}

// --- Bot Core Logic ---
void BotLoopPrincipal(BOT_CONTEXT* ctx) {
    DWORD dwTimeout = ctx->reactionTimeSeconds * 1000;

    while (ctx->botRodando) {
        DWORD accumulatedSleep = 0;
        while (accumulatedSleep < dwTimeout && ctx->botRodando) {
            Sleep(100);
            accumulatedSleep += 100;
        }

        if (!ctx->botRodando) break;

        if (ctx->pDadosShmBot == NULL || ctx->hMutexShmBot == NULL) {
            LogWarningBot(ctx, _T("SHM ou Mutex não disponível no loop principal. Aguardando..."));
            Sleep(1000);
            continue;
        }

        BOOL isGameCurrentlyActive = FALSE;
        WaitForSingleObject(ctx->hMutexShmBot, INFINITE);
        if (ctx->pDadosShmBot) {
            isGameCurrentlyActive = ctx->pDadosShmBot->jogoAtivo;
        }
        else {
            ReleaseMutex(ctx->hMutexShmBot);
            LogWarningBot(ctx, _T("pDadosShmBot é NULL após adquirir mutex."));
            Sleep(1000);
            continue;
        }
        ReleaseMutex(ctx->hMutexShmBot);

        if (isGameCurrentlyActive) {
            TentarEncontrarEEnviarPalavra(ctx);
        }
        else {
            LogBot(ctx, _T("Jogo não está ativo. Bot aguardando..."));
        }
    }
}

BOOL TentarEncontrarEEnviarPalavra(BOT_CONTEXT* ctx) {
    if (ctx->pDadosShmBot == NULL || ctx->hMutexShmBot == NULL || ctx->totalPalavrasBotDicionario == 0) {
        return FALSE;
    }

    TCHAR letrasAtuais[MAX_LETRAS_TABULEIRO + 1];
    int numLetrasNoTabuleiro;
    ZeroMemory(letrasAtuais, sizeof(letrasAtuais));

    WaitForSingleObject(ctx->hMutexShmBot, INFINITE);
    if (ctx->pDadosShmBot) {
        numLetrasNoTabuleiro = ctx->pDadosShmBot->numMaxLetrasAtual;
        for (int i = 0; i < numLetrasNoTabuleiro && i < MAX_LETRAS_TABULEIRO; ++i) {
            letrasAtuais[i] = ctx->pDadosShmBot->letrasVisiveis[i];
        }
        letrasAtuais[numLetrasNoTabuleiro < MAX_LETRAS_TABULEIRO ? numLetrasNoTabuleiro : MAX_LETRAS_TABULEIRO] = _T('\0');
    }
    else {
        ReleaseMutex(ctx->hMutexShmBot);
        return FALSE;
    }
    ReleaseMutex(ctx->hMutexShmBot);

    for (DWORD i = 0; i < ctx->totalPalavrasBotDicionario; ++i) {
        if (ctx->botDicionario[i] == NULL) continue;

        if (PodeFormarPalavra(ctx->botDicionario[i], letrasAtuais, numLetrasNoTabuleiro)) {
            MESSAGE msgPalavra;
            ZeroMemory(&msgPalavra, sizeof(MESSAGE));
            _tcscpy_s(msgPalavra.type, _countof(msgPalavra.type), _T("WORD"));
            _tcscpy_s(msgPalavra.username, _countof(msgPalavra.username), ctx->botUsername);
            _tcscpy_s(msgPalavra.data, _countof(msgPalavra.data), ctx->botDicionario[i]);

            EnviarMensagemAoServidorBot(ctx, &msgPalavra);
            LogBot(ctx, _T("Tentou a palavra: '%s'"), ctx->botDicionario[i]);
            return TRUE;
        }
    }
    LogBot(ctx, _T("Nenhuma palavra encontrada nesta rodada com as letras: [%s]"), letrasAtuais);
    return FALSE;
}

BOOL PodeFormarPalavra(const TCHAR* palavra, const TCHAR* letrasDisponiveisNoTabuleiro, int numMaxLetrasNoTabuleiro) {
    if (palavra == NULL || letrasDisponiveisNoTabuleiro == NULL || _tcslen(palavra) == 0) {
        return FALSE;
    }

    TCHAR copiaLetras[MAX_LETRAS_TABULEIRO + 1];
    ZeroMemory(copiaLetras, sizeof(copiaLetras));
    for (int i = 0; i < numMaxLetrasNoTabuleiro && i < MAX_LETRAS_TABULEIRO; ++i) {
        copiaLetras[i] = _totupper(letrasDisponiveisNoTabuleiro[i]);
    }
    copiaLetras[numMaxLetrasNoTabuleiro < MAX_LETRAS_TABULEIRO ? numMaxLetrasNoTabuleiro : MAX_LETRAS_TABULEIRO] = _T('\0');

    size_t lenPalavra = _tcslen(palavra);

    for (size_t i = 0; i < lenPalavra; ++i) {
        TCHAR charPalavra = _totupper(palavra[i]);
        BOOL encontrouChar = FALSE;
        for (int j = 0; j < numMaxLetrasNoTabuleiro; ++j) {
            if (copiaLetras[j] == charPalavra) {
                copiaLetras[j] = _T('_');
                encontrouChar = TRUE;
                break;
            }
        }
        if (!encontrouChar) {
            return FALSE;
        }
    }
    return TRUE;
}

// --- Threads ---
DWORD WINAPI ThreadReceptorMensagensServidorBot(LPVOID param) {
    BOT_CONTEXT* ctx = (BOT_CONTEXT*)param;
    MESSAGE msgDoServidor;
    DWORD bytesLidos;
    OVERLAPPED ovReadPipe; ZeroMemory(&ovReadPipe, sizeof(OVERLAPPED));
    ovReadPipe.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (ovReadPipe.hEvent == NULL) {
        LogErrorBot(ctx, _T("TRA: Falha ao criar evento de leitura do pipe. Encerrando thread receptora."));
        if (ctx) ctx->botRodando = FALSE;
        return 1;
    }
    LogBot(ctx, _T("TRA: Thread Receptora de Mensagens do Servidor iniciada."));

    while (ctx->botRodando) {
        ResetEvent(ovReadPipe.hEvent);
        BOOL sucessoLeitura = ReadFile(ctx->hPipeServidorBot, &msgDoServidor, sizeof(MESSAGE), &bytesLidos, &ovReadPipe);
        DWORD dwError = GetLastError();

        if (!sucessoLeitura && dwError == ERROR_IO_PENDING) {
            HANDLE handles[1] = { ovReadPipe.hEvent };
            DWORD waitRes = WaitForMultipleObjects(1, handles, FALSE, 500);

            if (!ctx->botRodando) break;

            if (waitRes == WAIT_TIMEOUT) {
                continue;
            }
            else if (waitRes != WAIT_OBJECT_0) {
                if (ctx->botRodando) LogErrorBot(ctx, _T("TRA: Erro %lu ao esperar ReadFile do pipe."), GetLastError());
                ctx->botRodando = FALSE; break;
            }
            if (!GetOverlappedResult(ctx->hPipeServidorBot, &ovReadPipe, &bytesLidos, FALSE)) {
                if (ctx->botRodando) LogErrorBot(ctx, _T("TRA: GetOverlappedResult falhou após ReadFile do pipe: %lu."), GetLastError());
                ctx->botRodando = FALSE; break;
            }
            sucessoLeitura = TRUE;
        }
        else if (!sucessoLeitura) {
            if (ctx->botRodando) {
                if (dwError == ERROR_BROKEN_PIPE) LogWarningBot(ctx, _T("TRA: Pipe quebrado (servidor desconectou?)."));
                else LogErrorBot(ctx, _T("TRA: ReadFile falhou imediatamente: %lu."), dwError);
            }
            ctx->botRodando = FALSE; break;
        }

        if (!ctx->botRodando) break;

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {
            LogBot(ctx, _T("TRA: Recebido do servidor: Tipo='%s', User='%s', Data='%s', Pts=%d"),
                msgDoServidor.type, msgDoServidor.username, msgDoServidor.data, msgDoServidor.pontos);

            if (_tcscmp(msgDoServidor.type, _T("SHUTDOWN")) == 0) {
                LogWarningBot(ctx, _T("TRA: Recebida mensagem SHUTDOWN do servidor. Encerrando bot..."));
                ctx->botRodando = FALSE;
            }
            else if (_tcscmp(msgDoServidor.type, _T("JOIN_OK")) == 0) {
                LogBot(ctx, _T("TRA: Bot juntou-se ao jogo com sucesso: %s"), msgDoServidor.data);
            }
            else if (_tcscmp(msgDoServidor.type, _T("JOIN_USER_EXISTS")) == 0 ||
                _tcscmp(msgDoServidor.type, _T("JOIN_GAME_FULL")) == 0) {
                LogErrorBot(ctx, _T("TRA: Falha ao juntar ao jogo: %s. Encerrando bot."), msgDoServidor.data);
                ctx->botRodando = FALSE;
            }
            else if (_tcscmp(msgDoServidor.type, _T("SCORE_UPDATE")) == 0) {
                if (_tcscmp(msgDoServidor.username, ctx->botUsername) == 0) {
                    EnterCriticalSection(&ctx->csBotData);
                    ctx->botScore = (float)msgDoServidor.pontos;
                    LeaveCriticalSection(&ctx->csBotData);
                    LogBot(ctx, _T("Minha pontuação atualizada para: %.1f"), ctx->botScore);
                }
            }
            else if (_tcscmp(msgDoServidor.type, _T("WORD_VALID")) == 0) {
                LogBot(ctx, _T("Palavra '%s' aceite! (+%d pontos)"), msgDoServidor.data, msgDoServidor.pontos);
                EnterCriticalSection(&ctx->csBotData);
                ctx->botScore += (float)msgDoServidor.pontos;
                LeaveCriticalSection(&ctx->csBotData);
                LogBot(ctx, _T("Minha pontuação agora: %.1f"), ctx->botScore);
            }
            else if (_tcscmp(msgDoServidor.type, _T("WORD_INVALID")) == 0) {
                LogBot(ctx, _T("Palavra '%s' inválida/rejeitada. (%d pontos)"), msgDoServidor.data, msgDoServidor.pontos);
                EnterCriticalSection(&ctx->csBotData);
                ctx->botScore += (float)msgDoServidor.pontos;
                LeaveCriticalSection(&ctx->csBotData);
                LogBot(ctx, _T("Minha pontuação agora: %.1f"), ctx->botScore);
            }
        }
        else if (sucessoLeitura && bytesLidos == 0) {
            if (ctx->botRodando) {
                LogWarningBot(ctx, _T("TRA: Servidor fechou a conexão (EOF). Encerrando bot."));
                ctx->botRodando = FALSE;
            }
            break;
        }
        else if (bytesLidos != 0) {
            if (ctx->botRodando) {
                LogErrorBot(ctx, _T("TRA: Mensagem incompleta/errada do servidor (%lu bytes). Encerrando bot."), bytesLidos);
                ctx->botRodando = FALSE;
            }
            break;
        }
    }

    if (ovReadPipe.hEvent) CloseHandle(ovReadPipe.hEvent);
    LogBot(ctx, _T("TRA: Thread Receptora de Mensagens do Servidor a terminar."));
    return 0;
}