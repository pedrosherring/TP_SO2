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

// Definição de Timeout que estava em falta
#define IO_TIMEOUT 5000 

// ==========================================================================================
// PROTÓTIPOS DE FUNÇÕES INTERNAS (FORWARD DECLARATIONS)
// ==========================================================================================
void LogCliente(const TCHAR* format, ...); // Função de log principal
void LogErrorCliente(const TCHAR* format, ...); // Wrapper para LogCliente com prefixo [ERRO]
void LogWarningCliente(const TCHAR* format, ...); // Wrapper para LogCliente com prefixo [AVISO]

BOOL ConectarAoServidorJogo();
BOOL AbrirRecursosCompartilhadosCliente();
void LimparRecursosCliente();
void EnviarMensagemAoServidor(const MESSAGE* msg);
void MostrarEstadoJogoCliente();
void ProcessarInputUtilizador(const TCHAR* input);
DWORD WINAPI ThreadReceptorMensagensServidor(LPVOID param);
DWORD WINAPI ThreadMonitorSharedMemoryCliente(LPVOID param);


// ==========================================================================================
// VARIÁVEIS GLOBAIS DO JOGOUI
// ==========================================================================================
TCHAR g_meuUsername[MAX_USERNAME];
HANDLE g_hPipeServidor = INVALID_HANDLE_VALUE;

DadosJogoCompartilhados* g_pDadosShmCliente = NULL;
HANDLE g_hMapFileShmCliente = NULL;
HANDLE g_hEventoShmUpdateCliente = NULL;
HANDLE g_hMutexShmCliente = NULL;

volatile BOOL g_clienteRodando = TRUE;
long g_ultimaGeracaoConhecida = -1;

CRITICAL_SECTION g_csConsoleCliente;

HANDLE g_hThreadReceptorPipe = NULL;
HANDLE g_hThreadMonitorShm = NULL;


// ==========================================================================================
// FUNÇÃO PRINCIPAL - _tmain
// ==========================================================================================
int _tmain(int argc, TCHAR* argv[]) {
#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
    (void)_setmode(_fileno(stderr), _O_WTEXT);
#endif

    if (argc != 2) {
        _tprintf(_T("Uso: jogoui.exe <username>\n"));
        return 1;
    }
    StringCchCopy(g_meuUsername, MAX_USERNAME, argv[1]);
    InitializeCriticalSection(&g_csConsoleCliente);

    LogCliente(_T("JogoUI para '%s' a iniciar..."), g_meuUsername);

    if (!ConectarAoServidorJogo()) {
        LogErrorCliente(_T("Falha ao conectar ao servidor. Encerrando."));
        LimparRecursosCliente();
        DeleteCriticalSection(&g_csConsoleCliente);
        _tprintf(_T("Pressione Enter para sair...\n")); (void)getchar();
        return 1;
    }

    if (!AbrirRecursosCompartilhadosCliente()) {
        LogErrorCliente(_T("Falha ao abrir recursos compartilhados. Encerrando."));
        LimparRecursosCliente();
        DeleteCriticalSection(&g_csConsoleCliente);
        _tprintf(_T("Pressione Enter para sair...\n")); (void)getchar();
        return 1;
    }

    MESSAGE msgJoin; ZeroMemory(&msgJoin, sizeof(MESSAGE));
    StringCchCopy(msgJoin.type, _countof(msgJoin.type), _T("JOIN"));
    StringCchCopy(msgJoin.username, _countof(msgJoin.username), g_meuUsername);
    EnviarMensagemAoServidor(&msgJoin);

    g_hThreadReceptorPipe = CreateThread(NULL, 0, ThreadReceptorMensagensServidor, NULL, 0, NULL);
    if (g_hThreadReceptorPipe == NULL) {
        LogErrorCliente(_T("Falha ao criar thread receptora de pipe. Encerrando."));
        g_clienteRodando = FALSE;
        LimparRecursosCliente();
        DeleteCriticalSection(&g_csConsoleCliente);
        _tprintf(_T("Pressione Enter para sair...\n")); (void)getchar();
        return 1;
    }

    g_hThreadMonitorShm = CreateThread(NULL, 0, ThreadMonitorSharedMemoryCliente, NULL, 0, NULL);
    if (g_hThreadMonitorShm == NULL) {
        LogErrorCliente(_T("Falha ao criar thread monitora de SHM. Atualizações do tabuleiro podem não funcionar."));
        // Não necessariamente fatal, mas o tabuleiro não será atualizado pela TSM.
    }


    TCHAR inputBuffer[MAX_WORD + 20];

    EnterCriticalSection(&g_csConsoleCliente);
    MostrarEstadoJogoCliente();
    _tprintf(_T("%s> "), g_meuUsername);
    fflush(stdout);
    LeaveCriticalSection(&g_csConsoleCliente);

    while (g_clienteRodando) {
        if (_fgetts(inputBuffer, _countof(inputBuffer), stdin) != NULL) {
            inputBuffer[_tcscspn(inputBuffer, _T("\r\n"))] = _T('\0');
            if (!g_clienteRodando) break;

            ProcessarInputUtilizador(inputBuffer);

            if (g_clienteRodando) {
                EnterCriticalSection(&g_csConsoleCliente);
                _tprintf(_T("\n%s> "), g_meuUsername);
                fflush(stdout);
                LeaveCriticalSection(&g_csConsoleCliente);
            }
        }
        else {
            if (g_clienteRodando) {
                LogErrorCliente(_T("Erro ou EOF ao ler input. Encerrando..."));
                g_clienteRodando = FALSE;
            }
            break;
        }
    }

    LogCliente(_T("Loop principal de input terminado. Aguardando threads..."));
    if (g_hEventoShmUpdateCliente) SetEvent(g_hEventoShmUpdateCliente);

    if (g_hThreadReceptorPipe != NULL) {
        if (WaitForSingleObject(g_hThreadReceptorPipe, 3000) == WAIT_TIMEOUT) {
            LogWarningCliente(_T("Timeout ao aguardar thread receptora de pipe."));
        }
        CloseHandle(g_hThreadReceptorPipe);
        g_hThreadReceptorPipe = NULL;
    }
    if (g_hThreadMonitorShm != NULL) {
        if (WaitForSingleObject(g_hThreadMonitorShm, 2000) == WAIT_TIMEOUT) {
            LogWarningCliente(_T("Timeout ao aguardar thread monitora de SHM."));
        }
        CloseHandle(g_hThreadMonitorShm);
        g_hThreadMonitorShm = NULL;
    }


    LimparRecursosCliente();
    LogCliente(_T("JogoUI para '%s' encerrado."), g_meuUsername);
    DeleteCriticalSection(&g_csConsoleCliente);
    _tprintf(_T("\nDesconectado. Pressione Enter para sair...\n"));
    (void)getchar();
    return 0;
}

// ==========================================================================================
// FUNÇÕES AUXILIARES E THREADS DO JOGOUI
// ==========================================================================================
void LogCliente(const TCHAR* format, ...) {
    if (g_csConsoleCliente.DebugInfo == NULL) {
        TCHAR fbBuffer[1024];
        va_list fbArgs;
        va_start(fbArgs, format);
        StringCchVPrintf(fbBuffer, _countof(fbBuffer), format, fbArgs);
        va_end(fbArgs);
        _tprintf_s(_T("[JOGOUI-NO_CS] %s\n"), fbBuffer);
        fflush(stdout);
        return;
    }

    EnterCriticalSection(&g_csConsoleCliente);
    TCHAR buffer[2048];
    va_list args;
    va_start(args, format);
    SYSTEMTIME st;
    GetLocalTime(&st);
    size_t prefixLen = 0;

    StringCchPrintf(buffer, _countof(buffer), _T("\n%02d:%02d:%02d.%03d [%s-JOGOUI] "),
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_meuUsername[0] ? g_meuUsername : _T("CLIENT"));

    (void)StringCchLength(buffer, _countof(buffer), &prefixLen);

    if (prefixLen < _countof(buffer) - 1) {
        StringCchVPrintf(buffer + prefixLen, _countof(buffer) - prefixLen, format, args);
    }

    StringCchCat(buffer, _countof(buffer), _T("\n"));
    _tprintf_s(buffer);
    fflush(stdout);
    va_end(args);
    LeaveCriticalSection(&g_csConsoleCliente);
}

void LogErrorCliente(const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    LogCliente(_T("[ERRO] %s"), buffer);
}

void LogWarningCliente(const TCHAR* format, ...) {
    TCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    StringCchVPrintf(buffer, _countof(buffer), format, args);
    va_end(args);
    LogCliente(_T("[AVISO] %s"), buffer);
}


BOOL ConectarAoServidorJogo() {
    int tentativas = 0;
    const int MAX_TENTATIVAS_PIPE = 5;
    LogCliente(_T("Tentando conectar ao pipe do servidor: %s"), PIPE_NAME);
    while (tentativas < MAX_TENTATIVAS_PIPE && g_clienteRodando) {
        g_hPipeServidor = CreateFile(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        if (g_hPipeServidor != INVALID_HANDLE_VALUE) {
            DWORD dwMode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(g_hPipeServidor, &dwMode, NULL, NULL)) {
                LogErrorCliente(_T("Falha ao definir modo do pipe para mensagem: %lu"), GetLastError());
                CloseHandle(g_hPipeServidor);
                g_hPipeServidor = INVALID_HANDLE_VALUE;
                return FALSE;
            }
            LogCliente(_T("Conectado ao servidor com sucesso (Pipe: %p)."), g_hPipeServidor);
            return TRUE;
        }

        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) {
            LogErrorCliente(_T("Erro não esperado ao conectar ao pipe: %lu"), GetLastError());
            return FALSE;
        }
        LogWarningCliente(_T("Pipe ocupado ou não encontrado (tentativa %d/%d). Tentando novamente em 1s..."), tentativas + 1, MAX_TENTATIVAS_PIPE);
        Sleep(1000);
        tentativas++;
    }
    if (!g_clienteRodando) LogCliente(_T("Conexão cancelada durante tentativas."));
    else LogErrorCliente(_T("Não foi possível conectar ao servidor após %d tentativas."), MAX_TENTATIVAS_PIPE);
    return FALSE;
}

BOOL AbrirRecursosCompartilhadosCliente() {
    g_hMapFileShmCliente = OpenFileMapping(FILE_MAP_READ, FALSE, SHM_NAME);
    if (g_hMapFileShmCliente == NULL) {
        LogErrorCliente(_T("Falha ao abrir FileMapping '%s': %lu"), SHM_NAME, GetLastError());
        return FALSE;
    }
    g_pDadosShmCliente = (DadosJogoCompartilhados*)MapViewOfFile(g_hMapFileShmCliente, FILE_MAP_READ, 0, 0, sizeof(DadosJogoCompartilhados));
    if (g_pDadosShmCliente == NULL) {
        LogErrorCliente(_T("Falha ao mapear SHM '%s': %lu"), SHM_NAME, GetLastError());
        CloseHandle(g_hMapFileShmCliente); g_hMapFileShmCliente = NULL;
        return FALSE;
    }
    g_hEventoShmUpdateCliente = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, EVENT_SHM_UPDATE);
    if (g_hEventoShmUpdateCliente == NULL) {
        LogErrorCliente(_T("Falha ao abrir evento SHM '%s' com direitos de modificação: %lu."), EVENT_SHM_UPDATE, GetLastError());
        UnmapViewOfFile(g_pDadosShmCliente); g_pDadosShmCliente = NULL;
        CloseHandle(g_hMapFileShmCliente); g_hMapFileShmCliente = NULL;
        return FALSE;
    }
    g_hMutexShmCliente = OpenMutex(SYNCHRONIZE, FALSE, MUTEX_SHARED_MEM);
    if (g_hMutexShmCliente == NULL) {
        LogWarningCliente(_T("Falha ao abrir mutex da SHM '%s': %lu. Leitura pode ter pequenas inconsistências visuais."), MUTEX_SHARED_MEM, GetLastError());
    }

    LogCliente(_T("Recursos compartilhados abertos com sucesso."));
    if (g_pDadosShmCliente) g_ultimaGeracaoConhecida = g_pDadosShmCliente->generationCount;
    return TRUE;
}

void LimparRecursosCliente() {
    LogCliente(_T("Limpando recursos do cliente..."));
    if (g_hPipeServidor != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hPipeServidor);
        g_hPipeServidor = INVALID_HANDLE_VALUE;
    }
    if (g_pDadosShmCliente != NULL) {
        UnmapViewOfFile(g_pDadosShmCliente);
        g_pDadosShmCliente = NULL;
    }
    if (g_hMapFileShmCliente != NULL) {
        CloseHandle(g_hMapFileShmCliente);
        g_hMapFileShmCliente = NULL;
    }
    if (g_hEventoShmUpdateCliente != NULL) {
        CloseHandle(g_hEventoShmUpdateCliente);
        g_hEventoShmUpdateCliente = NULL;
    }
    if (g_hMutexShmCliente != NULL) {
        CloseHandle(g_hMutexShmCliente);
        g_hMutexShmCliente = NULL;
    }
}

void EnviarMensagemAoServidor(const MESSAGE* msg) {
    if (g_hPipeServidor == INVALID_HANDLE_VALUE || !g_clienteRodando) {
        LogErrorCliente(_T("Não é possível enviar mensagem: pipe inválido ou cliente não está rodando."));
        return;
    }
    DWORD bytesEscritos;
    OVERLAPPED ovWrite; ZeroMemory(&ovWrite, sizeof(OVERLAPPED));
    ovWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (ovWrite.hEvent == NULL) {
        LogErrorCliente(_T("Falha ao criar evento para WriteFile. Mensagem tipo '%s' não enviada."), msg->type);
        return;
    }

    if (!WriteFile(g_hPipeServidor, msg, sizeof(MESSAGE), &bytesEscritos, &ovWrite)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ovWrite.hEvent, IO_TIMEOUT) == WAIT_OBJECT_0) { // Usa IO_TIMEOUT definido
                GetOverlappedResult(g_hPipeServidor, &ovWrite, &bytesEscritos, FALSE);
                if (bytesEscritos != sizeof(MESSAGE)) {
                    LogErrorCliente(_T("WriteFile Overlapped enviou %lu bytes em vez de %zu para msg tipo '%s'."), bytesEscritos, sizeof(MESSAGE), msg->type);
                }
            }
            else {
                LogErrorCliente(_T("Timeout ao enviar mensagem tipo '%s'. Cancelando IO."), msg->type);
                CancelIoEx(g_hPipeServidor, &ovWrite);
                if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) g_clienteRodando = FALSE;
            }
        }
        else {
            LogErrorCliente(_T("Falha ao enviar mensagem tipo '%s' para o servidor: %lu"), msg->type, GetLastError());
            if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_PIPE_NOT_CONNECTED) g_clienteRodando = FALSE;
        }
    }
    else {
        GetOverlappedResult(g_hPipeServidor, &ovWrite, &bytesEscritos, FALSE);
        if (bytesEscritos != sizeof(MESSAGE)) {
            LogErrorCliente(_T("WriteFile Síncrono (com overlapped handle) enviou %lu bytes em vez de %zu para msg tipo '%s'."), bytesEscritos, sizeof(MESSAGE), msg->type);
        }
    }
    CloseHandle(ovWrite.hEvent);
}

void MostrarEstadoJogoCliente() {
    EnterCriticalSection(&g_csConsoleCliente);
    if (g_pDadosShmCliente != NULL) {
        if (g_hMutexShmCliente) WaitForSingleObject(g_hMutexShmCliente, 100);

        _tprintf(_T("\n====================================\n"));
        _tprintf(_T("Letras: "));
        for (int i = 0; i < g_pDadosShmCliente->numMaxLetrasAtual; ++i) {
            _tprintf(_T("%c "), g_pDadosShmCliente->letrasVisiveis[i]);
        }
        _tprintf(_T("\n"));

        if (g_pDadosShmCliente->ultimaPalavraIdentificada[0] != _T('\0')) {
            _tprintf(_T("Última palavra por %s: %s (+%d pts)\n"),
                g_pDadosShmCliente->usernameUltimaPalavra,
                g_pDadosShmCliente->ultimaPalavraIdentificada,
                g_pDadosShmCliente->pontuacaoUltimaPalavra);
        }
        _tprintf(_T("Jogo Ativo: %s\n"), g_pDadosShmCliente->jogoAtivo ? _T("Sim") : _T("Não"));
        _tprintf(_T("====================================\n"));

        if (g_hMutexShmCliente) ReleaseMutex(g_hMutexShmCliente);
    }
    else {
        _tprintf(_T("\n[AVISO] Memória partilhada não disponível.\n"));
    }
    fflush(stdout);
    LeaveCriticalSection(&g_csConsoleCliente);
}

void ProcessarInputUtilizador(const TCHAR* input) {
    MESSAGE msgParaServidor;
    ZeroMemory(&msgParaServidor, sizeof(MESSAGE));
    StringCchCopy(msgParaServidor.username, MAX_USERNAME, g_meuUsername);

    if (_tcslen(input) == 0) {
        return;
    }

    if (_tcscmp(input, _T(":sair")) == 0) {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("EXIT"));
        EnviarMensagemAoServidor(&msgParaServidor);
        g_clienteRodando = FALSE;
    }
    else if (_tcscmp(input, _T(":pont")) == 0) {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("GET_SCORE"));
        EnviarMensagemAoServidor(&msgParaServidor);
    }
    else if (_tcscmp(input, _T(":jogs")) == 0) {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("GET_JOGS"));
        EnviarMensagemAoServidor(&msgParaServidor);
    }
    else if (input[0] == _T(':')) {
        LogWarningCliente(_T("Comando desconhecido: '%s'"), input);
    }
    else {
        StringCchCopy(msgParaServidor.type, _countof(msgParaServidor.type), _T("WORD"));
        StringCchCopy(msgParaServidor.data, MAX_WORD, input);
        EnviarMensagemAoServidor(&msgParaServidor);
    }
}

DWORD WINAPI ThreadReceptorMensagensServidor(LPVOID param) {
    MESSAGE msgDoServidor;
    DWORD bytesLidos;
    OVERLAPPED ovReadPipe; ZeroMemory(&ovReadPipe, sizeof(OVERLAPPED));
    ovReadPipe.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (ovReadPipe.hEvent == NULL) {
        LogErrorCliente(_T("TRA: Falha ao criar evento de leitura do pipe. Encerrando thread."));
        return 1;
    }
    LogCliente(_T("TRA: Thread Receptora de Mensagens iniciada."));

    while (g_clienteRodando) {
        ResetEvent(ovReadPipe.hEvent);
        BOOL sucessoLeitura = ReadFile(g_hPipeServidor, &msgDoServidor, sizeof(MESSAGE), &bytesLidos, &ovReadPipe);

        if (!sucessoLeitura && GetLastError() == ERROR_IO_PENDING) {
            DWORD waitRes = WaitForSingleObject(ovReadPipe.hEvent, 500);
            if (waitRes == WAIT_TIMEOUT) {
                if (!g_clienteRodando) break;
                continue;
            }
            else if (waitRes != WAIT_OBJECT_0) {
                if (g_clienteRodando) LogErrorCliente(_T("TRA: Erro %lu ao esperar ReadFile do pipe."), GetLastError());
                g_clienteRodando = FALSE; break;
            }
            if (!GetOverlappedResult(g_hPipeServidor, &ovReadPipe, &bytesLidos, FALSE)) {
                if (g_clienteRodando) LogErrorCliente(_T("TRA: GOR falhou após ReadFile do pipe: %lu."), GetLastError());
                g_clienteRodando = FALSE; break;
            }
            sucessoLeitura = TRUE;
        }
        else if (!sucessoLeitura) {
            if (g_clienteRodando) LogErrorCliente(_T("TRA: ReadFile falhou imediatamente: %lu."), GetLastError());
            g_clienteRodando = FALSE; break;
        }

        if (!g_clienteRodando) break;

        if (sucessoLeitura && bytesLidos == sizeof(MESSAGE)) {
            EnterCriticalSection(&g_csConsoleCliente);
            _tprintf(_T("\n[SERVIDOR->%s] %s: %s (Pontos na msg: %d)\n"),
                g_meuUsername, msgDoServidor.type, msgDoServidor.data, msgDoServidor.pontos);

            if (_tcscmp(msgDoServidor.type, _T("JOIN_USER_EXISTS")) == 0 ||
                _tcscmp(msgDoServidor.type, _T("JOIN_GAME_FULL")) == 0 ||
                _tcscmp(msgDoServidor.type, _T("SHUTDOWN")) == 0) {
                LogWarningCliente(_T("Recebida mensagem de terminação do servidor: %s"), msgDoServidor.type);
                g_clienteRodando = FALSE;
            }
            fflush(stdout);
            LeaveCriticalSection(&g_csConsoleCliente);
            if (!g_clienteRodando) break;

        }
        else if (sucessoLeitura && bytesLidos == 0) {
            if (g_clienteRodando) { LogWarningCliente(_T("TRA: Servidor fechou a conexão (EOF).")); g_clienteRodando = FALSE; }
            break;
        }
        else if (bytesLidos != 0) {
            if (g_clienteRodando) { LogErrorCliente(_T("TRA: Mensagem incompleta do servidor (%lu bytes)."), bytesLidos); g_clienteRodando = FALSE; }
            break;
        }
    }

    if (ovReadPipe.hEvent) CloseHandle(ovReadPipe.hEvent);
    LogCliente(_T("TRA: Thread Receptora de Mensagens a terminar."));
    return 0;
}

DWORD WINAPI ThreadMonitorSharedMemoryCliente(LPVOID param) {
    LogCliente(_T("TSM: Thread Monitora de SHM iniciada."));
    if (g_hEventoShmUpdateCliente == NULL || g_pDadosShmCliente == NULL) {
        LogErrorCliente(_T("TSM: Evento ou SHM não inicializados. Encerrando thread."));
        return 1;
    }

    while (g_clienteRodando) {
        DWORD waitResult = WaitForSingleObject(g_hEventoShmUpdateCliente, 1000);

        if (!g_clienteRodando) break;

        if (waitResult == WAIT_OBJECT_0) {
            if (g_pDadosShmCliente->generationCount != g_ultimaGeracaoConhecida) {
                g_ultimaGeracaoConhecida = g_pDadosShmCliente->generationCount;
                MostrarEstadoJogoCliente();
                EnterCriticalSection(&g_csConsoleCliente);
                _tprintf(_T("%s> "), g_meuUsername);
                fflush(stdout);
                LeaveCriticalSection(&g_csConsoleCliente);
            }
            if (!ResetEvent(g_hEventoShmUpdateCliente)) {
                if (g_clienteRodando) LogErrorCliente(_T("TSM: Falha ao resetar evento SHM: %lu"), GetLastError());
            }
        }
        else if (waitResult == WAIT_TIMEOUT) {
            if (g_pDadosShmCliente && g_pDadosShmCliente->generationCount != g_ultimaGeracaoConhecida) {
                g_ultimaGeracaoConhecida = g_pDadosShmCliente->generationCount;
                MostrarEstadoJogoCliente();
                EnterCriticalSection(&g_csConsoleCliente);
                _tprintf(_T("%s> "), g_meuUsername);
                fflush(stdout);
                LeaveCriticalSection(&g_csConsoleCliente);
            }
        }
        else if (g_clienteRodando) {
            LogErrorCliente(_T("TSM: Erro %lu ao esperar evento SHM."), GetLastError());
            Sleep(1000);
        }
    }
    LogCliente(_T("TSM: Thread Monitora de SHM a terminar."));
    return 0;
}
