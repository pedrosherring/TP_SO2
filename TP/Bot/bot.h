#ifndef BOT_H_
#define BOT_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h> 

#include "../Comum/compartilhado.h" 

// Bot's dictionary settings
#define MAX_BOT_DICT_WORDS 20000 // Max words bot can load


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



#endif // BOT_H_