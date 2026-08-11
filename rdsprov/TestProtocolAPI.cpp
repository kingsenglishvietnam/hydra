#include "TestProtocolAPI.h"

/* Credentials come from the listener's own registry key, not from source.
 *
 * The sample hardcoded testuser / DontUseThis1 here. A real password in this
 * file would end up in git -- this tree is a clone of a public repo. The
 * RDP-Tcp listener key already carries Username / Password / Domain value
 * entries, so this is the pattern termsrv's own protocol uses.
 *
 * Still plaintext at rest, protected only by the key's ACL. Deliberate
 * trade for a single local teaching account; Kerberos would be the correct
 * answer for anything larger. */
static const WCHAR* HYDRA_LISTENER_KEY =
    L"System\\CurrentControlSet\\Control\\Terminal Server\\WinStations\\HydraProto";

static BOOL HydraReadCred(LPCWSTR valueName, WCHAR* out, DWORD cchOut)
{
    DWORD cb = cchOut * (DWORD)sizeof(WCHAR);
    LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, HYDRA_LISTENER_KEY, valueName,
                              RRF_RT_REG_SZ, NULL, out, &cb);
    if (st != ERROR_SUCCESS) { out[0] = 0; return FALSE; }
    return TRUE;
}



DWORD WINAPI WaitToConnect(LPVOID lpParameter)
{
    IWRdsProtocolListenerCallback* pListenerCallback;

    pListenerCallback = (IWRdsProtocolListenerCallback*)lpParameter;
    CONNECTION_CONFIG newConnectionConfig;
    /* Read from the listener key. Refuse to connect rather than falling back
     * to anything hardcoded -- a silent default would be worse than a failure. */
    if (!HydraReadCred(L"Username", newConnectionConfig.UserName, MAX_STR_SIZE) ||
        !HydraReadCred(L"Password", newConnectionConfig.Password, MAX_STR_SIZE))
    {
        OutputDebugStringW(L"[hydraproto] no Username/Password under the listener key"
                           L" -- refusing to create a connection\n");
        return FALSE;
    }
    if (!HydraReadCred(L"Domain", newConnectionConfig.Domain, MAX_STR_SIZE))
        newConnectionConfig.Domain[0] = 0;

    CONNECTION_OUTPUT newConnectionOutput;
    //Wait for the presence of a file to simulate a remote connection
    //For real protocols this would usually happen when a TCP/UDP socket connection is recieved
    bool retval = PathFileExistsW(L"C:\\TestProtocol\\createconnection.txt");
    while(!retval)
    {
        Sleep(5000);
        retval = PathFileExistsW(L"C:\\TestProtocol\\createconnection.txt");
    }

    CreateTestConnection(pListenerCallback, &newConnectionConfig, &newConnectionOutput);

    return TRUE;
}

HRESULT CreateTestConnection(
    IWRdsProtocolListenerCallback* pListenerCallback,
    PCONNECTION_CONFIG pConnectionConfig,
    PCONNECTION_OUTPUT pConnectionOutput
)
{
    HRESULT hr;
    ULONG ulConnectionId;
    WRDS_CONNECTION_SETTINGS WRdsConnectionSettings;
    IWRdsProtocolConnectionCallback* localConnectionCallback = NULL;

    ZeroMemory(&WRdsConnectionSettings, sizeof(WRdsConnectionSettings));

    CComObject<CWRdsProtocolConnection>* pConnection = NULL;
    hr = CComObject<CWRdsProtocolConnection>::CreateInstance(&pConnection);
    if (FAILED(hr))
    {
        return E_FAIL;
    }
    if (NULL == pConnection)
    {
        return E_POINTER;
    }

    // Set credentials
    pConnection->SetCredentials(pConnectionConfig->Domain, pConnectionConfig->UserName, pConnectionConfig->Password);

    // Connections will fail if these aren't set
    WRdsConnectionSettings.WRdsConnectionSettingLevel = WRDS_CONNECTION_SETTING_LEVEL_1;
    WRdsConnectionSettings.WRdsConnectionSetting.WRdsConnectionSettings1.WRdsListenerSettings.WRdsListenerSettingLevel = WRDS_LISTENER_SETTING_LEVEL_1;
    WRdsConnectionSettings.WRdsConnectionSetting.WRdsConnectionSettings1.WRdsListenerSettings.WRdsListenerSetting.WRdsListenerSettings1.pSecurityDescriptor = NULL;
    // Inform termsrv that there is a new connection
    hr = pListenerCallback->OnConnected(pConnection, &WRdsConnectionSettings, &localConnectionCallback);
    if (FAILED(hr))
    {
        return E_FAIL;
    }

    hr = localConnectionCallback->GetConnectionId(&ulConnectionId);
    if (FAILED(hr))
    {
        return E_FAIL;
    }
    pConnectionOutput->ulConnectionId = ulConnectionId;
    pConnection->SetConnectionCallback(localConnectionCallback);

    hr = localConnectionCallback->OnReady();
    if (FAILED(hr))
    {
        return E_FAIL;
    }

    return S_OK;
}

