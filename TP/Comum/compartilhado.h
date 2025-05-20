#ifndef COMPARTILHADO_H
#define COMPARTILHADO_H

#include <windows.h>
#include <tchar.h>

// ==========================================================================================
// NOMES PARA IPC (Inter-Process Communication)
// ==========================================================================================
#define PIPE_NAME _T("\\\\.\\pipe\\JogoPalavrasSO2Pipe") // Nome do Pipe (usado no TP original)
#define SHM_NAME _T("JogoPalavrasSO2SharedMem")         // Nome da Mem�ria Partilhada (usado no TP original)
#define EVENT_SHM_UPDATE _T("JogoPalavrasSO2SHMUpdateEvent") // Evento para atualiza��o da Mem�ria Partilhada (usado no TP original)
#define MUTEX_SHARED_MEM _T("JogoPalavrasSO2MutexSharedMem") // Mutex para proteger acesso � Shared Memory (novo, recomendado)
#define MUTEX_PLAYER_LIST _T("JogoPalavrasSO2MutexPlayerList") // Mutex para proteger lista de jogadores no �rbitro

// ==========================================================================================
// LIMITES E CONFIGURA��ES DO JOGO
// ==========================================================================================
#define MAX_USERNAME 32      // Mantido do seu simplificado (PDF sugere 50)
#define MAX_WORD 50          // Mantido do seu simplificado (PDF usa MAX_PALAVRA 50)
#define MAX_JOGADORES 20     // Conforme PDF
#define MAX_LETRAS_TABULEIRO 12 // Limite f�sico do array de letras, conforme PDF

// Valores padr�o e de configura��o do Registry (conforme PDF)
#define REGISTRY_PATH_TP TEXT("Software\\TrabSO2_Palavras") // Caminho um pouco mais espec�fico
#define REG_MAXLETRAS_NOME TEXT("MAXLETRAS")
#define REG_RITMO_NOME TEXT("RITMO")
#define DEFAULT_MAXLETRAS 6
#define DEFAULT_RITMO_SEGUNDOS 3

// ==========================================================================================
// ESTRUTURA PARA MENSAGENS VIA PIPE (Simplificada pelo utilizador)
// ==========================================================================================
// Mantendo a estrutura de mensagem simplificada fornecida, mas mapeando os tipos do PDF para strings.
// � importante que o campo 'type' seja usado de forma consistente.
// Para aderir mais estritamente ao PDF, uma enumera��o para 'type' e uma uni�o/buffer para 'data'
// seria mais robusto, como na MensagemPipe original. Mas vamos trabalhar com esta.
typedef struct {
    TCHAR type[20]; // Aumentado para tipos mais descritivos: "JOIN", "EXIT", "WORD", "GET_SCORE", "GET_JOGS", 
    // Respostas: "JOIN_OK", "JOIN_USER_EXISTS", "JOIN_GAME_FULL", "WORD_VALID", "WORD_INVALID",
    //            "SCORE_UPDATE", "PLAYER_LIST_UPDATE", "GAME_UPDATE", "SHUTDOWN"
    TCHAR username[MAX_USERNAME];
    TCHAR data[MAX_WORD + 256]; // Aumentado para acomodar listas de jogadores ou mensagens mais longas
    int pontos;                 // Usado para pontua��o em algumas mensagens
} MESSAGE;

// ==========================================================================================
// ESTRUTURA PARA MEM�RIA PARTILHADA (Alinhada com os requisitos do PDF)
// ==========================================================================================
typedef struct {
    TCHAR letrasVisiveis[MAX_LETRAS_TABULEIRO]; // Letras: 'A'-'Z' ou '_' para vazio.
    int numMaxLetrasAtual;                     // Configurado via registry, e.g., 6 (MAXLETRAS do PDF).

    // Informa��o para o painel e para notifica��es gerais
    TCHAR ultimaPalavraIdentificada[MAX_WORD];
    TCHAR usernameUltimaPalavra[MAX_USERNAME];
    int pontuacaoUltimaPalavra; // Pontos ganhos com a �ltima palavra

    // Poderia incluir aqui uma pequena lista dos melhores jogadores se necess�rio para o painel,
    // mas isso aumenta a complexidade da sincroniza��o e do tamanho da SHM.
    // Para j�, focamos no essencial.

    volatile LONG generationCount; // Para clientes detectarem atualiza��es.
    BOOL jogoAtivo;                // Indica se o jogo est� a decorrer (2+ jogadores).
} DadosJogoCompartilhados;


// NOTA: O seu 'arbitro.c' simplificado tinha uma formata��o de letras com espa�os na SHM.
// A estrutura DadosJogoCompartilhados acima armazena apenas as letras. A formata��o para display
// ('A B _ C') deve ser feita pelo cliente (jogoui/painel) ao ler 'letrasVisiveis'.

#endif // COMPARTILHADO_H