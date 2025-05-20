#ifndef COMPARTILHADO_H
#define COMPARTILHADO_H

#include <windows.h>
#include <tchar.h>

// ==========================================================================================
// NOMES PARA IPC (Inter-Process Communication)
// ==========================================================================================
#define PIPE_NAME _T("\\\\.\\pipe\\JogoPalavrasSO2Pipe") // Nome do Pipe (usado no TP original)
#define SHM_NAME _T("JogoPalavrasSO2SharedMem")         // Nome da Memória Partilhada (usado no TP original)
#define EVENT_SHM_UPDATE _T("JogoPalavrasSO2SHMUpdateEvent") // Evento para atualização da Memória Partilhada (usado no TP original)
#define MUTEX_SHARED_MEM _T("JogoPalavrasSO2MutexSharedMem") // Mutex para proteger acesso à Shared Memory (novo, recomendado)
#define MUTEX_PLAYER_LIST _T("JogoPalavrasSO2MutexPlayerList") // Mutex para proteger lista de jogadores no árbitro

// ==========================================================================================
// LIMITES E CONFIGURAÇÕES DO JOGO
// ==========================================================================================
#define MAX_USERNAME 32      // Mantido do seu simplificado (PDF sugere 50)
#define MAX_WORD 50          // Mantido do seu simplificado (PDF usa MAX_PALAVRA 50)
#define MAX_JOGADORES 20     // Conforme PDF
#define MAX_LETRAS_TABULEIRO 12 // Limite físico do array de letras, conforme PDF

// Valores padrão e de configuração do Registry (conforme PDF)
#define REGISTRY_PATH_TP TEXT("Software\\TrabSO2_Palavras") // Caminho um pouco mais específico
#define REG_MAXLETRAS_NOME TEXT("MAXLETRAS")
#define REG_RITMO_NOME TEXT("RITMO")
#define DEFAULT_MAXLETRAS 6
#define DEFAULT_RITMO_SEGUNDOS 3

// ==========================================================================================
// ESTRUTURA PARA MENSAGENS VIA PIPE (Simplificada pelo utilizador)
// ==========================================================================================
// Mantendo a estrutura de mensagem simplificada fornecida, mas mapeando os tipos do PDF para strings.
// É importante que o campo 'type' seja usado de forma consistente.
// Para aderir mais estritamente ao PDF, uma enumeração para 'type' e uma união/buffer para 'data'
// seria mais robusto, como na MensagemPipe original. Mas vamos trabalhar com esta.
typedef struct {
    TCHAR type[20]; // Aumentado para tipos mais descritivos: "JOIN", "EXIT", "WORD", "GET_SCORE", "GET_JOGS", 
    // Respostas: "JOIN_OK", "JOIN_USER_EXISTS", "JOIN_GAME_FULL", "WORD_VALID", "WORD_INVALID",
    //            "SCORE_UPDATE", "PLAYER_LIST_UPDATE", "GAME_UPDATE", "SHUTDOWN"
    TCHAR username[MAX_USERNAME];
    TCHAR data[MAX_WORD + 256]; // Aumentado para acomodar listas de jogadores ou mensagens mais longas
    int pontos;                 // Usado para pontuação em algumas mensagens
} MESSAGE;

// ==========================================================================================
// ESTRUTURA PARA MEMÓRIA PARTILHADA (Alinhada com os requisitos do PDF)
// ==========================================================================================
typedef struct {
    TCHAR letrasVisiveis[MAX_LETRAS_TABULEIRO]; // Letras: 'A'-'Z' ou '_' para vazio.
    int numMaxLetrasAtual;                     // Configurado via registry, e.g., 6 (MAXLETRAS do PDF).

    // Informação para o painel e para notificações gerais
    TCHAR ultimaPalavraIdentificada[MAX_WORD];
    TCHAR usernameUltimaPalavra[MAX_USERNAME];
    int pontuacaoUltimaPalavra; // Pontos ganhos com a última palavra

    // Poderia incluir aqui uma pequena lista dos melhores jogadores se necessário para o painel,
    // mas isso aumenta a complexidade da sincronização e do tamanho da SHM.
    // Para já, focamos no essencial.

    volatile LONG generationCount; // Para clientes detectarem atualizações.
    BOOL jogoAtivo;                // Indica se o jogo está a decorrer (2+ jogadores).
} DadosJogoCompartilhados;


// NOTA: O seu 'arbitro.c' simplificado tinha uma formatação de letras com espaços na SHM.
// A estrutura DadosJogoCompartilhados acima armazena apenas as letras. A formatação para display
// ('A B _ C') deve ser feita pelo cliente (jogoui/painel) ao ler 'letrasVisiveis'.

#endif // COMPARTILHADO_H