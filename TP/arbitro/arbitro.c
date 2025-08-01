﻿#include "arbitro.h"

// Função principal
int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    HANDLE hMutexArbitroSingle = CreateMutex(NULL, TRUE, MUTEX_ARBITRO_SINGLE);

    if (hMutexArbitroSingle == NULL) {
        _tprintf(_T("Erro ao criar Mutex do árbitro: %lu\n"), GetLastError());
        return 1;
    }

    // Verifica se o Mutex já existia (outra instância já está a correr)
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        _tprintf(_T("Outra instância do Árbitro já está a correr. Encerrando esta instância.\n"));
        CloseHandle(hMutexArbitroSingle);
        return 0;
    }

    SERVER_CONTEXT serverCtx;
    ZeroMemory(&serverCtx, sizeof(SERVER_CONTEXT));

    InitializeCriticalSection(&serverCtx.csLog);

    Log(&serverCtx.csLog, _T("[ARBITRO] Iniciando árbitro..."));

    if (!InicializarServidor(&serverCtx)) {
        LogError(&serverCtx.csLog, _T("[ARBITRO] Falha ao inicializar o servidor. Encerrando."));
        ReleaseMutex(hMutexArbitroSingle);
        CloseHandle(hMutexArbitroSingle);
        EncerrarServidor(&serverCtx);
        DeleteCriticalSection(&serverCtx.csLog);
        return 1;
    }

    HANDLE hThreads[2] = { NULL, NULL };
    THREAD_ARGS argsServico;
    argsServico.serverCtx = &serverCtx;
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
                    argsCliente->serverCtx = &serverCtx;
                    argsCliente->hPipeCliente = hPipe;

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
                LogWarning(&serverCtx.csLog, _T("[ARBITRO] Jogo cheio. Rejeitando conexão no pipe %p."), hPipe);
                MESSAGE msgCheio; ZeroMemory(&msgCheio, sizeof(MESSAGE));
                _tcscpy_s(msgCheio.type, _countof(msgCheio.type), _T("JOIN_GAME_FULL"));
                _tcscpy_s(msgCheio.username, _countof(msgCheio.username), _T("Arbitro"));
                _tcscpy_s(msgCheio.data, _countof(msgCheio.data), _T("O jogo está cheio."));
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
    if (hThreads[0] != NULL) { WaitForSingleObject(hThreads[0], INFINITE); CloseHandle(hThreads[0]); }
    if (hThreads[1] != NULL) { WaitForSingleObject(hThreads[1], INFINITE); CloseHandle(hThreads[1]); }

    ReleaseMutex(hMutexArbitroSingle);
    CloseHandle(hMutexArbitroSingle);
    EncerrarServidor(&serverCtx);
    Log(&serverCtx.csLog, _T("[ARBITRO] Servidor encerrado."));
    DeleteCriticalSection(&serverCtx.csLog);
    return 0;
}

// Inicialização e Encerramento do Servidor
BOOL InicializarServidor(SERVER_CONTEXT* ctx) {
    srand((unsigned)time(NULL));

    ConfigurarValoresRegistry(ctx);
    Log(&ctx->csLog, _T("[INIT] Configurações: MAXLETRAS=%d, RITMO=%ds"), ctx->maxLetrasConfig, ctx->ritmoConfigSegundos);

    if (!CarregarDicionarioServidor(ctx, _T("..\\..\\Comum\\dicionario.txt"))) {
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
    Log(&ctx->csLog, _T("[ENCERRAR] Estado servidorEmExecucao: %s"),
        ctx->servidorEmExecucao ? _T("TRUE") : _T("FALSE"));

    static BOOL jaExecutado = FALSE;
    if (jaExecutado) {
        Log(&ctx->csLog, _T("[ENCERRAR] Encerramento já foi executado anteriormente."));
        return;
    }
    jaExecutado = TRUE;


    ctx->servidorEmExecucao = FALSE;

    TCHAR vencedorUsername[MAX_USERNAME] = _T("");
    float maiorPontuacao = -1000.0f;
    BOOL empate = FALSE;
    BOOL jogadoresEncontrados = FALSE;

    Log(&ctx->csLog, _T("[ENCERRAR] Determinando vencedor..."));

    EnterCriticalSection(&ctx->csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo) {
            jogadoresEncontrados = TRUE;
            Log(&ctx->csLog, _T("[ENCERRAR] Jogador %s com %.1f pontos"),
                ctx->listaJogadores[i].username, ctx->listaJogadores[i].pontos);

            if (ctx->listaJogadores[i].pontos > maiorPontuacao) {
                maiorPontuacao = ctx->listaJogadores[i].pontos;
                _tcscpy_s(vencedorUsername, _countof(vencedorUsername), ctx->listaJogadores[i].username);
                empate = FALSE;
            }
            else if (ctx->listaJogadores[i].pontos == maiorPontuacao) {
                empate = TRUE;
            }
        }
    }
    LeaveCriticalSection(&ctx->csListaJogadores);

    // Notificar o resultado do jogo para os jogadores
    MESSAGE msgVencedor;
    ZeroMemory(&msgVencedor, sizeof(MESSAGE));
    _tcscpy_s(msgVencedor.type, _countof(msgVencedor.type), _T("GAME_WINNER"));
    _tcscpy_s(msgVencedor.username, _countof(msgVencedor.username), _T("Arbitro"));

    if (jogadoresEncontrados) {
        if (!empate && vencedorUsername[0] != _T('\0')) {
            _sntprintf_s(msgVencedor.data, _countof(msgVencedor.data), _TRUNCATE,
                _T("O jogo terminou! O vencedor é %s com %.1f pontos!"), vencedorUsername, maiorPontuacao);
            Log(&ctx->csLog, _T("[ENCERRAR] Vencedor: %s com %.1f pontos."), vencedorUsername, maiorPontuacao);
        }
        else if (empate) {
            _sntprintf_s(msgVencedor.data, _countof(msgVencedor.data), _TRUNCATE,
                _T("O jogo terminou! Houve um empate com %.1f pontos!"), maiorPontuacao);
            Log(&ctx->csLog, _T("[ENCERRAR] Empate com %.1f pontos."), maiorPontuacao);
        }
        else {
            _tcscpy_s(msgVencedor.data, _countof(msgVencedor.data),
                _T("O jogo terminou! Erro na determinação do vencedor."));
            Log(&ctx->csLog, _T("[ENCERRAR] Erro na determinação do vencedor."));
        }
    }
    else {
        _tcscpy_s(msgVencedor.data, _countof(msgVencedor.data),
            _T("O jogo terminou! Nenhum jogador ativo encontrado."));
        Log(&ctx->csLog, _T("[ENCERRAR] Nenhum jogador ativo encontrado."));
    }


    Log(&ctx->csLog, _T("[ENCERRAR] Enviando resultado do jogo..."));
    NotificarTodosOsJogadores(ctx, &msgVencedor, NULL);

    // Aguardar para garantir que a mensagem seja processada
    Sleep(2000);

    MESSAGE msgShutdown;
    ZeroMemory(&msgShutdown, sizeof(MESSAGE));
    _tcscpy_s(msgShutdown.type, _countof(msgShutdown.type), _T("SHUTDOWN"));
    _tcscpy_s(msgShutdown.username, _countof(msgShutdown.username), _T("Arbitro"));
    _tcscpy_s(msgShutdown.data, _countof(msgShutdown.data), _T("O servidor está a encerrar."));

    Log(&ctx->csLog, _T("[ENCERRAR] Enviando mensagem de shutdown..."));
    NotificarTodosOsJogadores(ctx, &msgShutdown, NULL);

    if (ctx->hEventoShmUpdate) SetEvent(ctx->hEventoShmUpdate);

    // Tentar conectar consigo mesmo para acordar threads bloqueadas
    HANDLE hSelfConnect = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hSelfConnect != INVALID_HANDLE_VALUE) {
        CloseHandle(hSelfConnect);
    }

    Log(&ctx->csLog, _T("[ENCERRAR] Aguardando threads de cliente terminarem... (3s)"));
    Sleep(3000);

    // Limpar recursos dos jogadores
    if (ctx->csListaJogadores.DebugInfo != NULL) {
        EnterCriticalSection(&ctx->csListaJogadores);
        for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
            if (ctx->listaJogadores[i].hPipe != INVALID_HANDLE_VALUE) {
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
        LogWarning(&ctx->csLog, _T("[ENCERRAR] Seção crítica da lista de jogadores não inicializada ou já deletada."));
    }

    LimparMemoriaPartilhadaArbitro(ctx);
    LiberarDicionarioServidor(ctx);

    Log(&ctx->csLog, _T("[ENCERRAR] Recursos principais do servidor liberados."));
}

// Funções de Log
void Log(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...) {
    if (csLogParam == NULL || csLogParam->DebugInfo == NULL) {
        TCHAR fallbackBuffer[1024];
        va_list fbArgs;
        va_start(fbArgs, format);
        _vsntprintf_s(fallbackBuffer, _countof(fallbackBuffer), _TRUNCATE, format, fbArgs);
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

    _sntprintf_s(buffer, _countof(buffer), _TRUNCATE, _T("%02d:%02d:%02d.%03d "), st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    size_t prefixLen = _tcslen(buffer);
    if (prefixLen < _countof(buffer) - 1) {
        _vsntprintf_s(buffer + prefixLen, _countof(buffer) - prefixLen, _TRUNCATE, format, args);
    }
    _tcscat_s(buffer, _countof(buffer), _T("\n"));

    _tprintf_s(buffer);
    fflush(stdout);
    va_end(args);
    LeaveCriticalSection(csLogParam);
}

void LogError(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    _vsntprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    Log(csLogParam, _T("[ERRO] %s"), buffer);
}

void LogWarning(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    _vsntprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    Log(csLogParam, _T("[AVISO] %s"), buffer);
}

// Funções Auxiliares e Threads
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
                ctx->maxLetrasConfig = MAX_LETRAS_TABULEIRO;
                RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)&ctx->maxLetrasConfig, sizeof(DWORD));
            }
        }
        else {
            LogWarning(&ctx->csLog, _T("[REG] Não leu MAXLETRAS. Usando padrão %d e criando/atualizando."), ctx->maxLetrasConfig);
            RegSetValueEx(hKey, REG_MAXLETRAS_NOME, 0, REG_DWORD, (const BYTE*)&ctx->maxLetrasConfig, sizeof(DWORD));
        }
        dwSize = sizeof(DWORD);
        if (RegQueryValueEx(hKey, REG_RITMO_NOME, NULL, NULL, (LPBYTE)&dwValor, &dwSize) == ERROR_SUCCESS) {
            if (dwValor > 0 && dwValor < 300) {
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
    if (ctx->maxLetrasConfig > MAX_LETRAS_TABULEIRO)
        ctx->maxLetrasConfig = MAX_LETRAS_TABULEIRO;
    if (ctx->maxLetrasConfig <= 0)
        ctx->maxLetrasConfig = DEFAULT_MAXLETRAS;
    if (ctx->ritmoConfigSegundos <= 0)
        ctx->ritmoConfigSegundos = DEFAULT_RITMO_SEGUNDOS;
}

//Carregar e Liberar dicionário
BOOL CarregarDicionarioServidor(SERVER_CONTEXT* ctx, const TCHAR* nomeArquivo) {
    DICIONARIO_ARBITRO* dict = &ctx->dicionario;

    InitializeCriticalSection(&dict->csDicionario);
    dict->palavras = NULL;
    dict->totalPalavras = 0;
    FILE* arquivo;

    if (_tfopen_s(&arquivo, nomeArquivo, _T("r, ccs=UTF-8")) != 0 || !arquivo) {
        LogError(&ctx->csLog, _T("[DIC] Erro ao abrir ficheiro de dicionário '%s'."), nomeArquivo);
        DeleteCriticalSection(&dict->csDicionario);
        return FALSE;
    }

    TCHAR linha[MAX_WORD + 2];
    DWORD capacidade = 200;
    dict->palavras = (TCHAR**)malloc(capacidade * sizeof(TCHAR*));
    if (!dict->palavras) {
        fclose(arquivo);
        LogError(&ctx->csLog, _T("[DIC] Falha ao alocar memória inicial para dicionário."));
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
    Log(&ctx->csLog, _T("[DIC] Dicionário '%s' carregado com %lu palavras."), nomeArquivo, dict->totalPalavras);
    if (dict->totalPalavras == 0) {
        LogWarning(&ctx->csLog, _T("[DIC] Dicionário carregado está vazio!"));
    }
    return TRUE;
}

void LiberarDicionarioServidor(SERVER_CONTEXT* ctx) {
    DICIONARIO_ARBITRO* dict = &ctx->dicionario;
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

//Inicializar e Limpar memória partilhada (SHM)
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

    ctx->hEventoShmUpdate = CreateEvent(NULL, TRUE, FALSE, EVENT_SHM_UPDATE);
    if (ctx->hEventoShmUpdate == NULL) {
        LogError(&ctx->csLog, _T("[SHM] Erro ao criar evento SHM (%s): %lu"), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(ctx->pDadosShm); ctx->pDadosShm = NULL;
        CloseHandle(ctx->hMapFileShm); ctx->hMapFileShm = NULL;
        return FALSE;
    }

    ctx->hMutexShm = CreateMutex(NULL, FALSE, MUTEX_SHARED_MEM);
    if (ctx->hMutexShm == NULL) {
        LogError(&ctx->csLog, _T("[SHM] Erro ao criar mutex SHM (%s): %lu"), MUTEX_SHARED_MEM, GetLastError());
        CloseHandle(ctx->hEventoShmUpdate); ctx->hEventoShmUpdate = NULL;
        UnmapViewOfFile(ctx->pDadosShm); ctx->pDadosShm = NULL;
        CloseHandle(ctx->hMapFileShm); ctx->hMapFileShm = NULL;
        return FALSE;
    }

    WaitForSingleObject(ctx->hMutexShm, INFINITE);
    ctx->pDadosShm->numMaxLetrasAtual = maxLetras;
    for (int i = 0; i < MAX_LETRAS_TABULEIRO; i++) {
        ctx->pDadosShm->letrasVisiveis[i] = (i < maxLetras) ? _T('_') : _T('\0');
    }
    _tcscpy_s(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
    _tcscpy_s(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
    ctx->pDadosShm->pontuacaoUltimaPalavra = 0;
    ctx->pDadosShm->generationCount = 0;
    ctx->pDadosShm->jogoAtivo = FALSE;
    ReleaseMutex(ctx->hMutexShm);
    SetEvent(ctx->hEventoShmUpdate);

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

//-------------------- THREADS --------------------
DWORD WINAPI ThreadGestorLetras(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    SERVER_CONTEXT* ctx = args->serverCtx;
    int indiceLetraAntiga = 0;
    Log(&ctx->csLog, _T("[LETRAS] ThreadGestorLetras iniciada."));

    while (ctx->servidorEmExecucao) {
        int ritmoAtualSegundos = ctx->ritmoConfigSegundos;

        for (int i = 0; i < ritmoAtualSegundos * 10 && ctx->servidorEmExecucao; ++i) {
            Sleep(100);
        }
        if (!ctx->servidorEmExecucao) break;

        if (!ctx->jogoRealmenteAtivo) {
            continue;
        }

        WaitForSingleObject(ctx->hMutexShm, INFINITE);
        if (ctx->pDadosShm) {
            int maxLetras = ctx->pDadosShm->numMaxLetrasAtual;
            int posParaNovaLetra = -1;
            int letrasAtuaisNoTabuleiro = 0;

            for (int i = 0; i < maxLetras; i++) {
                if (ctx->pDadosShm->letrasVisiveis[i] != _T('_')) {
                    letrasAtuaisNoTabuleiro++;
                }
                else if (posParaNovaLetra == -1) {
                    posParaNovaLetra = i;
                }
            }

            if (posParaNovaLetra == -1 && letrasAtuaisNoTabuleiro >= maxLetras && maxLetras > 0) {
                posParaNovaLetra = indiceLetraAntiga;
                indiceLetraAntiga = (indiceLetraAntiga + 1) % maxLetras;
            }
            else if (posParaNovaLetra == -1 && letrasAtuaisNoTabuleiro < maxLetras && maxLetras > 0) {
                for (int i = 0; i < maxLetras; ++i) { if (ctx->pDadosShm->letrasVisiveis[i] == _T('_')) { posParaNovaLetra = i; break; } }
                if (posParaNovaLetra == -1 && maxLetras > 0) posParaNovaLetra = 0;
            }

            if (posParaNovaLetra != -1 && maxLetras > 0) {
                TCHAR novaLetra = _T('A') + (rand() % 26);
                ctx->pDadosShm->letrasVisiveis[posParaNovaLetra] = novaLetra;
                ctx->pDadosShm->generationCount++;
                SetEvent(ctx->hEventoShmUpdate);
            }
        }
        ReleaseMutex(ctx->hMutexShm);
    }
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
            if (ctx->servidorEmExecucao) {
                if (feof(stdin)) LogWarning(&ctx->csLog, _T("[ADMIN] EOF no stdin. Encerrando thread admin."));
                else LogError(&ctx->csLog, _T("[ADMIN] Erro ao ler comando do admin."));
            }
            break;
        }
        comando[_tcscspn(comando, _T("\r\n"))] = _T('\0');

        if (_tcslen(comando) == 0) continue;

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
        else if (_tcsnicmp(comando, _T("iniciarbot"), 10) == 0) {
            TCHAR botUsername[MAX_USERNAME];
            TCHAR botReactionTimeStr[10];
            int botReactionTime;

            Log(&ctx->csLog, _T("[ADMIN] Comando: iniciarbot"));
            Log(&ctx->csLog, _T("Admin> Digite o nome para o novo bot: "));
            fflush(stdout);
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
            if (EncontrarJogador(ctx, botUsername) != -1) {
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[ADMIN] Username '%s' já está em uso."), botUsername);
                continue;
            }
            LeaveCriticalSection(&ctx->csListaJogadores);

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
            TCHAR commandLine[MAX_PATH + MAX_USERNAME + 20];
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));

            _sntprintf_s(commandLine, _countof(commandLine), _TRUNCATE, _T("bot.exe %s %d"), botUsername, botReactionTime);

            Log(&ctx->csLog, _T("[ADMIN] Tentando iniciar bot com comando: %s"), commandLine);

            if (!CreateProcess(NULL, commandLine, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
                LogError(&ctx->csLog, _T("[ADMIN] Falha ao criar processo do bot (%s): %lu. Verifique se bot.exe está no PATH ou no mesmo diretório."), botUsername, GetLastError());
            }
            else {
                Log(&ctx->csLog, _T("[ADMIN] Processo do bot '%s' iniciado com PID %lu. O bot deverá conectar-se em breve."), botUsername, pi.dwProcessId);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }
        else if (_tcsncmp(comando, _T("excluir "), 8) == 0) {
            ZeroMemory(usernameParam, sizeof(usernameParam));
            if (_stscanf_s(comando, _T("excluir %31s"), usernameParam, (unsigned)_countof(usernameParam) - 1) == 1) {
                usernameParam[MAX_USERNAME - 1] = _T('\0');
                Log(&ctx->csLog, _T("[ADMIN] Comando: excluir %s"), usernameParam);
                RemoverJogador(ctx, usernameParam, TRUE);
            }
            else {
                LogWarning(&ctx->csLog, _T("[ADMIN] Comando excluir mal formatado. Uso: excluir <username>"));
            }
        }
        else if (_tcscmp(comando, _T("acelerar")) == 0) {
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
            ctx->servidorEmExecucao = FALSE;
            if (ctx->hEventoShmUpdate) SetEvent(ctx->hEventoShmUpdate);
            HANDLE hSelfConnect = CreateFile(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hSelfConnect != INVALID_HANDLE_VALUE) {
                CloseHandle(hSelfConnect);
            }
            break;
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
    return 0;
}

DWORD WINAPI ThreadClienteConectado(LPVOID param) {
    THREAD_ARGS* args = (THREAD_ARGS*)param;
    SERVER_CONTEXT* ctx = args->serverCtx;
    HANDLE hPipe = args->hPipeCliente;

    MESSAGE msg;
    DWORD bytesLidos, bytesEscritos;
    JOGADOR_INFO_ARBITRO* meuJogadorInfo = NULL;
    TCHAR usernameEsteCliente[MAX_USERNAME] = { 0 };
    BOOL esteClienteAtivoNaThread = FALSE;
    int meuIndiceNaLista = -1;
    BOOL continuar = TRUE;

    OVERLAPPED olRead = { 0 };
    olRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (olRead.hEvent == NULL) {
        LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Falha ao criar evento de leitura."), hPipe);
        continuar = FALSE;
    }

    Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Nova thread para cliente iniciada."), hPipe);

    // Processar mensagem JOIN inicial
    if (continuar) {
        ResetEvent(olRead.hEvent);
        if (!ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesLidos, &olRead) && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(olRead.hEvent, IO_TIMEOUT) != WAIT_OBJECT_0) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Timeout/Erro (%lu) ReadFile JOIN."), hPipe, GetLastError());
                continuar = FALSE;
            }
            else if (!GetOverlappedResult(hPipe, &olRead, &bytesLidos, FALSE)) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] GetOverlappedResult JOIN falhou (%lu)."), hPipe, GetLastError());
                continuar = FALSE;
            }
        }

        if (continuar && bytesLidos == sizeof(MESSAGE) && _tcscmp(msg.type, _T("JOIN")) == 0) {
            _tcscpy_s(usernameEsteCliente, _countof(usernameEsteCliente), msg.username);
            Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Recebido JOIN de '%s'."), hPipe, usernameEsteCliente);

            EnterCriticalSection(&ctx->csListaJogadores);
            int idxExistente = EncontrarJogador(ctx, usernameEsteCliente);
            if (idxExistente != -1) {
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Username '%s' já em uso."), hPipe, usernameEsteCliente);
                MESSAGE resp = { 0 };
                _tcscpy_s(resp.type, _countof(resp.type), _T("JOIN_USER_EXISTS"));
                _tcscpy_s(resp.data, _countof(resp.data), _T("Username em uso."));
                WriteFile(hPipe, &resp, sizeof(MESSAGE), &bytesEscritos, NULL);
                continuar = FALSE;
            }
            else if (ctx->totalJogadoresAtivos >= MAX_JOGADORES) {
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Jogo cheio. Rejeitando '%s'."), hPipe, usernameEsteCliente);
                MESSAGE resp = { 0 };
                _tcscpy_s(resp.type, _countof(resp.type), _T("JOIN_GAME_FULL"));
                _tcscpy_s(resp.data, _countof(resp.data), _T("Jogo cheio."));
                WriteFile(hPipe, &resp, sizeof(MESSAGE), &bytesEscritos, NULL);
                continuar = FALSE;
            }
            else {
                for (int i = 0; i < MAX_JOGADORES; ++i) {
                    if (!ctx->listaJogadores[i].ativo) {
                        meuIndiceNaLista = i;
                        meuJogadorInfo = &ctx->listaJogadores[i];
                        _tcscpy_s(meuJogadorInfo->username, _countof(meuJogadorInfo->username), usernameEsteCliente);
                        meuJogadorInfo->pontos = 0.0f;
                        meuJogadorInfo->hPipe = hPipe;
                        meuJogadorInfo->ativo = TRUE;
                        meuJogadorInfo->dwThreadIdCliente = GetCurrentThreadId();
                        ctx->totalJogadoresAtivos++;
                        esteClienteAtivoNaThread = TRUE;

                        Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Jogador '%s' adicionado (idx %d). Total: %lu"), hPipe, usernameEsteCliente, meuIndiceNaLista, ctx->totalJogadoresAtivos);

                        MESSAGE respOK = { 0 };
                        _tcscpy_s(respOK.type, _countof(respOK.type), _T("JOIN_OK"));
                        _sntprintf_s(respOK.data, _countof(respOK.data), _TRUNCATE, _T("Bem-vindo, %s!"), usernameEsteCliente);
                        WriteFile(hPipe, &respOK, sizeof(MESSAGE), &bytesEscritos, NULL);

                        MESSAGE notifJoin = { 0 };
                        _tcscpy_s(notifJoin.type, _countof(notifJoin.type), _T("GAME_UPDATE"));
                        _sntprintf_s(notifJoin.data, _countof(notifJoin.data), _TRUNCATE, _T("Jogador %s entrou no jogo."), usernameEsteCliente);
                        NotificarTodosOsJogadores(ctx, &notifJoin, usernameEsteCliente);

                        VerificarEstadoJogo(ctx);
                        break;
                    }
                }
                if (!esteClienteAtivoNaThread) {
                    LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Falha ao adicionar '%s' (sem slot disponível?)."), hPipe, usernameEsteCliente);
                    continuar = FALSE;
                }
                LeaveCriticalSection(&ctx->csListaJogadores);
            }
        }
        else {
            LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Primeira msg não foi JOIN ou bytes errados (%lu). Desconectando."), hPipe, bytesLidos);
            continuar = FALSE;
        }
    }

    // Loop principal de processamento de mensagens
    while (continuar && ctx->servidorEmExecucao && esteClienteAtivoNaThread) {
        ResetEvent(olRead.hEvent);
        BOOL sucessoLeitura = ReadFile(hPipe, &msg, sizeof(MESSAGE), &bytesLidos, &olRead);
        DWORD dwErrorRead = GetLastError();

        if (!sucessoLeitura && dwErrorRead == ERROR_IO_PENDING) {
            HANDLE handles[1] = { olRead.hEvent };
            DWORD waitRes = WaitForMultipleObjects(1, handles, FALSE, READ_TIMEOUT_THREAD_JOGADOR);

            if (!ctx->servidorEmExecucao) break;

            if (waitRes == WAIT_TIMEOUT) {
                DWORD pkBytesRead, pkTotalBytesAvail, pkBytesLeftThisMessage;
                if (!PeekNamedPipe(hPipe, NULL, 0, &pkBytesRead, &pkTotalBytesAvail, &pkBytesLeftThisMessage)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) {
                        LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Pipe para '%s' quebrou durante timeout. %lu"), hPipe, usernameEsteCliente, GetLastError());
                        esteClienteAtivoNaThread = FALSE;
                        break;
                    }
                }
                continue;
            }
            else if (waitRes != WAIT_OBJECT_0) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Erro %lu ao esperar ReadFile para '%s'."), hPipe, GetLastError(), usernameEsteCliente);
                esteClienteAtivoNaThread = FALSE;
                break;
            }
            if (!GetOverlappedResult(hPipe, &olRead, &bytesLidos, FALSE)) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] GOR falhou após ReadFile para '%s': %lu."), hPipe, usernameEsteCliente, GetLastError());
                esteClienteAtivoNaThread = FALSE;
                break;
            }
            sucessoLeitura = TRUE;
        }
        else if (!sucessoLeitura) {
            if (ctx->servidorEmExecucao) LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] ReadFile falhou para '%s': %lu."), hPipe, usernameEsteCliente, dwErrorRead);
            esteClienteAtivoNaThread = FALSE;
            break;
        }

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {
            EnterCriticalSection(&ctx->csListaJogadores);
            if (meuIndiceNaLista == -1 || meuIndiceNaLista >= MAX_JOGADORES ||
                !ctx->listaJogadores[meuIndiceNaLista].ativo ||
                ctx->listaJogadores[meuIndiceNaLista].hPipe != hPipe ||
                _tcscmp(ctx->listaJogadores[meuIndiceNaLista].username, usernameEsteCliente) != 0) {
                LeaveCriticalSection(&ctx->csListaJogadores);
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Jogador '%s' tornou-se inválido (idx %d) ou pipe/username desatualizado. Encerrando thread."), hPipe, usernameEsteCliente, meuIndiceNaLista);
                esteClienteAtivoNaThread = FALSE;
                break;
            }
            meuJogadorInfo = &ctx->listaJogadores[meuIndiceNaLista];
            LeaveCriticalSection(&ctx->csListaJogadores);

            if (_tcscmp(msg.type, _T("WORD")) == 0) {
                Log(&ctx->csLog, _T("[CLIENT_THREAD %p] '%s' submeteu: '%s'"), hPipe, usernameEsteCliente, msg.data);
                ValidarPalavraJogo(args, msg.data, usernameEsteCliente);
            }
            else if (_tcscmp(msg.type, _T("GET_SCORE")) == 0) {
                MESSAGE respScore = { 0 };
                _tcscpy_s(respScore.type, _countof(respScore.type), _T("SCORE_UPDATE"));
                _tcscpy_s(respScore.username, _countof(respScore.username), usernameEsteCliente);

                EnterCriticalSection(&ctx->csListaJogadores);
                if (meuJogadorInfo && meuJogadorInfo->ativo && meuJogadorInfo->hPipe == hPipe && _tcscmp(meuJogadorInfo->username, usernameEsteCliente) == 0) {
                    _sntprintf_s(respScore.data, _countof(respScore.data), _TRUNCATE, _T("%.1f"), meuJogadorInfo->pontos);
                    respScore.pontos = (int)meuJogadorInfo->pontos;
                }
                else {
                    _tcscpy_s(respScore.data, _countof(respScore.data), _T("Erro/Inativo"));
                    respScore.pontos = 0;
                }
                LeaveCriticalSection(&ctx->csListaJogadores);
                WriteFile(hPipe, &respScore, sizeof(MESSAGE), &bytesEscritos, NULL);
            }
            else if (_tcscmp(msg.type, _T("GET_JOGS")) == 0) {
                MESSAGE respJogs = { 0 };
                _tcscpy_s(respJogs.type, _countof(respJogs.type), _T("PLAYER_LIST_UPDATE"));
                _tcscpy_s(respJogs.username, _countof(respJogs.username), _T("Arbitro"));

                TCHAR listaBuffer[sizeof(respJogs.data)] = { 0 };
                TCHAR* pBufferAtual = listaBuffer;
                size_t bufferRestante = _countof(listaBuffer);

                _tcscpy_s(pBufferAtual, bufferRestante, _T("Jogadores Ativos:\n"));
                pBufferAtual += _tcslen(pBufferAtual);
                bufferRestante = _countof(listaBuffer) - _tcslen(listaBuffer);

                EnterCriticalSection(&ctx->csListaJogadores);
                for (DWORD i = 0; i < MAX_JOGADORES && bufferRestante > 1; ++i) {
                    if (ctx->listaJogadores[i].ativo) {
                        TCHAR linhaJogador[100];
                        _sntprintf_s(linhaJogador, _countof(linhaJogador), _TRUNCATE, _T(" - %s (%.1f pts)\n"),
                            ctx->listaJogadores[i].username, ctx->listaJogadores[i].pontos);
                        size_t lenLinha = _tcslen(linhaJogador);
                        if (lenLinha < bufferRestante) {
                            _tcscpy_s(pBufferAtual, bufferRestante, linhaJogador);
                            pBufferAtual += lenLinha;
                            bufferRestante -= lenLinha;
                        }
                        else {
                            break;
                        }
                    }
                }
                LeaveCriticalSection(&ctx->csListaJogadores);

                _tcscpy_s(respJogs.data, _countof(respJogs.data), listaBuffer);
                WriteFile(hPipe, &respJogs, sizeof(MESSAGE), &bytesEscritos, NULL);
            }
            else if (_tcscmp(msg.type, _T("EXIT")) == 0) {
                Log(&ctx->csLog, _T("[CLIENT_THREAD %p] '%s' solicitou sair."), hPipe, usernameEsteCliente);
                esteClienteAtivoNaThread = FALSE;
                break;
            }
            else {
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] '%s' enviou msg desconhecida: '%s'"), hPipe, usernameEsteCliente, msg.type);
            }
        }
        else if (!sucessoLeitura || bytesLidos == 0) {
            if (ctx->servidorEmExecucao) {
                Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Cliente '%s' desconectou (EOF ou erro)."), hPipe, usernameEsteCliente);
            }
            esteClienteAtivoNaThread = FALSE;
            break;
        }
        else {
            if (ctx->servidorEmExecucao) {
                LogError(&ctx->csLog, _T("[CLIENT_THREAD %p] Erro de framing para '%s'. Bytes: %lu. Esperado: %zu."), hPipe, usernameEsteCliente, bytesLidos, sizeof(MESSAGE));
            }
            esteClienteAtivoNaThread = FALSE;
            break;
        }
    }

    // Bloco de limpeza
    Log(&ctx->csLog, _T("[CLIENT_THREAD %p] Limpando para '%s'..."), hPipe, usernameEsteCliente[0] ? usernameEsteCliente : _T("(desconhecido/não juntou)"));

    if (usernameEsteCliente[0] != _T('\0') && meuIndiceNaLista != -1) {
        RemoverJogador(ctx, usernameEsteCliente, FALSE);
    }
    else if (meuIndiceNaLista != -1) {
        EnterCriticalSection(&ctx->csListaJogadores);
        if (meuIndiceNaLista >= 0 && meuIndiceNaLista < MAX_JOGADORES &&
            ctx->listaJogadores[meuIndiceNaLista].hPipe == hPipe) {
            if (ctx->listaJogadores[meuIndiceNaLista].ativo) {
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Limpando jogador parcialmente adicionado (idx %d, user '%s') que não tinha usernameEsteCliente setado na thread."), hPipe, meuIndiceNaLista, ctx->listaJogadores[meuIndiceNaLista].username);
                ctx->listaJogadores[meuIndiceNaLista].ativo = FALSE;
                if (ctx->totalJogadoresAtivos > 0) ctx->totalJogadoresAtivos--;
                VerificarEstadoJogo(ctx);
            }
            ctx->listaJogadores[meuIndiceNaLista].hPipe = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&ctx->csListaJogadores);
    }

    if (olRead.hEvent) {
        CloseHandle(olRead.hEvent);
    }

    if (hPipe != INVALID_HANDLE_VALUE) {
        EnterCriticalSection(&ctx->csListaJogadores);
        for (int i = 0; i < MAX_JOGADORES; ++i) {
            if (ctx->listaJogadores[i].hPipe == hPipe) {
                LogWarning(&ctx->csLog, _T("[CLIENT_THREAD %p] Pipe ainda estava na lista de jogadores para '%s' no momento da limpeza."), hPipe, ctx->listaJogadores[i].username);
                ctx->listaJogadores[i].hPipe = INVALID_HANDLE_VALUE;
                break;
            }
        }
        LeaveCriticalSection(&ctx->csListaJogadores);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    if (args) {
        free(args);
    }

    return 0;
}

void RemoverJogador(SERVER_CONTEXT* ctx, const TCHAR* username, BOOL notificarClienteParaSair) {
    int idxRemovido = -1;
    HANDLE hPipeDoRemovido = INVALID_HANDLE_VALUE;
    TCHAR nomeDoRemovido[MAX_USERNAME];
    nomeDoRemovido[0] = _T('\0');

    EnterCriticalSection(&ctx->csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo && _tcscmp(ctx->listaJogadores[i].username, username) == 0) {
            _tcscpy_s(nomeDoRemovido, MAX_USERNAME, ctx->listaJogadores[i].username);


            ctx->listaJogadores[i].ativo = FALSE;
            hPipeDoRemovido = ctx->listaJogadores[i].hPipe;
            ctx->listaJogadores[i].hPipe = INVALID_HANDLE_VALUE;
            if (ctx->totalJogadoresAtivos > 0) ctx->totalJogadoresAtivos--;
            idxRemovido = (int)i;
            Log(&ctx->csLog, _T("[JOGADOR] '%s' marcado como inativo. Total ativos agora: %lu"), nomeDoRemovido, ctx->totalJogadoresAtivos);
            break;
        }
    }
    LeaveCriticalSection(&ctx->csListaJogadores);

    if (idxRemovido != -1) {
        if (notificarClienteParaSair && hPipeDoRemovido != INVALID_HANDLE_VALUE) {
            MESSAGE msgKick; ZeroMemory(&msgKick, sizeof(MESSAGE));
            _tcscpy_s(msgKick.type, _countof(msgKick.type), _T("SHUTDOWN"));
            _tcscpy_s(msgKick.username, _countof(msgKick.username), _T("Arbitro"));
            _tcscpy_s(msgKick.data, _countof(msgKick.data), _T("Você foi desconectado/excluído pelo administrador."));
            DWORD bw;
            WriteFile(hPipeDoRemovido, &msgKick, sizeof(MESSAGE), &bw, NULL);
        }

        MESSAGE msgNotificacaoSaida; ZeroMemory(&msgNotificacaoSaida, sizeof(MESSAGE));
        _tcscpy_s(msgNotificacaoSaida.type, _countof(msgNotificacaoSaida.type), _T("GAME_UPDATE"));
        _sntprintf_s(msgNotificacaoSaida.data, _countof(msgNotificacaoSaida.data), _TRUNCATE, _T("Jogador %s saiu do jogo."), nomeDoRemovido);
        NotificarTodosOsJogadores(ctx, &msgNotificacaoSaida, nomeDoRemovido);

        VerificarEstadoJogo(ctx);
    }
    else {
        LogWarning(&ctx->csLog, _T("[JOGADOR] '%s' não encontrado para remoção ou já estava inativo."), username);
    }
}

int EncontrarJogador(SERVER_CONTEXT* ctx, const TCHAR* username) {
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo && _tcscmp(ctx->listaJogadores[i].username, username) == 0) {
            return (int)i;
        }
    }
    return -1;
}

BOOL ValidarPalavraJogo(THREAD_ARGS* argsClienteThread, const TCHAR* palavraSubmetida, const TCHAR* usernameJogador) {
    SERVER_CONTEXT* ctx = argsClienteThread->serverCtx;
    HANDLE hPipeCliente = argsClienteThread->hPipeCliente;

    MESSAGE resposta; ZeroMemory(&resposta, sizeof(MESSAGE));
    _tcscpy_s(resposta.username, _countof(resposta.username), usernameJogador);
    float pontosAlterados = 0.0f;

    if (!ctx->jogoRealmenteAtivo) {
        _tcscpy_s(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
        _tcscpy_s(resposta.data, _countof(resposta.data), _T("O jogo não está ativo."));
    }
    else {
        TCHAR palavraUpper[MAX_WORD];
        _tcscpy_s(palavraUpper, MAX_WORD, palavraSubmetida);
        _tcsupr_s(palavraUpper, MAX_WORD);

        BOOL existeNoDicionario = FALSE;
        EnterCriticalSection(&ctx->dicionario.csDicionario);
        if (ctx->dicionario.palavras && ctx->dicionario.totalPalavras > 0) {
            for (DWORD i = 0; i < ctx->dicionario.totalPalavras; ++i) {
                if (ctx->dicionario.palavras[i] && _tcscmp(ctx->dicionario.palavras[i], palavraUpper) == 0) {
                    existeNoDicionario = TRUE;
                    break;
                }
            }
        }
        LeaveCriticalSection(&ctx->dicionario.csDicionario);

        if (!existeNoDicionario) {
            _tcscpy_s(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
            _sntprintf_s(resposta.data, _countof(resposta.data), _TRUNCATE, _T("'%s' não existe no dicionário."), palavraSubmetida);
            pontosAlterados = -((float)_tcslen(palavraSubmetida) * 0.5f);
        }
        else {
            WaitForSingleObject(ctx->hMutexShm, INFINITE);
            BOOL podeFormar = TRUE;
            TCHAR letrasDisponiveisCopia[MAX_LETRAS_TABULEIRO + 1];
            int numMaxLetrasAtual = 0;
            if (ctx->pDadosShm) {
                numMaxLetrasAtual = ctx->pDadosShm->numMaxLetrasAtual;
                for (int k = 0; k < numMaxLetrasAtual && k < MAX_LETRAS_TABULEIRO; ++k) {
                    letrasDisponiveisCopia[k] = ctx->pDadosShm->letrasVisiveis[k];
                }
                letrasDisponiveisCopia[numMaxLetrasAtual < MAX_LETRAS_TABULEIRO ? numMaxLetrasAtual : MAX_LETRAS_TABULEIRO] = _T('\0');
            }
            else {
                podeFormar = FALSE;
            }

            size_t lenPalavra = _tcslen(palavraUpper);
            if (lenPalavra == 0) podeFormar = FALSE;

            if (podeFormar) {
                for (size_t k = 0; k < lenPalavra; ++k) {
                    BOOL encontrouLetra = FALSE;
                    for (int m = 0; m < numMaxLetrasAtual; ++m) {
                        if (letrasDisponiveisCopia[m] == palavraUpper[k]) {
                            letrasDisponiveisCopia[m] = _T(' ');
                            encontrouLetra = TRUE;
                            break;
                        }
                    }
                    if (!encontrouLetra) {
                        podeFormar = FALSE;
                        break;
                    }
                }
            }

            if (podeFormar && ctx->pDadosShm) {
                _tcscpy_s(resposta.type, _countof(resposta.type), _T("WORD_VALID"));
                pontosAlterados = (float)lenPalavra;
                _sntprintf_s(resposta.data, _countof(resposta.data), _TRUNCATE, _T("'%s' válida! +%.1f pts."), palavraSubmetida, pontosAlterados);

                for (size_t k = 0; k < lenPalavra; ++k) {
                    for (int m = 0; m < numMaxLetrasAtual; ++m) {
                        if (ctx->pDadosShm->letrasVisiveis[m] == palavraUpper[k]) {
                            ctx->pDadosShm->letrasVisiveis[m] = _T('_');
                            break;
                        }
                    }
                }
                ctx->pDadosShm->generationCount++;
                _tcscpy_s(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, palavraSubmetida);
                _tcscpy_s(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, usernameJogador);
                ctx->pDadosShm->pontuacaoUltimaPalavra = (int)pontosAlterados;
                SetEvent(ctx->hEventoShmUpdate);

                MESSAGE notifPalavra; ZeroMemory(&notifPalavra, sizeof(MESSAGE));
                _tcscpy_s(notifPalavra.type, _countof(notifPalavra.type), _T("GAME_UPDATE"));
                _sntprintf_s(notifPalavra.data, _countof(notifPalavra.data), _TRUNCATE, _T("%s acertou '%s' (+%.1f pts)!"), usernameJogador, palavraSubmetida, pontosAlterados);
                NotificarTodosOsJogadores(ctx, &notifPalavra, NULL);
            }
            else {
                _tcscpy_s(resposta.type, _countof(resposta.type), _T("WORD_INVALID"));
                if (ctx->pDadosShm) {
                    _sntprintf_s(resposta.data, _countof(resposta.data), _TRUNCATE, _T("'%s' não pode ser formada com as letras atuais."), palavraSubmetida);
                }
                else {
                    _sntprintf_s(resposta.data, _countof(resposta.data), _TRUNCATE, _T("Erro de servidor ao validar '%s'."), palavraSubmetida);
                }
                pontosAlterados = -((float)lenPalavra * 0.5f);
            }
            ReleaseMutex(ctx->hMutexShm);
        }
    }
    resposta.pontos = (int)floor(pontosAlterados + 0.5f);

    if (pontosAlterados != 0.0f) {
        EnterCriticalSection(&ctx->csListaJogadores);
        int idxJogador = EncontrarJogador(ctx, usernameJogador);
        if (idxJogador != -1) {
            ctx->listaJogadores[idxJogador].pontos += pontosAlterados;
            Log(&ctx->csLog, _T("[JOGO] %s (%s '%s'): %+.1f pts. Total: %.1f"), usernameJogador,
                (_tcscmp(resposta.type, _T("WORD_VALID")) == 0 ? _T("acertou") : _T("errou/inválida")),
                palavraSubmetida,
                pontosAlterados, ctx->listaJogadores[idxJogador].pontos);
        }
        LeaveCriticalSection(&ctx->csListaJogadores);
    }

    DWORD bytesEscritos;
    WriteFile(hPipeCliente, &resposta, sizeof(MESSAGE), &bytesEscritos, NULL);
    return (_tcscmp(resposta.type, _T("WORD_VALID")) == 0);
}

void VerificarEstadoJogo(SERVER_CONTEXT* ctx) {
    EnterCriticalSection(&ctx->csListaJogadores);
    BOOL deveEstarAtivo = (ctx->totalJogadoresAtivos >= MIN_JOGADORES_PARA_INICIAR);
    DWORD totalJogadoresAtivosSnapshot = ctx->totalJogadoresAtivos;
    LeaveCriticalSection(&ctx->csListaJogadores);

    WaitForSingleObject(ctx->hMutexShm, INFINITE);
    if (!ctx->pDadosShm) {
        ReleaseMutex(ctx->hMutexShm);
        LogError(&ctx->csLog, _T("[JOGO] SHM nula em VerificarEstadoJogo."));
        return;
    }
    BOOL estavaAtivo = ctx->pDadosShm->jogoAtivo;

    if (deveEstarAtivo && !estavaAtivo) {
        ctx->pDadosShm->jogoAtivo = TRUE;
        ctx->jogoRealmenteAtivo = TRUE;
        Log(&ctx->csLog, _T("[JOGO] O jogo começou/recomeçou! (%lu jogadores)"), totalJogadoresAtivosSnapshot);

        for (int i = 0; i < ctx->pDadosShm->numMaxLetrasAtual; ++i) ctx->pDadosShm->letrasVisiveis[i] = _T('_');
        _tcscpy_s(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
        _tcscpy_s(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
        ctx->pDadosShm->pontuacaoUltimaPalavra = 0;
        ctx->pDadosShm->generationCount++;
        SetEvent(ctx->hEventoShmUpdate);

        MESSAGE msgInicio; ZeroMemory(&msgInicio, sizeof(MESSAGE));
        _tcscpy_s(msgInicio.type, _countof(msgInicio.type), _T("GAME_START"));
        _tcscpy_s(msgInicio.username, _countof(msgInicio.username), _T("Arbitro"));
        _tcscpy_s(msgInicio.data, _countof(msgInicio.data), _T("O jogo começou!"));
        NotificarTodosOsJogadores(ctx, &msgInicio, NULL);
    }
    else if (!deveEstarAtivo && estavaAtivo) {
        ctx->pDadosShm->jogoAtivo = FALSE;
        ctx->jogoRealmenteAtivo = FALSE;
        Log(&ctx->csLog, _T("[JOGO] O jogo terminou/pausou! (%lu jogadores, mínimo %d necessário)"), totalJogadoresAtivosSnapshot, MIN_JOGADORES_PARA_INICIAR);

        for (int i = 0; i < ctx->pDadosShm->numMaxLetrasAtual; ++i) ctx->pDadosShm->letrasVisiveis[i] = _T('_');
        _tcscpy_s(ctx->pDadosShm->ultimaPalavraIdentificada, MAX_WORD, _T(""));
        _tcscpy_s(ctx->pDadosShm->usernameUltimaPalavra, MAX_USERNAME, _T(""));
        ctx->pDadosShm->pontuacaoUltimaPalavra = 0;
        ctx->pDadosShm->generationCount++;
        SetEvent(ctx->hEventoShmUpdate);

        EnterCriticalSection(&ctx->csListaJogadores);
        for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
            if (ctx->listaJogadores[i].ativo) {
                ctx->listaJogadores[i].pontos = 0.0f;
            }
        }
        LeaveCriticalSection(&ctx->csListaJogadores);

        MESSAGE msgFim; ZeroMemory(&msgFim, sizeof(MESSAGE));
        _tcscpy_s(msgFim.type, _countof(msgFim.type), _T("GAME_STOP"));
        _tcscpy_s(msgFim.username, _countof(msgFim.username), _T("Arbitro"));
        _sntprintf_s(msgFim.data, _countof(msgFim.data), _TRUNCATE, _T("Jogo pausado/terminado. Mínimo %d jogadores necessário."), MIN_JOGADORES_PARA_INICIAR);
        NotificarTodosOsJogadores(ctx, &msgFim, NULL);
    }
    ReleaseMutex(ctx->hMutexShm);
}

void NotificarTodosOsJogadores(SERVER_CONTEXT* ctx, const MESSAGE* msgAEnviar, const TCHAR* skipUsername) {
    DWORD bytesEscritos;
    EnterCriticalSection(&ctx->csListaJogadores);
    for (DWORD i = 0; i < MAX_JOGADORES; ++i) {
        if (ctx->listaJogadores[i].ativo && ctx->listaJogadores[i].hPipe != INVALID_HANDLE_VALUE &&
            (skipUsername == NULL || _tcscmp(ctx->listaJogadores[i].username, skipUsername) != 0)) {
            BOOL writeOk = WriteFile(ctx->listaJogadores[i].hPipe, msgAEnviar, sizeof(MESSAGE), &bytesEscritos, NULL);
            if (!writeOk || bytesEscritos != sizeof(MESSAGE)) {
                LogWarning(&ctx->csLog, _T("[NOTIF] Falha ao notificar '%s' (pipe %p, erro %lu). Marcando para remoção."),
                    ctx->listaJogadores[i].username, ctx->listaJogadores[i].hPipe, GetLastError());
                TCHAR usernameToRemove[MAX_USERNAME];
                _tcscpy_s(usernameToRemove, _countof(usernameToRemove), ctx->listaJogadores[i].username);
                LeaveCriticalSection(&ctx->csListaJogadores);
                RemoverJogador(ctx, usernameToRemove, FALSE);
                EnterCriticalSection(&ctx->csListaJogadores);
            }
        }
    }
    LeaveCriticalSection(&ctx->csListaJogadores);
}