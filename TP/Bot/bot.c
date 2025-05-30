#include "bot.h" 

#include <stdio.h>   
#include <stdlib.h>  
#include <fcntl.h>   
#include <io.h>      
#include <time.h>    

int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    srand((unsigned)time(NULL));

    BOT_CONTEXT botCtx;
    ZeroMemory(&botCtx, sizeof(BOT_CONTEXT));

    botCtx.hPipeServidorBot = INVALID_HANDLE_VALUE;
    botCtx.reactionTimeSeconds = 10;
    botCtx.botRodando = TRUE;
    botCtx.botUltimaGeracaoConhecidaShm = -1;


    if (!ProcessarArgumentosBot(&botCtx, argc, argv)) {
        _tprintf(_T("Uso: bot.exe <username> <reaction_time_segundos>\n"));
        _tprintf(_T("Exemplo: bot.exe BotJogador 5\n"));
        return 1;
    }


    InitializeCriticalSection(&botCtx.csBotConsole);
    InitializeCriticalSection(&botCtx.csBotData);

    LogBot(&botCtx, _T("Bot '%s' a iniciar com tempo de reação de %d segundos..."), botCtx.botUsername, botCtx.reactionTimeSeconds);

    if (!CarregarDicionarioBot(&botCtx, _T("..\\..\\Comum\\dicionario.txt"))) {
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

        if (botCtx.hPipeServidorBot != INVALID_HANDLE_VALUE) {

        }
        LimparRecursosBot(&botCtx);
        LiberarDicionarioBot(&botCtx);
        DeleteCriticalSection(&botCtx.csBotData);
        DeleteCriticalSection(&botCtx.csBotConsole);
        return 1;
    }

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


void LogBot(BOT_CONTEXT* ctx, const TCHAR* format, ...) {

    if (ctx == NULL || ctx->csBotConsole.DebugInfo == NULL) {

        return;
    }

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

BOOL ProcessarArgumentosBot(BOT_CONTEXT* ctx, int argc, TCHAR* argv[]) {
    if (argc != 3) {
        return FALSE;
    }
    _tcscpy_s(ctx->botUsername, MAX_USERNAME, argv[1]);
    ctx->reactionTimeSeconds = _tstoi(argv[2]);

    if (ctx->reactionTimeSeconds < 5 || ctx->reactionTimeSeconds > 30) {
        _tprintf(_T("Tempo de reação inválido. Deve ser entre 5 e 30.\n"));
        return FALSE;
    }
    return TRUE;
}

BOOL CarregarDicionarioBot(BOT_CONTEXT* ctx, const TCHAR* nomeArquivo) {
    FILE* arquivo;


    if (_tfopen_s(&arquivo, nomeArquivo, _T("rt, ccs=UTF-8")) != 0 || arquivo == NULL) {

        LogErrorBot(ctx, _T("Erro ao abrir ficheiro de dicionário do bot '%s'. Verifique se o arquivo existe e o caminho está correto."), nomeArquivo);
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

            for (DWORD i = 0; i < ctx->totalPalavrasBotDicionario; i++) {
                free(ctx->botDicionario[i]);
                ctx->botDicionario[i] = NULL;
            }
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

BOOL ConectarAoServidorBot(BOT_CONTEXT* ctx) {
    int tentativas = 0;
    const int MAX_TENTATIVAS_PIPE = 5;

    while (tentativas < MAX_TENTATIVAS_PIPE && ctx->botRodando) {
        ctx->hPipeServidorBot = CreateFile(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

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
    ctx->hMapFileShmBot = OpenFileMapping(
        FILE_MAP_READ,
        FALSE,
        SHM_NAME);

    if (ctx->hMapFileShmBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao abrir FileMapping '%s': %lu"), SHM_NAME, GetLastError());
        return FALSE;
    }

    ctx->pDadosShmBot = (DadosJogoCompartilhados*)MapViewOfFile(
        ctx->hMapFileShmBot,
        FILE_MAP_READ,
        0,
        0,
        sizeof(DadosJogoCompartilhados));

    if (ctx->pDadosShmBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao mapear SHM '%s': %lu"), SHM_NAME, GetLastError());
        CloseHandle(ctx->hMapFileShmBot); ctx->hMapFileShmBot = NULL;
        return FALSE;
    }

    ctx->hEventoShmUpdateBot = OpenEvent(
        EVENT_MODIFY_STATE | SYNCHRONIZE,
        FALSE,
        EVENT_SHM_UPDATE);

    if (ctx->hEventoShmUpdateBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao abrir evento SHM '%s': %lu."), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(ctx->pDadosShmBot); ctx->pDadosShmBot = NULL;
        CloseHandle(ctx->hMapFileShmBot); ctx->hMapFileShmBot = NULL;
        return FALSE;
    }

    ctx->hMutexShmBot = OpenMutex(
        SYNCHRONIZE,
        FALSE,
        MUTEX_SHARED_MEM);

    if (ctx->hMutexShmBot == NULL) {
        LogErrorBot(ctx, _T("Falha ao abrir mutex da SHM '%s': %lu. Isso é crítico para o bot."), MUTEX_SHARED_MEM, GetLastError());
        CloseHandle(ctx->hEventoShmUpdateBot); ctx->hEventoShmUpdateBot = NULL;
        UnmapViewOfFile(ctx->pDadosShmBot); ctx->pDadosShmBot = NULL;
        CloseHandle(ctx->hMapFileShmBot); ctx->hMapFileShmBot = NULL;
        return FALSE;
    }


    if (ctx->pDadosShmBot) {
        DWORD waitResult = WaitForSingleObject(ctx->hMutexShmBot, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            ctx->botUltimaGeracaoConhecidaShm = ctx->pDadosShmBot->generationCount;
            ReleaseMutex(ctx->hMutexShmBot);
        }
        else {
            LogErrorBot(ctx, _T("Falha ao adquirir mutex para ler geração inicial da SHM: %lu"), GetLastError());

        }
    }

    LogBot(ctx, _T("Recursos compartilhados abertos com sucesso."));
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
        LogWarningBot(ctx, _T("Tentativa de enviar msg '%s' com pipe inválido ou bot não rodando."), msg->type);
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

                if (!GetOverlappedResult(ctx->hPipeServidorBot, &ovWrite, &bytesEscritos, FALSE) || 
                    bytesEscritos != sizeof(MESSAGE)) {
                    if (ctx->botRodando) LogErrorBot(ctx, _T("GetOverlappedResult falhou ou bytes incorretos (%lu) para msg tipo '%s': %lu"), bytesEscritos, msg->type, GetLastError());
                }
                else {

                }
            }
            else {
                if (ctx->botRodando) LogErrorBot(ctx, _T("Timeout ao enviar mensagem tipo '%s'. Cancelando IO."), msg->type);
                CancelIoEx(ctx->hPipeServidorBot, &ovWrite);

                DWORD lastErr = GetLastError();
                if (lastErr == ERROR_BROKEN_PIPE || lastErr == ERROR_PIPE_NOT_CONNECTED || lastErr == ERROR_OPERATION_ABORTED) {
                    if (ctx->botRodando) LogErrorBot(ctx, _T("Pipe quebrou ou IO cancelado durante envio. Encerrando bot. Err: %lu"), lastErr);
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
        if (!GetOverlappedResult(ctx->hPipeServidorBot, &ovWrite, &bytesEscritos, FALSE) ||
            bytesEscritos != sizeof(MESSAGE)) {
            if (ctx->botRodando) LogErrorBot(ctx, _T("WriteFile síncrono (com overlapped) falhou ou bytes incorretos (%lu) para msg tipo '%s': %lu"), bytesEscritos, msg->type, GetLastError());
        }
        else {

        }
    }
    CloseHandle(ovWrite.hEvent);
}


void BotLoopPrincipal(BOT_CONTEXT* ctx) {
    DWORD dwTimeoutBase = ctx->reactionTimeSeconds * 1000;

    while (ctx->botRodando) {

        DWORD randomDelay = rand() % 501;
        DWORD currentTimeout = dwTimeoutBase + randomDelay;

        DWORD accumulatedSleep = 0;
        DWORD sleepInterval = 100;

        while (accumulatedSleep < currentTimeout && ctx->botRodando) {
            Sleep(sleepInterval);
            accumulatedSleep += sleepInterval;
        }

        if (!ctx->botRodando) break;

        if (ctx->pDadosShmBot == NULL || ctx->hMutexShmBot == NULL) {
            LogWarningBot(ctx, _T("SHM ou Mutex não disponível no loop principal. Aguardando..."));
            Sleep(1000);
            continue;
        }

        BOOL isGameCurrentlyActive = FALSE;
        BOOL shmReadSuccess = FALSE;
        DWORD waitResult = WaitForSingleObject(ctx->hMutexShmBot, 1000);

        if (waitResult == WAIT_OBJECT_0) {
            if (ctx->pDadosShmBot) { 
                isGameCurrentlyActive = ctx->pDadosShmBot->jogoAtivo;

                shmReadSuccess = TRUE;
            }
            else {
                LogWarningBot(ctx, _T("pDadosShmBot é NULL após adquirir mutex."));
            }
            ReleaseMutex(ctx->hMutexShmBot);
        }
        else if (waitResult == WAIT_TIMEOUT) {
            LogWarningBot(ctx, _T("Timeout ao adquirir mutex da SHM no loop principal."));

        }
        else {
            LogErrorBot(ctx, _T("Erro ao adquirir mutex da SHM no loop principal: %lu"), GetLastError());
            ctx->botRodando = FALSE;
            break;
        }

        if (!shmReadSuccess) {
            Sleep(1000);
            continue;
        }

        if (isGameCurrentlyActive){
            TentarEncontrarEEnviarPalavra(ctx);
        }
 
    }
}


BOOL TentarEncontrarEEnviarPalavra(BOT_CONTEXT* ctx) {
    if (ctx->pDadosShmBot == NULL || ctx->hMutexShmBot == NULL || ctx->totalPalavrasBotDicionario == 0) {
        if (ctx->totalPalavrasBotDicionario == 0) {

        }
        return FALSE;
    }

    TCHAR letrasAtuais[MAX_LETRAS_TABULEIRO + 1];
    int numLetrasNoTabuleiro;
    long shmGeneration;
    ZeroMemory(letrasAtuais, sizeof(letrasAtuais));

    DWORD waitResult = WaitForSingleObject(ctx->hMutexShmBot, 1000);
    if (waitResult == WAIT_OBJECT_0) {
        if (ctx->pDadosShmBot) {

            if (ctx->pDadosShmBot->generationCount == ctx->botUltimaGeracaoConhecidaShm && ctx->botUltimaGeracaoConhecidaShm != -1) {
                ReleaseMutex(ctx->hMutexShmBot);

                return FALSE;
            }
            ctx->botUltimaGeracaoConhecidaShm = ctx->pDadosShmBot->generationCount;
            shmGeneration = ctx->pDadosShmBot->generationCount; 

            numLetrasNoTabuleiro = ctx->pDadosShmBot->numMaxLetrasAtual;
            if (numLetrasNoTabuleiro > MAX_LETRAS_TABULEIRO) numLetrasNoTabuleiro = MAX_LETRAS_TABULEIRO; 

            for (int i = 0; i < numLetrasNoTabuleiro; ++i) {
                letrasAtuais[i] = ctx->pDadosShmBot->letrasVisiveis[i];
            }
            letrasAtuais[numLetrasNoTabuleiro] = _T('\0'); 
        }
        else {
            ReleaseMutex(ctx->hMutexShmBot);
            LogWarningBot(ctx, _T("pDadosShmBot é NULL em TentarEncontrarEEnviarPalavra após mutex."));
            return FALSE;
        }
        ReleaseMutex(ctx->hMutexShmBot);
    }
    else if (waitResult == WAIT_TIMEOUT) {
        LogWarningBot(ctx, _T("Timeout ao adquirir mutex da SHM para ler letras."));
        return FALSE;
    }
    else {
        LogErrorBot(ctx, _T("Erro ao adquirir mutex da SHM para ler letras: %lu"), GetLastError());
        ctx->botRodando = FALSE; 
        return FALSE;
    }

    if (_tcslen(letrasAtuais) == 0) {

        return FALSE;
    }



    DWORD startIndex = rand() % ctx->totalPalavrasBotDicionario;

    for (DWORD i = 0; i < ctx->totalPalavrasBotDicionario; ++i) {
        if (!ctx->botRodando) return FALSE; 

        DWORD currentIndex = (startIndex + i) % ctx->totalPalavrasBotDicionario;
        if (ctx->botDicionario[currentIndex] == NULL) continue;

        if (PodeFormarPalavra(ctx->botDicionario[currentIndex], letrasAtuais, numLetrasNoTabuleiro)) {
            MESSAGE msgPalavra;
            ZeroMemory(&msgPalavra, sizeof(MESSAGE));
            _tcscpy_s(msgPalavra.type, _countof(msgPalavra.type), _T("WORD"));
            _tcscpy_s(msgPalavra.username, _countof(msgPalavra.username), ctx->botUsername);
            _tcscpy_s(msgPalavra.data, _countof(msgPalavra.data), ctx->botDicionario[currentIndex]);

            LogBot(ctx, _T("Encontrou '%s' com letras [%s] (geração %ld). Tentando enviar."), ctx->botDicionario[currentIndex], letrasAtuais, shmGeneration);
            EnviarMensagemAoServidorBot(ctx, &msgPalavra);

            return TRUE;
        }
    }

    return FALSE;
}


BOOL PodeFormarPalavra(const TCHAR* palavra, const TCHAR* letrasDisponiveisNoTabuleiro, int numMaxLetrasNoTabuleiro) {
    if (palavra == NULL || letrasDisponiveisNoTabuleiro == NULL || _tcslen(palavra) == 0) {
        return FALSE;
    }

    if (_tcslen(palavra) > (size_t)numMaxLetrasNoTabuleiro) {
        return FALSE;
    }



    TCHAR copiaLetras[MAX_LETRAS_TABULEIRO + 1];
    ZeroMemory(copiaLetras, sizeof(copiaLetras));


    int lenTabuleiro = (int)_tcslen(letrasDisponiveisNoTabuleiro);
    int effectiveLen = min(lenTabuleiro, numMaxLetrasNoTabuleiro);
    effectiveLen = min(effectiveLen, MAX_LETRAS_TABULEIRO);

    for (int i = 0; i < effectiveLen; ++i) {
        copiaLetras[i] = _totupper(letrasDisponiveisNoTabuleiro[i]);
    }
    copiaLetras[effectiveLen] = _T('\0');

    size_t lenPalavra = _tcslen(palavra);

    for (size_t i = 0; i < lenPalavra; ++i) {
        TCHAR charPalavra = _totupper(palavra[i]);
        BOOL encontrouChar = FALSE;
        for (int j = 0; j < effectiveLen; ++j) {
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

    LogBot(ctx, _T("TRA: Thread receptora de mensagens iniciada."));

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
                if (ctx->botRodando) {
                    DWORD readError = GetLastError();
                    if (readError == ERROR_BROKEN_PIPE || readError == ERROR_PIPE_NOT_CONNECTED) {
                        LogWarningBot(ctx, _T("TRA: Pipe quebrou (GetOverlappedResult): %lu. Servidor pode ter fechado."), readError);
                    }
                    else {
                        LogErrorBot(ctx, _T("TRA: GetOverlappedResult falhou após ReadFile do pipe: %lu."), readError);
                    }
                }
                ctx->botRodando = FALSE; break;
            }

            sucessoLeitura = TRUE;
        }
        else if (!sucessoLeitura) {
            if (ctx->botRodando) {
                if (dwError == ERROR_BROKEN_PIPE) {
                    LogWarningBot(ctx, _T("TRA: Pipe quebrado (servidor desconectou?)."));
                }
                else {
                    LogErrorBot(ctx, _T("TRA: ReadFile falhou imediatamente: %lu."), dwError);
                }
            }
            ctx->botRodando = FALSE; break;
        }


        if (!ctx->botRodando) break;

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {


            if (_tcscmp(msgDoServidor.type, _T("SHUTDOWN")) == 0) {
                LogWarningBot(ctx, _T("TRA: Recebida mensagem SHUTDOWN do servidor. Encerrando bot..."));
                ctx->botRodando = FALSE; 
            }
            else if (_tcscmp(msgDoServidor.type, _T("JOIN_OK")) == 0) {
                LogBot(ctx, _T("TRA: Bot '%s' juntou-se ao jogo com sucesso: %s"), ctx->botUsername, msgDoServidor.data);
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
                    LogBot(ctx, _T("Minha pontuação total atualizada para: %.1f"), ctx->botScore);
                }
            }
            else if (_tcscmp(msgDoServidor.type, _T("WORD_VALID")) == 0) {
                if (_tcscmp(msgDoServidor.username, ctx->botUsername) == 0) { 
                    LogBot(ctx, _T("Minha palavra '%s' foi aceite! (+%d pontos)"), msgDoServidor.data, msgDoServidor.pontos);
                    EnterCriticalSection(&ctx->csBotData);
                    ctx->botScore += (float)msgDoServidor.pontos;
                    LeaveCriticalSection(&ctx->csBotData);
                    LogBot(ctx, _T("Minha pontuação agora: %.1f"), ctx->botScore);
                }
                else {

                }
            }
            else if (_tcscmp(msgDoServidor.type, _T("WORD_INVALID")) == 0) {
                if (_tcscmp(msgDoServidor.username, ctx->botUsername) == 0) { 
                    LogBot(ctx, _T("Minha palavra '%s' foi inválida/rejeitada. (%d pontos)"), msgDoServidor.data, msgDoServidor.pontos);
                    EnterCriticalSection(&ctx->csBotData);
                    ctx->botScore += (float)msgDoServidor.pontos; 
                    LeaveCriticalSection(&ctx->csBotData);
                    LogBot(ctx, _T("Minha pontuação agora: %.1f"), ctx->botScore);
                }
                else {

                }
            }

            else if (_tcscmp(msgDoServidor.type, _T("NEW_ROUND_INFO")) == 0 || _tcscmp(msgDoServidor.type, _T("GAME_UPDATE")) == 0) {
                LogBot(ctx, _T("TRA: Informação de rodada/jogo recebida: %s. Bot vai reavaliar."), msgDoServidor.data);

            }
            else {
                LogBot(ctx, _T("TRA: Recebida msg não tratada do servidor: Tipo='%s', Data='%s'"), msgDoServidor.type, msgDoServidor.data);
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
                LogErrorBot(ctx, _T("TRA: Mensagem incompleta/errada do servidor (%lu bytes, esperava %lu). Encerrando bot."), bytesLidos, (DWORD)sizeof(MESSAGE));
                ctx->botRodando = FALSE;
            }
            break;
        }

    }

    if (ovReadPipe.hEvent) CloseHandle(ovReadPipe.hEvent);
    LogBot(ctx, _T("TRA: Thread receptora de mensagens terminada."));
    return 0;
}