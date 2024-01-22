#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "common/timer.h"
#include "common/types/value/value.h"
#include "main/kuzu_fwd.h"
#include "transaction/transaction_context.h"

namespace kuzu {

namespace binder {
class Binder;
}

namespace common {
class RandomEngine;
}

namespace main {
class Database;

struct ActiveQuery {
    explicit ActiveQuery();
    std::atomic<bool> interrupted;
    common::Timer timer;

    void reset();
};

using replace_func_t = std::function<std::unique_ptr<common::Value>(common::Value*)>;

/**
 * @brief Contain client side configuration. We make profiler associated per query, so profiler is
 * not maintained in client context.
 */
class ClientContext {
    friend class Connection;
    friend class binder::Binder;
    friend class testing::TinySnbDDLTest;
    friend class testing::TinySnbCopyCSVTransactionTest;
    friend struct ThreadsSetting;
    friend struct TimeoutSetting;
    friend struct VarLengthExtendMaxDepthSetting;
    friend struct EnableSemiMaskSetting;

public:
    explicit ClientContext(Database* database);

    inline void interrupt() { activeQuery.interrupted = true; }

    bool isInterrupted() const { return activeQuery.interrupted; }

    inline bool isTimeOutEnabled() const { return timeoutInMS != 0; }

    inline uint64_t getTimeoutRemainingInMS() {
        KU_ASSERT(isTimeOutEnabled());
        auto elapsed = activeQuery.timer.getElapsedTimeInMS();
        return elapsed >= timeoutInMS ? 0 : timeoutInMS - elapsed;
    }

    inline bool isEnableSemiMask() const { return enableSemiMask; }

    void startTimingIfEnabled();

    KUZU_API common::Value getCurrentSetting(const std::string& optionName);

    transaction::Transaction* getTx() const;
    transaction::TransactionContext* getTransactionContext() const;

    void setReplaceFunc(replace_func_t replaceFunc) { this->replaceFunc = std::move(replaceFunc); }

    void setExtensionOption(std::string name, common::Value value);

    common::RandomEngine* getRandomEngine() { return randomEngine.get(); }

    common::VirtualFileSystem* getVFSUnsafe() const;

    std::string getExtensionDir() const;

    Database* getDatabase() const { return database; }

    storage::MemoryManager* getMemoryManager();

private:
    inline void resetActiveQuery() { activeQuery.reset(); }

    uint64_t numThreadsForExecution;
    ActiveQuery activeQuery;
    uint64_t timeoutInMS;
    uint32_t varLengthExtendMaxDepth;
    std::unique_ptr<transaction::TransactionContext> transactionContext;
    bool enableSemiMask;
    replace_func_t replaceFunc;
    std::unordered_map<std::string, common::Value> extensionOptionValues;
    std::unique_ptr<common::RandomEngine> randomEngine;
    Database* database;
};

} // namespace main
} // namespace kuzu
