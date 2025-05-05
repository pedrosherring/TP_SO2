#include <stdio.h>
#include <windows.h>
#include <fcntl.h>
#include <tchar.h>
#include <io.h>

#define MAXLETRAS_PADRAO 6
#define RITMO_PADRAO 3
#define MAXLETRAS_MAXIMO 12

DWORD WINAPI ConfigurarRegistro(DWORD* maxLetras, DWORD* ritmo) {
    HKEY hKey;
    DWORD tamanho = sizeof(DWORD);
    LONG resultado;

    // Inicializa com valores padrão
    *maxLetras = MAXLETRAS_PADRAO;
    *ritmo = RITMO_PADRAO;

    // Tenta abrir a chave do registro
    resultado = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\TrabSO2", 0, KEY_READ, &hKey);
    if (resultado == ERROR_SUCCESS) {
        // Lê MAXLETRAS
        resultado = RegQueryValueEx(hKey, L"MAXLETRAS", NULL, NULL, (LPBYTE)maxLetras, &tamanho);
        if (resultado != ERROR_SUCCESS) {
            *maxLetras = MAXLETRAS_PADRAO;
        }

        // Lê RITMO
        tamanho = sizeof(DWORD);
        resultado = RegQueryValueEx(hKey, L"RITMO", NULL, NULL, (LPBYTE)ritmo, &tamanho);
        if (resultado != ERROR_SUCCESS) {
            *ritmo = RITMO_PADRAO;
        }

        RegCloseKey(hKey);
    }
    else {
        // Chave não existe, cria-a com valores padrão
        resultado = RegCreateKey(HKEY_CURRENT_USER, L"Software\\TrabSO2", &hKey);
        if (resultado == ERROR_SUCCESS) {
            RegSetValueEx(hKey, L"MAXLETRAS", 0, REG_DWORD, (LPBYTE)maxLetras, sizeof(DWORD));
            RegSetValueEx(hKey, L"RITMO", 0, REG_DWORD, (LPBYTE)ritmo, sizeof(DWORD));
            RegCloseKey(hKey);
        }
        else {
            _tprintf(_T("Erro ao criar chave do registro: %ld\n"), resultado);
        }
    }

    // Valida MAXLETRAS
    if (*maxLetras > MAXLETRAS_MAXIMO) {
        *maxLetras = MAXLETRAS_MAXIMO;
        resultado = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\TrabSO2", 0, KEY_WRITE, &hKey);
        if (resultado == ERROR_SUCCESS) {
            RegSetValueEx(hKey, L"MAXLETRAS", 0, REG_DWORD, (LPBYTE)maxLetras, sizeof(DWORD));
            RegCloseKey(hKey);
        }
    }
}

int _tmain(int argc, LPTSTR argv[]) {
    DWORD maxLetras, ritmo;

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    ConfigurarRegistro(&maxLetras, &ritmo);
    _tprintf(_T("[ARBITRO] Jogo iniciado com MAXLETRAS=%lu, RITMO=%lu\n"), maxLetras, ritmo);

    return 0;
}

