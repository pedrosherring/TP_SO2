#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>
#include <fcntl.h>
#include <io.h>
#include <ctype.h>
#include <wchar.h>

#include "../Comum/compartilhado.h" // Ficheiro partilhado

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array[0]))
#endif

// Definição de Timeout
#define IO_TIMEOUT 5000

// Estrutura principal para o estado do cliente (JogoUI)
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

// Declarações de funções
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