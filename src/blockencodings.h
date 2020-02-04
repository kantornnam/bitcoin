// Copyright (c) 2016-2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BLOCKENCODINGS_H
#define BITCOIN_BLOCKENCODINGS_H

#include <primitives/block.h>

// Transaction compression schemes for compact block relay can be introduced by writing
// an actual formatter here.
using TransactionCompression = DefaultFormatter;

class CTxMemPool;

struct Uint48Formatter
{
    template <typename Stream, typename I> void Ser(Stream& s, I v)
    {
        if (v < 0 || (v >> 48) != 0) throw std::ios_base::failure("Uint48Formatter value out of range");

        uint32_t lsb = v & 0xffffffff;
        uint16_t msb = (v >> 32) & 0xffff;
        s << lsb << msb;
    }

    template <typename Stream, typename I> void Unser(Stream& s, I& v)
    {
        static_assert(std::numeric_limits<I>::max() >= 0xffffffffffff && std::numeric_limits<I>::min() <= 0, "Uint48Formatter needs a 48+ bit type");

        uint32_t lsb;
        uint16_t msb;
        s >> lsb >> msb;
        v = (uint64_t(msb) << 32) | uint64_t(lsb);
    }
};

template<bool Unser>
class DifferenceTransform
{
    int32_t m_shift = 0;

public:
    uint16_t operator()(uint16_t val)
    {
        int32_t out = m_shift + val;
        if (out < 0 || out > 0xffff) throw std::ios_base::failure("differential value overflow");
        m_shift = Unser ? out + 1 : (-(int32_t)val) - 1;
        return uint16_t(out);
    }
};

class BlockTransactionsRequest {
public:
    // A BlockTransactionsRequest message
    uint256 blockhash;
    std::vector<uint16_t> indexes;

    SERIALIZE_METHODS(BlockTransactionsRequest, obj)
    {
        READWRITE(obj.blockhash, Using<VectorFormatter<CompactSizeFormatter, DifferenceTransform<false>, DifferenceTransform<true>>>(obj.indexes));
    }
};

class BlockTransactions {
public:
    // A BlockTransactions message
    uint256 blockhash;
    std::vector<CTransactionRef> txn;

    BlockTransactions() {}
    explicit BlockTransactions(const BlockTransactionsRequest& req) :
        blockhash(req.blockhash), txn(req.indexes.size()) {}

    SERIALIZE_METHODS(BlockTransactions, obj)
    {
        READWRITE(obj.blockhash, Using<VectorFormatter<TransactionCompression>>(obj.txn));
    }
};

// Dumb serialization/storage-helper for CBlockHeaderAndShortTxIDs and PartiallyDownloadedBlock
struct PrefilledTransaction {
    // Used as an offset since last prefilled tx in CBlockHeaderAndShortTxIDs,
    // as a proper transaction-in-block-index in PartiallyDownloadedBlock
    uint16_t index;
    CTransactionRef tx;

    SERIALIZE_METHODS(PrefilledTransaction, obj) { READWRITE(COMPACTSIZE(obj.index), Using<TransactionCompression>(obj.tx)); }
};

typedef enum ReadStatus_t
{
    READ_STATUS_OK,
    READ_STATUS_INVALID, // Invalid object, peer is sending bogus crap
    READ_STATUS_FAILED, // Failed to process object
    READ_STATUS_CHECKBLOCK_FAILED, // Used only by FillBlock to indicate a
                                   // failure in CheckBlock.
} ReadStatus;

class CBlockHeaderAndShortTxIDs {
private:
    mutable uint64_t shorttxidk0, shorttxidk1;
    uint64_t nonce;

    void FillShortTxIDSelector() const;

    friend class PartiallyDownloadedBlock;

    static const int SHORTTXIDS_LENGTH = 6;
protected:
    std::vector<uint64_t> shorttxids;
    std::vector<PrefilledTransaction> prefilledtxn;

public:
    CBlockHeader header;

    // Dummy for deserialization
    CBlockHeaderAndShortTxIDs() {}

    CBlockHeaderAndShortTxIDs(const CBlock& block, bool fUseWTXID);

    uint64_t GetShortID(const uint256& txhash) const;

    size_t BlockTxCount() const { return shorttxids.size() + prefilledtxn.size(); }

    SERIALIZE_METHODS(CBlockHeaderAndShortTxIDs, obj)
    {
        static_assert(SHORTTXIDS_LENGTH == 6, "shorttxids serialization assumes 6-byte shorttxids");
        READWRITE(obj.header, obj.nonce, Using<VectorFormatter<Uint48Formatter>>(obj.shorttxids), obj.prefilledtxn);
        if (ser_action.ForRead()) {
            if (obj.BlockTxCount() > std::numeric_limits<uint16_t>::max()) {
                throw std::ios_base::failure("indexes overflowed 16 bits");
            }
            obj.FillShortTxIDSelector();
        }
    }
};

class PartiallyDownloadedBlock {
protected:
    std::vector<CTransactionRef> txn_available;
    size_t prefilled_count = 0, mempool_count = 0, extra_count = 0;
    CTxMemPool* pool;
public:
    CBlockHeader header;
    explicit PartiallyDownloadedBlock(CTxMemPool* poolIn) : pool(poolIn) {}

    // extra_txn is a list of extra transactions to look at, in <witness hash, reference> form
    ReadStatus InitData(const CBlockHeaderAndShortTxIDs& cmpctblock, const std::vector<std::pair<uint256, CTransactionRef>>& extra_txn);
    bool IsTxAvailable(size_t index) const;
    ReadStatus FillBlock(CBlock& block, const std::vector<CTransactionRef>& vtx_missing);
};

#endif // BITCOIN_BLOCKENCODINGS_H
