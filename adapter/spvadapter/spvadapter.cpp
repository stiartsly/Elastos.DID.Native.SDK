#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>

#include <MasterWalletManager.h>
#include <IMasterWallet.h>
#include <ISubWallet.h>
#include <IMainchainSubWallet.h>
#include <IIDChainSubWallet.h>

#include <iostream>
#include <string>
#include <fstream>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "spvadapter.h"

using namespace Elastos::ElaWallet;

static const char *ID_CHAIN = "IDChain";

class SubWalletCallback;

struct SpvDidAdapter {
    bool synced;
    IMasterWalletManager *manager;
    IMasterWallet *masterWallet;
    IIDChainSubWallet *idWallet;
    SubWalletCallback *callback;
    char *resolver;
};

enum TransactionStatus { DELETED, ADDED, UPDATED };

class TransactionCallback {
private:
    TransactionStatus status;
    bool confirm;
    SpvTransactionCallback *callback;
    void *context;

public:
    ~TransactionCallback() {}
    TransactionCallback() :
        status(DELETED), confirm(false), callback(NULL),  context(NULL) {}
    TransactionCallback(bool _confirm, SpvTransactionCallback *_callback, void *_context) :
        status(DELETED), confirm(_confirm), callback(_callback), context(_context) {}

    bool needConfirm() {
        return confirm;
    }

    void setStatus(TransactionStatus _status) {
        status = _status;
    }

    TransactionStatus getStatus(void) {
        return status;
    }

    void success(const std::string txid) {
        callback(txid.c_str(), 0, NULL, context);
    }

    void failed(const std::string txid, int status, std::string &msg) {
        callback(txid.c_str(), status, msg.c_str(), context);
    }
};

class SubWalletCallback : public ISubWalletCallback {
private:
    SpvDidAdapter *adapter;
    std::map<const std::string, TransactionCallback> txCallbacks;

    void RemoveTransactionCallback(const std::string &tx) {
        txCallbacks.erase(tx);
    }

public:
    ~SubWalletCallback() {
        txCallbacks.clear();
    }

    SubWalletCallback(SpvDidAdapter *_adapter) : adapter(_adapter) {}

    virtual void OnBlockSyncStarted() {
    }

    virtual void OnBlockSyncProgress(const nlohmann::json &progressInfo) {
        if (progressInfo["Progress"] == 100)
            adapter->synced = true;
    }

    virtual void OnBlockSyncStopped() {
    }

    virtual void OnBalanceChanged(const std::string &asset,
            const std::string &balance) {
    }

    virtual void OnTransactionStatusChanged(
            const std::string &txid, const std::string &status,
            const nlohmann::json &desc, uint32_t confirms) {
        if (txCallbacks.find(txid) == txCallbacks.end())
            return;

        // Ignore the other status except Updated when first confirm.
        TransactionCallback &callback = txCallbacks[txid];

        if (status.compare("Updated") == 0) {
            if (callback.needConfirm() && confirms == 1) {
                callback.success(txid);
                RemoveTransactionCallback(txid);
            } else {
                callback.setStatus(UPDATED);
            }
        }
    }

    virtual void OnTxPublished(const std::string &txid,
            const nlohmann::json &result) {
        if (txCallbacks.find(txid) == txCallbacks.end())
            return;

        TransactionCallback &callback = txCallbacks[txid];
        int rc = result["Code"];
        if (rc == 0) {
            if (callback.getStatus() == UPDATED && !callback.needConfirm()) {
                callback.success(txid);
                RemoveTransactionCallback(txid);
            }
        } else {
            std::string msg = result["Reason"];
            callback.failed(txid, rc, msg);
            RemoveTransactionCallback(txid);
        }
    }

    virtual void OnAssetRegistered(const std::string &asset,
            const nlohmann::json &info) {
    }

    virtual void OnConnectStatusChanged(const std::string &status) {
    }

    void RegisterTransactionCallback(const std::string &tx, bool confirm,
            SpvTransactionCallback *callback, void *context) {
        TransactionCallback txCallback(confirm, callback, context);
        txCallbacks[tx] = txCallback;
    }
};

class Semaphore {
public:
    Semaphore (int _count = 0) : count(_count) {}

    inline void notify() {
        std::unique_lock<std::mutex> lock(mtx);
        count++;
        cv.notify_one();
    }

    inline void wait() {
        std::unique_lock<std::mutex> lock(mtx);
        while (count == 0)
            cv.wait(lock);

        count--;
    }

private:
    std::mutex mtx;
    std::condition_variable cv;
    int count;
};

static void SyncStart(SpvDidAdapter *adapter)
{
    auto subWallets = adapter->masterWallet->GetAllSubWallets();
    for (auto it = subWallets.begin(); it != subWallets.end(); ++it) {
        try {
           if ((*it)->GetChainID() == ID_CHAIN)
                (*it)->AddCallback(adapter->callback);
            (*it)->SyncStart();
        } catch (...) {
            // ignore
        }
    }

    while (!adapter->synced) {
        // Waiting for sync to latest block height.
        sleep(1);
    }

}

static void SyncStop(SpvDidAdapter *adapter)
{
    auto subWallets = adapter->masterWallet->GetAllSubWallets();
    for (auto it = subWallets.begin(); it != subWallets.end(); ++it) {
        try {
            if ((*it)->GetChainID() == ID_CHAIN)
               (*it)->SyncStop();
            (*it)->RemoveCallback();
        } catch (...) {
            // ignore
        }
    }

    try {
        adapter->manager->FlushData();
    } catch (...) {
        // ignore
    }
}

SpvDidAdapter *SpvDidAdapter_Create(const char *walletDir, const char *walletId,
        const char *network, const char *resolver)
{
    nlohmann::json netConfig;
    IMasterWallet *masterWallet;
    int syncState = 0;
    char *url = NULL;

    if (!walletDir || !walletId)
        return NULL;

    if (!network)
        network = "MainNet";

    if (strcmp(network, "MainNet") == 0 || strcmp(network, "TestNet") == 0 ||
            strcmp(network, "RegTest") == 0) {
        netConfig = nlohmann::json();
    } else {
        try {
            std::ifstream in(network);
            netConfig = nlohmann::json::parse(in);
        } catch (...) {
            // error number?
            return NULL;
        }

        network = "PrvNet";
    }

    IMasterWalletManager *manager = new MasterWalletManager(
            walletDir, network, netConfig);
    if (!manager)
        return NULL;

    if (resolver) {
        url = strdup(resolver);
        if (!url) {
            delete manager;
            return NULL;
        }
    }

    IIDChainSubWallet *idWallet = NULL;

    CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
    if (rc != CURLE_OK) {
        if (url)
            free(url);
        delete manager;
        return NULL;
    }

    try {
        masterWallet = manager->GetMasterWallet(walletId);
        std::vector<ISubWallet *> subWallets = masterWallet->GetAllSubWallets();
        for (auto it = subWallets.begin(); it != subWallets.end(); ++it) {
            if ((*it)->GetChainID() == ID_CHAIN)
                idWallet = dynamic_cast<IIDChainSubWallet *>(*it);
        }
    } catch (...) {
    }

    if (!idWallet) {
        if (url)
            free(url);
        delete manager;
        curl_global_cleanup();
        return NULL;
    }

    SpvDidAdapter *adapter = new SpvDidAdapter;
    SubWalletCallback *callback = new SubWalletCallback(adapter);

    adapter->synced = false;
    adapter->manager = manager;
    adapter->masterWallet = masterWallet;
    adapter->idWallet = idWallet;
    adapter->callback = callback;
    adapter->resolver = url;

    SyncStart(adapter);

    return adapter;
}

void SpvDidAdapter_Destroy(SpvDidAdapter *adapter)
{
    if (!adapter)
        return;

    SyncStop(adapter);

    if (adapter->resolver)
        free(adapter->resolver);

    delete adapter->callback;
    delete adapter->manager;
    delete adapter;

    curl_global_cleanup();
}

int SpvDidAdapter_IsAvailable(SpvDidAdapter *adapter)
{
    if (!adapter)
        return 0;

    try {
        // Force sync
        adapter->idWallet->SyncStart();

        auto result = adapter->idWallet->GetAllTransaction(0, 1, "");
        auto count = result["MaxCount"];
        if (count < 1)
            return 1;

        auto tx = result["Transactions"][0];
        std::string confirm = tx["ConfirmStatus"];
        if (std::stoi(confirm) >= 2)
            return 1;
    } catch (...) {
        return 0;
    }

    return 0;
}

class TransactionResult {
private:
    char txid[128];
    int status;
    char message[256];
    Semaphore sem;

public:
    TransactionResult() : status(0) {
        *txid = 0;
        *message = 0;
    }

    const char *getTxid(void) {
        return txid;
    }

    int getStatus(void) {
        return status;
    }

    const char *getMessage(void) {
        return message;
    }

    void update(const char *_txid, int _status, const char *_message) {
        status = _status;

        if (_txid)
            strcpy(txid, _txid);

        if (_message)
            strcpy(message, _message);

        sem.notify();
    }

    void wait(void) {
        sem.wait();
    }
};

static void TransactionCallback(const char *txid, int status,
        const char *msg, void *context)
{
    TransactionResult *tr = (TransactionResult *)context;
    tr->update(txid, status, msg);
}

const char *SpvDidAdapter_CreateIdTransaction(SpvDidAdapter *adapter,
        const char *payload, const char *memo, const char *password)
{
    if (!adapter || !payload || !password)
        return NULL;

    if (!memo)
        memo = "";

    TransactionResult tr;

    int rc = SpvDidAdapter_CreateIdTransactionEx(adapter, payload, memo, 0,
        TransactionCallback, &tr, password);
    if (rc < 0)
        return NULL;

    tr.wait();
    if (tr.getStatus() < 0)
        return NULL;
    else
        return strdup(tr.getTxid());
}

int SpvDidAdapter_CreateIdTransactionEx(SpvDidAdapter *adapter,
        const char *payload, const char *memo, int confirm,
        SpvTransactionCallback *txCallback, void *context,
        const char *password)
{
    if (!adapter || !payload || !password)
        return -1;

    if (!memo)
        memo = "";

    try {
        auto payloadJson = nlohmann::json::parse(payload);

        auto tx = adapter->idWallet->CreateIDTransaction(payloadJson, memo);
        tx = adapter->idWallet->SignTransaction(tx, password);
        tx = adapter->idWallet->PublishTransaction(tx);
        std::string txid = tx["TxHash"];
        adapter->callback->RegisterTransactionCallback(txid,
                confirm, txCallback, context);
        return 0;
    } catch (...) {
        txCallback(NULL, -1, "SPV adapter internal error.", context);
        return -1;
    }

    return -1;
}

typedef struct HttpResponseBody {
    size_t used;
    size_t sz;
    void *data;
} HttpResponseBody;

static size_t HttpResponseBodyWriteCallback(char *ptr,
        size_t size, size_t nmemb, void *userdata)
{
    HttpResponseBody *response = (HttpResponseBody *)userdata;
    size_t length = size * nmemb;

    if (response->sz - response->used < length) {
        size_t new_sz;
        size_t last_try;
        void *new_data;

        if (response->sz + length < response->sz) {
            response->used = 0;
            return 0;
        }

        for (new_sz = response->sz ? response->sz << 1 : 512, last_try = response->sz;
            new_sz > last_try && new_sz <= response->sz + length;
            last_try = new_sz, new_sz <<= 1) ;

        if (new_sz <= last_try)
            new_sz = response->sz + length;

        new_sz += 16;

        new_data = realloc(response->data, new_sz);
        if (!new_data) {
            response->used = 0;
            return 0;
        }

        response->data = new_data;
        response->sz = new_sz;
    }

    memcpy((char *)response->data + response->used, ptr, length);
    response->used += length;

    return length;
}

typedef struct HttpRequestBody {
    size_t used;
    size_t sz;
    char *data;
} HttpRequestBody;

static size_t HttpRequestBodyReadCallback(void *dest, size_t size,
        size_t nmemb, void *userdata)
{
    HttpRequestBody *request = (HttpRequestBody *)userdata;
    size_t length = size * nmemb;
    size_t bytes_copy = request->sz - request->used;

    if (bytes_copy) {
        if(bytes_copy > length)
            bytes_copy = length;

        memcpy(dest, request->data + request->used, bytes_copy);

        request->used += bytes_copy;
        return bytes_copy;
    }

    return 0;
}

#define DID_RESOLVE_REQUEST "{\"method\":\"resolvedid\",\"params\":{\"did\":\"%s\",\"all\":%s}}"

// Caller need free the pointer
const char *SpvDidAdapter_Resolve(SpvDidAdapter *adapter, const char *did, int all)
{
    HttpRequestBody request;
    HttpResponseBody response;
    char buffer[256];
    const char *forAll;

    if (!adapter || !did || !adapter->resolver)
        return NULL;

    // TODO: max did length
    if (strlen(did) > 64)
        return NULL;

    forAll = !all ? "false" : "true";

    request.used = 0;
    request.sz = sprintf(buffer, DID_RESOLVE_REQUEST, did, forAll);
    request.data = buffer;

    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, adapter->resolver);

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, HttpRequestBodyReadCallback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &request);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request.sz);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HttpResponseBodyWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    memset(&response, 0, sizeof(response));
    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        if (response.data)
            free(response.data);

        return NULL;
    }

    ((char *)response.data)[response.used] = 0;
    return (const char *)response.data;
}

void SpvDidAdapter_FreeMemory(SpvDidAdapter *adapter, void *mem)
{
    (void)(adapter);

    free(mem);
}
