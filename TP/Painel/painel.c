#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tchar.h>
#include <stdio.h> // For sprintf, etc. if needed for display
#include <stdlib.h> // For qsort if sorting players
#include <strsafe.h>

#include "../Comum/compartilhado.h" // Your modified compartilhado.h

// --- Defines for Panel ---
#define ID_TIMER_REFRESH 1 // Timer ID for refreshing data
#define REFRESH_INTERVAL 1000 // Refresh every 1 second (or wait on event)

// Menu IDs
#define IDM_FILE_EXIT 101
#define IDM_OPTIONS_SETMAXPLAYERS 102 // Example
#define IDM_HELP_ABOUT 103

// --- Global Variables for Panel ---
HWND g_hMainWnd = NULL;
DadosJogoCompartilhados* g_pDadosShmPanel = NULL; // Pointer to mapped shared memory
HANDLE g_hMapFileShmPanel = NULL;
HANDLE g_hEventoShmUpdatePanel = NULL;
HANDLE g_hMutexShmPanel = NULL; // Mutex to access shared memory

LONG g_panelLastGenerationCount = -1;
int g_maxPlayersToShow = 5; // Default, configurable via menu

// Cached data from SHM to use in WM_PAINT
TCHAR g_letrasVisiveisCache[MAX_LETRAS_TABULEIRO + MAX_LETRAS_TABULEIRO]; // With spaces
TCHAR g_ultimaPalavraCache[MAX_WORD + MAX_USERNAME + 20]; // "User: Palavra (+X pts)"
PanelPlayerData g_playersCache[MAX_PLAYERS_FOR_PANEL]; // Assuming MAX_PLAYERS_FOR_PANEL from modified SHM
int g_numJogadoresCache = 0;
BOOL g_jogoAtivoCache = FALSE;

// --- Forward Declarations ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
BOOL InitSharedMemoryPanel();
void CleanupSharedMemoryPanel();
void UpdatePanelDataFromSHM();
void DrawPanelInfo(HDC hdc, HWND hWnd); // Main drawing function
void ShowAboutDialog(HWND hWndParent);
// Add dialog procedure for setting max players if you implement that

// --- WinMain ---
int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEX wc;
    MSG msg;
    LPCTSTR szClassName = _T("PainelSO2WindowClass");

    // Register the window class
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL; // We'll create the menu programmatically or via resource
    wc.lpszClassName = szClassName;
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, _T("Window Registration Failed!"), _T("Error!"), MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    // Create the main window
    g_hMainWnd = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        szClassName,
        _T("Painel do Jogo de Palavras SO2"),
        WS_OVERLAPPEDWINDOW, // Standard window
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, // Size and position
        NULL, NULL, hInstance, NULL);

    if (g_hMainWnd == NULL) {
        MessageBox(NULL, _T("Window Creation Failed!"), _T("Error!"), MB_ICONEXCLAMATION | MB_OK);
        return 0;
    }

    // Initialize Shared Memory
    if (!InitSharedMemoryPanel()) {
        MessageBox(g_hMainWnd, _T("Falha ao inicializar memória partilhada! O painel não pode obter dados."), _T("Erro de Memória Partilhada"), MB_OK | MB_ICONERROR);
        // Decide if the panel should run without SHM or exit.
        // For now, let it run but it won't show data.
    }

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    // Set a timer to refresh data periodically
    // Alternatively, use a dedicated thread to wait on g_hEventoShmUpdatePanel
    SetTimer(g_hMainWnd, ID_TIMER_REFRESH, REFRESH_INTERVAL, NULL);

    // Message loop
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CleanupSharedMemoryPanel();
    KillTimer(g_hMainWnd, ID_TIMER_REFRESH);
    return (int)msg.wParam;
}

// --- Window Procedure ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
    {
        // Create Menu
        HMENU hMenubar = CreateMenu();
        HMENU hMenuFile = CreateMenu();
        HMENU hMenuOptions = CreateMenu();
        HMENU hMenuHelp = CreateMenu();

        AppendMenu(hMenubar, MF_POPUP, (UINT_PTR)hMenuFile, _T("&Ficheiro"));
        AppendMenu(hMenubar, MF_POPUP, (UINT_PTR)hMenuOptions, _T("&Opções"));
        AppendMenu(hMenubar, MF_POPUP, (UINT_PTR)hMenuHelp, _T("&Ajuda"));

        AppendMenu(hMenuFile, MF_STRING, IDM_FILE_EXIT, _T("&Sair"));
        AppendMenu(hMenuOptions, MF_STRING, IDM_OPTIONS_SETMAXPLAYERS, _T("Definir &Máx Jogadores Visíveis..."));
        AppendMenu(hMenuHelp, MF_STRING, IDM_HELP_ABOUT, _T("&Sobre..."));

        SetMenu(hWnd, hMenubar);
    }
    break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_FILE_EXIT:
            PostMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        case IDM_OPTIONS_SETMAXPLAYERS:
            // TODO: Implement a dialog to get new g_maxPlayersToShow
            MessageBox(hWnd, _T("Funcionalidade para definir máx jogadores a implementar."), _T("Info"), MB_OK);
            break;
        case IDM_HELP_ABOUT:
            ShowAboutDialog(hWnd);
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == ID_TIMER_REFRESH) {
            UpdatePanelDataFromSHM(); // Fetch new data
            InvalidateRect(hWnd, NULL, TRUE); // Request a repaint
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawPanelInfo(hdc, hWnd); // Call your drawing function
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_ERASEBKGND:
        return 1; // Handle background erasing to reduce flicker

    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// --- Shared Memory Functions ---
BOOL InitSharedMemoryPanel() {
    // Open existing File Mapping
    g_hMapFileShmPanel = OpenFileMapping(
        FILE_MAP_READ,          // Read access
        FALSE,                  // Do not inherit handle
        SHM_NAME);              // Name of mapping object

    if (g_hMapFileShmPanel == NULL) {
        _tprintf(_T("Painel: Erro ao abrir FileMapping (%s): %lu\n"), SHM_NAME, GetLastError());
        return FALSE;
    }

    // Map view of file
    g_pDadosShmPanel = (DadosJogoCompartilhados*)MapViewOfFile(
        g_hMapFileShmPanel,     // Handle to map object
        FILE_MAP_READ,          // Read access
        0,                      // High-order DWORD of file offset
        0,                      // Low-order DWORD of file offset
        sizeof(DadosJogoCompartilhados)); // Number of bytes to map

    if (g_pDadosShmPanel == NULL) {
        _tprintf(_T("Painel: Erro ao mapear SHM (%s): %lu\n"), SHM_NAME, GetLastError());
        CloseHandle(g_hMapFileShmPanel);
        g_hMapFileShmPanel = NULL;
        return FALSE;
    }

    // Open Mutex for synchronization
    g_hMutexShmPanel = OpenMutex(
        SYNCHRONIZE,            // Request synchronize access
        FALSE,                  // Do not inherit handle
        MUTEX_SHARED_MEM);      // Name of mutex

    if (g_hMutexShmPanel == NULL) {
        _tprintf(_T("Painel: Erro ao abrir Mutex SHM (%s): %lu. Dados podem ser inconsistentes.\n"), MUTEX_SHARED_MEM, GetLastError());
        // Non-fatal, but warn user or log
    }

    // Open Event for update notifications (optional if using timer, but good for responsiveness)
    g_hEventoShmUpdatePanel = OpenEvent(
        SYNCHRONIZE,            // Need to wait on it
        FALSE,
        EVENT_SHM_UPDATE);      // Name of event

    if (g_hEventoShmUpdatePanel == NULL) {
        _tprintf(_T("Painel: Erro ao abrir Evento SHM (%s): %lu. Usará apenas timer.\n"), EVENT_SHM_UPDATE, GetLastError());
    }


    _tprintf(_T("Painel: Memória partilhada e objetos de sync abertos.\n"));
    UpdatePanelDataFromSHM(); // Initial data fetch
    return TRUE;
}

void CleanupSharedMemoryPanel() {
    if (g_pDadosShmPanel != NULL) {
        UnmapViewOfFile(g_pDadosShmPanel);
        g_pDadosShmPanel = NULL;
    }
    if (g_hMapFileShmPanel != NULL) {
        CloseHandle(g_hMapFileShmPanel);
        g_hMapFileShmPanel = NULL;
    }
    if (g_hMutexShmPanel != NULL) {
        CloseHandle(g_hMutexShmPanel);
        g_hMutexShmPanel = NULL;
    }
    if (g_hEventoShmUpdatePanel != NULL) {
        CloseHandle(g_hEventoShmUpdatePanel);
        g_hEventoShmUpdatePanel = NULL;
    }
    _tprintf(_T("Painel: Recursos de memória partilhada libertados.\n"));
}

// --- Data Update and Drawing ---
// Comparison function for qsort (to sort players by score descending)
int ComparePlayers(const void* a, const void* b) {
    PanelPlayerData* playerA = (PanelPlayerData*)a;
    PanelPlayerData* playerB = (PanelPlayerData*)b;
    // For descending order
    if (playerB->pontos > playerA->pontos) return 1;
    if (playerB->pontos < playerA->pontos) return -1;
    return 0;
}


void UpdatePanelDataFromSHM() {
    if (g_pDadosShmPanel == NULL) return;
    if (g_hMutexShmPanel == NULL) {
        // Attempt to read without mutex if it failed to open, data might be inconsistent
        _tprintf(_T("Aviso: Lendo SHM sem mutex.\n"));
    }
    else {
        DWORD dwWaitResult = WaitForSingleObject(g_hMutexShmPanel, 500); // Wait max 0.5 sec
        if (dwWaitResult != WAIT_OBJECT_0) {
            _tprintf(_T("Aviso: Timeout ou falha ao esperar pelo mutex da SHM. Dados podem não ser atualizados ou ser inconsistentes.\n"));
            return; // Don't update if can't get mutex in reasonable time
        }
    }

    // Check generation count to see if data has changed
    if (g_pDadosShmPanel->generationCount == g_panelLastGenerationCount && g_panelLastGenerationCount != -1) {
        if (g_hMutexShmPanel != NULL) ReleaseMutex(g_hMutexShmPanel);
        return; // No change
    }
    g_panelLastGenerationCount = g_pDadosShmPanel->generationCount;

    // Copy data to local cache
    // Letras Visiveis
    ZeroMemory(g_letrasVisiveisCache, sizeof(g_letrasVisiveisCache));
    for (int i = 0; i < g_pDadosShmPanel->numMaxLetrasAtual && i < MAX_LETRAS_TABULEIRO; ++i) {
        TCHAR letterStr[3];
        StringCchPrintf(letterStr, _countof(letterStr), _T("%c "), g_pDadosShmPanel->letrasVisiveis[i]);
        StringCchCat(g_letrasVisiveisCache, _countof(g_letrasVisiveisCache), letterStr);
    }

    // Ultima Palavra
    if (g_pDadosShmPanel->ultimaPalavraIdentificada[0] != _T('\0')) {
        StringCchPrintf(g_ultimaPalavraCache, _countof(g_ultimaPalavraCache), _T("Última: %s por %s (+%d pts)"),
            g_pDadosShmPanel->ultimaPalavraIdentificada,
            g_pDadosShmPanel->usernameUltimaPalavra,
            g_pDadosShmPanel->pontuacaoUltimaPalavra);
    }
    else {
        StringCchCopy(g_ultimaPalavraCache, _countof(g_ultimaPalavraCache), _T("Última: (Nenhuma)"));
    }

    g_jogoAtivoCache = g_pDadosShmPanel->jogoAtivo;

    // Players (assuming painelJogadores and numJogadoresParaPainel exist in your modified SHM struct)
    g_numJogadoresCache = 0;
    for (int i = 0; i < g_pDadosShmPanel->numJogadoresParaPainel && i < MAX_PLAYERS_FOR_PANEL; ++i) {
        if (g_pDadosShmPanel->painelJogadores[i].ativo) { // Check if player is active
            g_playersCache[g_numJogadoresCache++] = g_pDadosShmPanel->painelJogadores[i];
        }
    }
    // Sort players by score (descending)
    if (g_numJogadoresCache > 0) {
        qsort(g_playersCache, g_numJogadoresCache, sizeof(PanelPlayerData), ComparePlayers);
    }


    if (g_hMutexShmPanel != NULL) ReleaseMutex(g_hMutexShmPanel);
    _tprintf(_T("Painel: Dados atualizados da SHM. Geração: %ld\n"), g_panelLastGenerationCount);
}

void DrawPanelInfo(HDC hdc, HWND hWnd) {
    RECT clientRect;
    GetClientRect(hWnd, &clientRect);

    // Use a compatible DC for double buffering to reduce flicker
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdc, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
    HANDLE hOld = SelectObject(hdcMem, hbmMem);

    // Erase background (or fill with a color)
    FillRect(hdcMem, &clientRect, (HBRUSH)(COLOR_WINDOW + 1));

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdcMem, hFont);

    int yPos = 10;
    int xMargin = 10;
    int lineHeight = 20;

    SetTextColor(hdcMem, RGB(0, 0, 0)); // Black text

    // 1. Jogo Ativo
    TCHAR jogoAtivoStr[50];
    StringCchPrintf(jogoAtivoStr, _countof(jogoAtivoStr), _T("Jogo Ativo: %s"), g_jogoAtivoCache ? _T("SIM") : _T("NÃO"));
    TextOut(hdcMem, xMargin, yPos, jogoAtivoStr, (int)_tcslen(jogoAtivoStr));
    yPos += lineHeight;

    // 2. Letras Visíveis
    TextOut(hdcMem, xMargin, yPos, _T("Letras Visíveis:"), (int)_tcslen(_T("Letras Visíveis:")));
    yPos += lineHeight;
    TextOut(hdcMem, xMargin + 20, yPos, g_letrasVisiveisCache, (int)_tcslen(g_letrasVisiveisCache));
    yPos += lineHeight * 2; // Extra space

    // 3. Última Palavra
    TextOut(hdcMem, xMargin, yPos, g_ultimaPalavraCache, (int)_tcslen(g_ultimaPalavraCache));
    yPos += lineHeight * 2;

    // 4. Lista de Jogadores
    TextOut(hdcMem, xMargin, yPos, _T("Jogadores (Top %d):"), g_maxPlayersToShow);
    yPos += lineHeight;

    for (int i = 0; i < g_numJogadoresCache && i < g_maxPlayersToShow; ++i) {
        TCHAR playerLine[MAX_USERNAME + 50];
        // Assuming pontos is float in PanelPlayerData for this example
        StringCchPrintf(playerLine, _countof(playerLine), _T("%d. %s - %.1f pontos"),
            i + 1,
            g_playersCache[i].username,
            g_playersCache[i].pontos);
        TextOut(hdcMem, xMargin + 20, yPos, playerLine, (int)_tcslen(playerLine));
        yPos += lineHeight;
    }
    if (g_numJogadoresCache == 0) {
        TextOut(hdcMem, xMargin + 20, yPos, _T("(Nenhum jogador)"), (int)_tcslen(_T("(Nenhum jogador)")));
    }

    SelectObject(hdcMem, hOldFont); // Restore old font

    // Copy from memory DC to screen DC
    BitBlt(hdc, 0, 0, clientRect.right - clientRect.left, clientRect.bottom - clientRect.top, hdcMem, 0, 0, SRCCOPY);

    // Clean up GDI objects
    SelectObject(hdcMem, hOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}


// --- Dialogs ---
void ShowAboutDialog(HWND hWndParent) {
    // Simpler to use MessageBox for "About" unless complex layout is needed
    TCHAR aboutText[256];
    StringCchPrintf(aboutText, _countof(aboutText),
        _T("Painel Jogo de Palavras SO2\n\n")
        _T("Desenvolvido por:\n")
        _T("[Seu Nome/Nomes do Grupo Aqui]\n")
        _T("Engenharia Informática - DEIS ISEC\n")
        _T("Sistemas Operativos 2 - 2024/25"));
    MessageBox(hWndParent, aboutText, _T("Sobre o Painel SO2"), MB_OK | MB_ICONINFORMATION);
}