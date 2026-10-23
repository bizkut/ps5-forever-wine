/* A title prefix must not be published as initialized after an interrupted
 * RegisterDlls pass.  Inspect normal registrations; never replace a signature
 * result or choose a file/signer exception.  Called before the done marker and
 * before any application can cache missing Wintrust provider callbacks. */
#ifndef WINEBOOT_PS5_CRYPTO_H
#define WINEBOOT_PS5_CRYPTO_H

#include <wintrust.h>
#include <softpub.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "wine/ps5_prefix_init.h"

static BOOL wineboot_crypto_registration_ready(void)
{
    static const struct
    {
        const WCHAR *key, *function;
        const char *proc;
        BOOL sip;
    } entries[] = {
        {L"Initialization", SP_INIT_FUNCTION, "SoftpubInitialize", FALSE},
        {L"Message", SP_OBJTRUST_FUNCTION, "SoftpubLoadMessage", FALSE},
        {L"Signature", SP_SIGTRUST_FUNCTION, "SoftpubLoadSignature", FALSE},
        {L"Certificate", WT_PROVIDER_CERTTRUST_FUNCTION, "WintrustCertificateTrust", FALSE},
        {L"CertCheck", SP_CHKCERT_FUNCTION, "SoftpubCheckCert", FALSE},
        {L"FinalPolicy", SP_FINALPOLICY_FUNCTION, "SoftpubAuthenticode", FALSE},
        {L"Cleanup", SP_CLEANUPPOLICY_FUNCTION, "SoftpubCleanup", FALSE},
        {L"GetSignedDataMsg", L"CryptSIPGetSignedDataMsg", "CryptSIPGetSignedDataMsg", TRUE},
        {L"PutSignedDataMsg", L"CryptSIPPutSignedDataMsg", "CryptSIPPutSignedDataMsg", TRUE},
        {L"CreateIndirectData", L"CryptSIPCreateIndirectData", "CryptSIPCreateIndirectData", TRUE},
        {L"VerifyIndirectData", L"CryptSIPVerifyIndirectData", "CryptSIPVerifyIndirectData", TRUE},
        {L"RemoveSignedDataMsg", L"CryptSIPRemoveSignedDataMsg", "CryptSIPRemoveSignedDataMsg", TRUE},
    };
    static const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    WCHAR path[256], value[MAX_PATH];
    unsigned int view, i;
    HMODULE module;
    BOOL ready = TRUE;

    /* Both architectures are installed by this x86-64 title's wineboot.  A
     * successful native pass is not evidence of a completed Wow64Install. */
    for (view = 0; view < ARRAY_SIZE(views); ++view)
        for (i = 0; i < ARRAY_SIZE(entries); ++i)
        {
            HKEY key;
            DWORD type = 0, size = sizeof(value);
            LONG error;

            if (entries[i].sip)
                swprintf(path, ARRAY_SIZE(path),
                         L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDll%ls\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", entries[i].key);
            else
                swprintf(path, ARRAY_SIZE(path),
                         L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\%ls\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", entries[i].key);
            if ((error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | views[view], &key)))
                return FALSE;
            error = RegQueryValueExW(key, entries[i].sip ? L"Dll" : L"$DLL", NULL,
                                     &type, (BYTE *)value, &size);
            ready = !error && type == REG_SZ && size >= sizeof(WCHAR) &&
                    !(size % sizeof(WCHAR)) && !value[size / sizeof(WCHAR) - 1] &&
                    !wcsicmp(value, SP_POLICY_PROVIDER_DLL_NAME);
            size = sizeof(value);
            if (ready)
            {
                error = RegQueryValueExW(key, entries[i].sip ? L"FuncName" : L"$Function", NULL,
                                         &type, (BYTE *)value, &size);
                ready = !error && type == REG_SZ && size >= sizeof(WCHAR) &&
                        !(size % sizeof(WCHAR)) && !value[size / sizeof(WCHAR) - 1] &&
                        !wcscmp(value, entries[i].function);
            }
            RegCloseKey(key);
            if (!ready) return FALSE;
        }
    /* Setup's native and Wow64 children load their own DLL while registering;
     * also prove this process can resolve every normal provider entry point. */
    if (!(module = LoadLibraryW(SP_POLICY_PROVIDER_DLL_NAME))) return FALSE;
    for (i = 0; i < ARRAY_SIZE(entries); ++i)
        if (!GetProcAddress(module, entries[i].proc))
        {
            ready = FALSE;
            break;
        }
    FreeLibrary(module);
    return ready;
}

/* A real key is required: predefined HKLM is not a native registry handle.
 * The title server flushes its registered hives and reports any save failure.
 * Directory metadata is synchronized by the native proof publisher after
 * quiescence, before it certifies the persistent checkpoint. */
static BOOL wineboot_flush_crypto_registry(void)
{
    static const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    unsigned int view;

    for (view = 0; view < ARRAY_SIZE(views); ++view)
    {
        HKEY key;
        LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software", 0, KEY_READ | views[view], &key);
        if (error)
        {
            SetLastError(error);
            return FALSE;
        }
        error = RegFlushKey(key);
        RegCloseKey(key);
        if (error)
        {
            SetLastError(error);
            return FALSE;
        }
    }
    return TRUE;
}

static inline BOOL wineboot_finish_title_prefix(const WCHAR *config_dir, BOOL installed)
{
    WCHAR *file;
    int fd;
    BOOL written = FALSE;

    if (!installed || !config_dir || !wineboot_crypto_registration_ready() ||
        !wineboot_flush_crypto_registry()) return FALSE;
    if (!(file = malloc(wcslen(config_dir) * sizeof(WCHAR) + sizeof(L"\\.wineboot-init-done"))))
        return FALSE;
    lstrcpyW(file, config_dir);
    lstrcatW(file, L"\\.wineboot-init-done");
    if ((fd = _wopen(file, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666)) != -1)
    {
        written = write(fd, WINE_PS5_PREFIX_INIT_PROOF, sizeof(WINE_PS5_PREFIX_INIT_PROOF) - 1)
                  == (int)(sizeof(WINE_PS5_PREFIX_INIT_PROOF) - 1);
        if (close(fd)) written = FALSE;
        if (!written) _wunlink(file);
    }
    free(file);
    return written;
}

#endif
