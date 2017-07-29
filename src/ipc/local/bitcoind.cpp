#include <ipc/interfaces.h>

#include <chainparams.h>
#include <ipc/util.h>
#include <net.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <validation.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif
#ifdef ENABLE_WALLET
#include <wallet/fees.h>
#include <wallet/wallet.h>
#define CHECK_WALLET(x) x
#else
#define CHECK_WALLET(x) throw std::logic_error("Wallet function called in non-wallet build.")
#endif

namespace ipc {
namespace local {
namespace {

class LockedStateImpl : public Chain::LockedState
{
public:
    int getHeight() { return ::chainActive.Height(); }
    int getBlockHeight(const uint256& hash) override
    {
        auto it = ::mapBlockIndex.find(hash);
        if (it != ::mapBlockIndex.end() && it->second) {
            if (::chainActive.Contains(it->second)) {
                return it->second->nHeight;
            }
        }
        return -1;
    }
    int getBlockDepth(const uint256& hash) override
    {
        int height = getBlockHeight(hash);
        return height < 0 ? 0 : ::chainActive.Height() - height + 1;
    }
    uint256 getBlockHash(int height) override { return ::chainActive[height]->GetBlockHash(); }
    int64_t getBlockTime(int height) override { return ::chainActive[height]->GetBlockTime(); }
    int64_t getBlockTimeMax(int height) override { return ::chainActive[height]->GetBlockTimeMax(); }
    int64_t getBlockMedianTimePast(int height) override { return ::chainActive[height]->GetMedianTimePast(); }
    bool blockHasTransactions(int height) override
    {
        CBlockIndex* block = ::chainActive[height];
        return block && (block->nStatus & BLOCK_HAVE_DATA) && block->nTx > 0;
    }
    bool readBlockFromDisk(int height, CBlock& block)
    {
        return ReadBlockFromDisk(block, ::chainActive[height], Params().GetConsensus());
    }
    double guessVerificationProgress(int height)
    {
        return GuessVerificationProgress(Params().TxData(), ::chainActive[height]);
    }
    int findEarliestAtLeast(int64_t time)
    {
        CBlockIndex* block = ::chainActive.FindEarliestAtLeast(time);
        return block ? block->nHeight : -1;
    }
    int64_t findLastBefore(int64_t time, int start_height)
    {
        CBlockIndex* block = ::chainActive[start_height];
        while (block && block->GetBlockTime() < time) {
            block = ::chainActive.Next(block);
        }
        return block ? block->nHeight : -1;
    }
    bool isPotentialTip(const uint256& hash)
    {
        if (::chainActive.Tip()->GetBlockHash() == hash) return true;
        auto it = ::mapBlockIndex.find(hash);
        return it != ::mapBlockIndex.end() && it->second->GetAncestor(::chainActive.Height()) == ::chainActive.Tip();
    }
    int findFork(const uint256& hash, int* height) override
    {
        const CBlockIndex *block{nullptr}, *fork{nullptr};
        auto it = ::mapBlockIndex.find(hash);
        if (it != ::mapBlockIndex.end()) {
            block = it->second;
            fork = ::chainActive.FindFork(block);
        }
        if (height) *height = block ? block->nHeight : -1;
        return fork ? fork->nHeight : -1;
    }
    CBlockLocator getLocator() override { return ::chainActive.GetLocator(); }
    int findLocatorFork(const CBlockLocator& locator) override
    {
        CBlockIndex* fork = FindForkInGlobalIndex(::chainActive, locator);
        return fork ? fork->nHeight : -1;
    }
    bool checkFinalTx(const CTransaction& tx) override { return CheckFinalTx(tx); }
    bool isWitnessEnabled() override { return ::IsWitnessEnabled(::chainActive.Tip(), ::Params().GetConsensus()); }
    bool acceptToMemoryPool(CTransactionRef tx, CValidationState& state) override
    {
        return AcceptToMemoryPool(::mempool, state, tx, true, nullptr, nullptr, false, ::maxTxFee);
    }
};

class LockingStateImpl : public LockedStateImpl, public CCriticalBlock
{
    using CCriticalBlock::CCriticalBlock;
};

class ChainImpl : public Chain
{
public:
    std::unique_ptr<Chain::LockedState> lockState(bool try_lock) override
    {
        auto result = MakeUnique<LockingStateImpl>(::cs_main, "cs_main", __FILE__, __LINE__, try_lock);
        if (try_lock && result && !*result) return {};
        // std::move necessary on some compilers due to conversion from
        // LockingStateImpl to LockedState pointer
        return std::move(result);
    }
    std::unique_ptr<Chain::LockedState> assumeLocked() override { return MakeUnique<LockedStateImpl>(); }
    bool findBlock(const uint256& hash, CBlock* block, int64_t* time) override
    {
        LOCK(cs_main);
        auto it = ::mapBlockIndex.find(hash);
        if (it == ::mapBlockIndex.end()) {
            return false;
        }
        if (block) {
            if (!ReadBlockFromDisk(*block, it->second, Params().GetConsensus())) {
                block->SetNull();
            }
        }
        if (time) {
            *time = it->second->GetBlockTime();
        }
        return true;
    }
    int64_t getVirtualTransactionSize(const CTransaction& tx) override { return GetVirtualTransactionSize(tx); }
    RBFTransactionState isRBFOptIn(const CTransaction& tx) override
    {
        LOCK(::mempool.cs);
        return IsRBFOptIn(tx, ::mempool);
    }
    bool hasDescendantsInMempool(const uint256& txid) override
    {
        LOCK(::mempool.cs);
        auto it_mp = ::mempool.mapTx.find(txid);
        return it_mp != ::mempool.mapTx.end() && it_mp->GetCountWithDescendants() > 1;
    }
    bool relayTransaction(const uint256& txid) override
    {
        if (g_connman) {
            CInv inv(MSG_TX, txid);
            g_connman->ForEachNode([&inv](CNode* node) { node->PushInventory(inv); });
            return true;
        }
        return false;
    }
    bool transactionWithinChainLimit(const uint256& txid, size_t chain_limit) override
    {
        return ::mempool.TransactionWithinChainLimit(txid, chain_limit);
    }
    bool checkChainLimits(CTransactionRef tx) override
    {
        LockPoints lp;
        CTxMemPoolEntry entry(tx, 0, 0, 0, false, 0, lp);
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants = gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) * 1000;
        std::string errString;
        if (!::mempool.CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize,
                nLimitDescendants, nLimitDescendantSize, errString)) {
            return false;
        }
        return true;
    }
    CFeeRate getMinPoolFeeRate() override
    {
        return ::mempool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000);
    }
    CFeeRate getMinRelayFeeRate() override { return ::minRelayTxFee; }
    CFeeRate getIncrementalRelayFeeRate() override { return ::incrementalRelayFee; }
    CFeeRate getDustRelayFeeRate() override { return ::dustRelayFee; }
    CFeeRate getMaxDiscardFeeRate() override { CHECK_WALLET(return GetDiscardRate(::feeEstimator)); }
    CAmount getMaxTxFee() override { return ::maxTxFee; }
    CAmount getMinTxFee(unsigned int tx_bytes, const CCoinControl& coin_control, FeeCalculation* calc) override
    {
        CHECK_WALLET(return GetMinimumFee(tx_bytes, coin_control, ::mempool, ::feeEstimator, calc));
    }
    CAmount getRequiredTxFee(unsigned int tx_bytes) override
    {
        CHECK_WALLET(return GetRequiredFee(tx_bytes));
    }
    bool getPruneMode() override { return ::fPruneMode; }
    bool p2pEnabled() override { return g_connman != nullptr; }
    int64_t getAdjustedTime() override { return GetAdjustedTime(); }
};

} // namespace

std::unique_ptr<Chain> MakeChain() { return MakeUnique<ChainImpl>(); }

} // namespace local
} // namespace ipc
