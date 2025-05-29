#ifndef ARBITRO_H
#define ARBITRO_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>

#include "../Comum/compartilhado.h" // Ficheiro partilhado entre os programas

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof(array[0]))
#endif

// Definições de Timeout
#define IO_TIMEOUT 5000
#define CONNECT_TIMEOUT 500
#define READ_TIMEOUT_THREAD_JOGADOR 500

#define MIN_JOGADORES_PARA_INICIAR 2
#define MUTEX_ARBITRO_SINGLE _T("ArbitroSingleMutex")

// Estruturas internas do árbitro
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

// Estrutura principal para o estado do servidor (árbitro)
typedef struct {
    JOGADOR_INFO_ARBITRO listaJogadores[MAX_JOGADORES];
    DWORD totalJogadoresAtivos;
    CRITICAL_SECTION csListaJogadores;
    DadosJogoCompartilhados* pDadosShm;
    HANDLE hMapFileShm;
    HANDLE hEventoShmUpdate;
    HANDLE hMutexShm;
    DICIONARIO_ARBITRO dicionario;
    int maxLetrasConfig;
    int ritmoConfigSegundos;
    volatile BOOL servidorEmExecucao;
    volatile BOOL jogoRealmenteAtivo;
    CRITICAL_SECTION csLog;
} SERVER_CONTEXT;

// ESTRUTURA DE ARGUMENTOS PARA AS THREADS
typedef struct {
    SERVER_CONTEXT* serverCtx;
    HANDLE hPipeCliente;
} THREAD_ARGS;

// Protótipos de funções internas
void Log(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...);
void LogError(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...);
void LogWarning(CRITICAL_SECTION* csLogParam, const TCHAR* format, ...);

BOOL InicializarServidor(SERVER_CONTEXT* ctx);
void EncerrarServidor(SERVER_CONTEXT* ctx);
void ConfigurarValoresRegistry(SERVER_CONTEXT* ctx);
BOOL CarregarDicionarioServidor(SERVER_CONTEXT* ctx, const TCHAR* nomeArquivo);
void LiberarDicionarioServidor(SERVER_CONTEXT* ctx);
BOOL InicializarMemoriaPartilhadaArbitro(SERVER_CONTEXT* ctx, int maxLetras);
void LimparMemoriaPartilhadaArbitro(SERVER_CONTEXT* ctx);

DWORD WINAPI ThreadGestorLetras(LPVOID param);
DWORD WINAPI ThreadAdminArbitro(LPVOID param);
DWORD WINAPI ThreadClienteConectado(LPVOID param);

void RemoverJogador(SERVER_CONTEXT* ctx, const TCHAR* username, BOOL notificarClienteParaSair);
int EncontrarJogador(SERVER_CONTEXT* ctx, const TCHAR* username);
void NotificarTodosOsJogadores(SERVER_CONTEXT* ctx, const MESSAGE* msgAEnviar, const TCHAR* skipUsername);
BOOL ValidarPalavraJogo(THREAD_ARGS* argsClienteThread, const TCHAR* palavraSubmetida, const TCHAR* usernameJogador);
void VerificarEstadoJogo(SERVER_CONTEXT* ctx);

#endif // ARBITRO_H