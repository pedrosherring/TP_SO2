#ifndef COMPARTILHADO_H
#define COMPARTILHADO_H

#include <windows.h>
#include <tchar.h>

// ==========================================================================================
// NOMES PARA IPC (Inter-Process Communication)
// ==========================================================================================
#define PIPE_NAME _T("\\\\.\\pipe\\JogoPalavrasSO2Pipe") // Nome do Pipe
#define SHM_NAME _T("JogoPalavrasSO2SharedMem")         // Nome da Memória Partilhada 
#define EVENT_SHM_UPDATE _T("JogoPalavrasSO2SHMUpdateEvent") // Evento para atualização da Memória Partilhada 
#define MUTEX_SHARED_MEM _T("JogoPalavrasSO2MutexSharedMem") // Mutex para proteger acesso à Shared Memory 
#define MUTEX_PLAYER_LIST _T("JogoPalavrasSO2MutexPlayerList") // Mutex para proteger lista de jogadores no árbitro

// ==========================================================================================
// LIMITES E CONFIGURAÇÕES DO JOGO
// ==========================================================================================
#define MAX_USERNAME 32      // Mantido do seu simplificado
#define MAX_WORD 50          // Mantido do seu simplificado
#define MAX_JOGADORES 20    
#define MAX_LETRAS_TABULEIRO 12 // Limite físico do array de letras

// Valores padrão e de configuração do Registry
#define REGISTRY_PATH_TP _T("Software\\TrabSO2_Palavras")
#define REG_MAXLETRAS_NOME _T("MAXLETRAS")
#define REG_RITMO_NOME _T("RITMO")
#define DEFAULT_MAXLETRAS 6
#define DEFAULT_RITMO_SEGUNDOS 3

// ==========================================================================================
// ESTRUTURA PARA MENSAGENS VIA PIPE
// ==========================================================================================
typedef struct {
    TCHAR type[20]; // Aumentado para tipos mais descritivos: "JOIN", "EXIT", "WORD", "GET_SCORE", "GET_JOGS", 
    // Respostas: "JOIN_OK", "JOIN_USER_EXISTS", "JOIN_GAME_FULL", "WORD_VALID", "WORD_INVALID",
    //            "SCORE_UPDATE", "PLAYER_LIST_UPDATE", "GAME_UPDATE", "SHUTDOWN"
    TCHAR username[MAX_USERNAME];
    TCHAR data[MAX_WORD + 256]; // Aumentado para acomodar listas de jogadores ou mensagens mais longas
    int pontos;                 // Usado para pontuação em algumas mensagens
} MESSAGE;

// ==========================================================================================
// ESTRUTURA PARA MEMÓRIA PARTILHADA
// ==========================================================================================
typedef struct {
    TCHAR letrasVisiveis[MAX_LETRAS_TABULEIRO]; // Letras: 'A'-'Z' ou '_' para vazio.
    int numMaxLetrasAtual;                     // Configurado via registry

    // Informação para o painel e para notificações gerais
    TCHAR ultimaPalavraIdentificada[MAX_WORD];
    TCHAR usernameUltimaPalavra[MAX_USERNAME];
    int pontuacaoUltimaPalavra; // Pontos ganhos com a última palavra


    volatile LONG generationCount; // Para clientes detectarem atualizações.
    BOOL jogoAtivo;                // Indica se o jogo está a decorrer (2+ jogadores).
} DadosJogoCompartilhados;

#endif // COMPARTILHADO_H