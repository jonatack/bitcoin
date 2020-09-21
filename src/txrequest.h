// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXREQUEST_H
#define BITCOIN_TXREQUEST_H

#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <chrono>
#include <unordered_map>
#include <vector>

#include <stdint.h>

/** Data structure to keep track of, and schedule, transaction downloads from peers.
 *
 * === High level behavior ===
 *
 * We keep track of which peers have announced which transactions, and use that to determine which requests
 * should go to which peer, when, and in what order.
 *
 * The following information is tracked per announcement:
 * - which peer announced it (through NodeId)
 * - the txid or wtxid of the transaction (collectively called txhash in what follows)
 * - whether it was a tx or wtx announcement (see BIP339).
 * - what the earliest permitted time is that that transaction can be requested from that peer (called "reqtime").
 * - whether it's from a "preferred" peer or not (outbound and whitelisted peers are preferred).
 * - whether the peer was the "first" to announce this txhash within its class (see further for details).
 * - whether or not the transaction was requested already, and if so, when it times out (called "exptime").
 * - whether or not the transaction request failed already (timed out, or NOTFOUND was received).
 *
 * Transaction requests are then assigned to peers, following these rules:
 *
 * - No transaction is requested as long as another request for the same txhash is outstanding (it needs to fail
 *   first by passing exptime, or a NOTFOUND response has to be received).
 *   Rationale: avoid wasting bandwidth on multiple copies of the same transaction.
 *
 * - The same transaction is never requested twice from the same peer, unless the transaction was forgotten in
 *   between (see next point), and re-announced.
 *   Rationale: giving a peer multiple chances to announce a transaction multiple times would allow them to bias
 *              requests in their favor, worsening invblock attacks.
 *
 * - Announcements are only forgotten about when the peer that announced them went offline, when the transaction
 *   was received successfully, or when no candidates for a transaction remain that haven't been tried already.
 *   Rationale: we need to eventually forget announcements to keep memory bounded, but as long as viable
 *              candidate peers remain, we prefer to avoid fetching from failed ones.
 *
 * - Transactions are not requested from a peer until its reqtime has passed.
 *   Rationale: enable net_processing code to define a delay for less-than-ideal peers, so that (presumed) better
 *              peers have a chance to give their announcement first.
 *
 * - If multiple viable candidate peers exist according to the above rules, pick a peer as follows:
 *   - If any preferred peers are available, non-preferred peers are not considered for what follows.
 *     Rationale: preferred peers (outbound, whitelisted) are chosen by us, so are less likely to be under attacker
 *                control.
 *   - Among the remaining candidates, choose the one with the first marker if one exists (there can be at most
 *     one such peer, see further).
 *     Rationale: in non-attack scenarios we want to give one chance to request from the fastest peer to reduce
 *                latency, and reduce risk of breaking chains of dependent transactions. An attacker who races the
 *                network can exploit this to delay us learning about a transaction, but it is available only once
 *                per txhash.
 *   - If no remaining candidates have the first marker, pick a uniformly random peer among the candidates.
 *     Rationale: if the first mechanism failed, random assignments are hard to influence for attackers.
 *
 * "First" marker rules: the first marker is given to announcements for which at the time of announcement:
 * - No requests for its txhash have been attempted (ever, or since it was forgotten about).
 * - The peer that announced them was not overloaded.
 * - No announcement for the same txhash from another peer within the same preferred/nonpreferred class already has
 *   the first marker already.
 *
 * === Specification ===
 *
 * The data structure maintains a collection of entries:
 *
 * - CANDIDATE entries represent transactions that were announced by peer, and become available for download after
 *   their reqtime has passed.
 *
 * - REQUESTED entries represent transactions that have been requested, and we're awaiting a response for from that
 *   peer. Theie exptime value determines when the request times out.
 *
 * - COMPLETED entries represent transactions that have been requested from a peer, but they timed out, a NOTFOUND
 *   message was received for them, or an invalid response was received. They're only kept around to prevent
 *   requesting them again. If only COMPLETED entries for a given txhash remain (so no CANDIDATE or
 *   REQUESTED ones), all of them are deleted (this is an invariant, and maintained by all operations below).
 *
 * The following operations are supported on this data structure:
 *
 * - ReceivedInv(gtxid, peer, preferred, overloaded, reqtime) adds a new CANDIDATE entry, unless one already exists
 *   for that (txhash, peer) combination (whether it's CANDIDATE, REQUESTED, or COMPLETED). Note that this means a
 *   second INV with the same txhash from the same peer will be ignored, even if one is a txid and the other is
 *   wtxid (but that shouldn't happen, as BIP339 requires that all announced inventory is exclusively using
 *   MSG_WTX). The new entry is given the specified preferred and reqtime values, and takes it is_wtxid from the
 *   specified gtxid. It is eligible to get a first marker if overloaded is false (but also subject to the other
 *   rules above).
 *
 * - DeletedPeer(peer) deletes all entries for a given peer. It should be called when a peer goes offline.
 *
 * - AlreadyHaveTx(gtxid) deletes all entries for a given txhash. It should be called when a transaction is
 *   successfully added to the mempool, seen in a block, or for whatever reason we no longer care about it.
 *   The is_wtxid flag of gtxid is ignored.
 *
 * - ReceivedResponse(peer, gtxid) converts any CANDIDATE or REQUESTED entry to a COMPLETED one, if one exists;
 *   otherwise it has no effect. It should be called whenever a transaction or NOTFOUND was received from a peer.
 *   When the transaction is acceptable, AlreadyHaveTx should be called instead of (or in addition to) this
 *   operation.
 *
 * - GetRequestable(peer, now) does the following:
 *   - Convert all REQUESTED entries (for all txhashes/peers) with (exptime <= now) to COMPLETED entries.
 *   - Requestable entries are selected: CANDIDATE entries from the specified peer with (reqtime <= now) for which
 *     the specified peer is the best choice among all such CANDIDATE entries with the same txhash (subject to
 *     preference/first rules, and tiebreaking using a deterministic salted hash of peer and txhash).
 *   - The selected entries are sorted in order of announcement (even if multiple were added at the same time, or
 *     even when the clock went backwards while they were being added), converted to GenTxids using their is_wtxid
 *     flag, and returned.
 *
 * - RequestedTx(peer, gtxid) converts the CANDIDATE entry for the provided peer and gtxid into a REQUESTED one, with
 *   exptime set to (now + timeout). It can ONLY be called immediately after GetRequestable was called (for the same
 *   peer), with only AlreadyHaveTx and other RequestedTx calls (both for other txhashes) in between. Any other
 *   non-const operation removes the ability to call RequestedTx.
 */
class TxRequestTracker {
public:
    /** A functor with embedded salt that computes priority of a txhash/peer combination. Lower priorities are
     *  selected first. */
    class PriorityComputer {
        const uint64_t m_k0, m_k1;
    public:
        explicit PriorityComputer(bool deterministic);
        uint64_t operator()(const uint256& txhash, uint64_t peer, bool preferred, bool first) const;
    };

private:
    //! The various states a (txid,node) pair can be in.
    //! Note that CANDIDATE is split up into 3 substates (DELAYED, BEST, READY), allowing more efficient implementation.
    //! Also note that the sorting order of EntryTxHash relies on the specific order of values in this enum.
    enum class State : uint8_t {
        //! A CANDIDATE entry whose reqtime is in the future.
        CANDIDATE_DELAYED,
        //! The best CANDIDATE for a given txid; only if there is no REQUESTED entry already for that txid.
        //! The CANDIDATE_BEST is the lowest-priority entry among all CANDIDATE_READY (and _BEST) ones for that txid.
        CANDIDATE_BEST,
        //! A REQUESTED entry.
        REQUESTED,
        //! A CANDIDATE entry that's not CANDIDATE_DELAYED or CANDIDATE_BEST.
        CANDIDATE_READY,
        //! A COMPLETED entry.
        COMPLETED,

        //! An invalid State value that's larger than all valid ones.
        TOO_LARGE,
    };

    /** A flag (in Entry::m_per_txhash) to indicate that for that txhash,
     *  new preferred announcements are not eligible to get the 'first' marker. */
    static constexpr uint8_t TXHASHINFO_NO_MORE_PREFERRED_FIRST = 1;
    /** A flag (in Entry::m_per_txhash) to indicate that for that txhash,
     *  new non-preferred announcements are not eligible to get the 'first' marker. */
    static constexpr uint8_t TXHASHINFO_NO_MORE_NONPREFERRED_FIRST = 2;

    //! Tag for the EntryPeer-based index.
    struct ByPeer {};
    //! Tag for the EntryTxHash-based index.
    struct ByTxHash {};
    //! Tag for the EntryTime-based index.
    struct ByTime {};

    //! The ByPeer index is sorted by (peer, state == CANDIDATE_BEST, txid)
    using EntryPeer = std::tuple<uint64_t, bool, const uint256&>;

    //! The ByTxHash index is sorted by (txhash, state, priority [CANDIDATE_READY]; 0 [otherwise])
    using EntryTxHash = std::tuple<const uint256&, State, uint64_t>;

    //! The ByTime index is sorted by (0 [CANDIDATE_DELAYED,REQUESTED]; 1 [COMPLETED];
    // 2 [CANDIDATE_READY,CANDIDATE_BEST], time)
    using EntryTime = std::pair<int, std::chrono::microseconds>;

    //! The current sequence number. Increases for every announcement. This is used to sort txids returned by
    //! GetRequestable in announcement order.
    uint64_t m_sequence{0};

    //! An announcement entry.
    struct Entry {
        //! Txid that was announced.
        const uint256 m_txhash;
        //! For CANDIDATE_{DELAYED,BEST,READY} the reqtime; for REQUESTED the exptime
        std::chrono::microseconds m_time;
        //! What peer the request was from.
        const uint64_t m_peer;
        //! What sequence number this announcement has.
        const uint64_t m_sequence : 56;
        //! Whether the request is preferred (giving it priority higher than non-preferred ones).
        const bool m_preferred : 1;
        //! Whether this is a wtxid request.
        const bool m_is_wtxid : 1;
        //! Whether this was: the very first announcement for this txhash, within the
        //! preferred or non-preferred ones, and no request had been made for this
        //! txhash from any peer at the time the announcement came in.
        const bool m_first : 1;

        //! What state this announcement is in
        //! This is a uint8_t instead of a State to silence a GCC warning.
        uint8_t m_state : 3;

        /** Per-txhash flags. These are stored in the last Entry for a given txhash (ByTxHash order).
         *  The values for other Entry objects can be arbitrary subsets of the actual flags. */
        mutable uint8_t m_per_txhash : 2;

        //! Convert the m_state variable to a State enum.
        State GetState() const { return State(m_state); }
        //! Convert a State to a uint8_t and store it in m_state.
        void SetState(State state) { m_state = uint8_t(state); }

        //! Whether this entry is selected. There can be at most 1 selected peer per txhash.
        bool IsSelected() const
        {
            return GetState() == State::CANDIDATE_BEST || GetState() == State::REQUESTED;
        }

        //! Whether this entry is waiting for a certain time to pass.
        bool IsWaiting() const
        {
            return GetState() == State::REQUESTED || GetState() == State::CANDIDATE_DELAYED;
        }

        //! Whether this entry can feasibly be selected if the current IsSelected() one disappears.
        bool IsSelectable() const
        {
            return GetState() == State::CANDIDATE_READY || GetState() == State::CANDIDATE_BEST;
        }

        //! Construct a new entry from scratch, initially in CANDIDATE_DELATED state.
        Entry(const GenTxid& gtxid, uint64_t peer, bool preferred, std::chrono::microseconds reqtime,
            uint64_t sequence, bool first) :
            m_txhash(gtxid.GetHash()), m_time(reqtime), m_peer(peer), m_sequence(sequence), m_preferred(preferred),
            m_is_wtxid(gtxid.IsWtxid()), m_first(first), m_state(uint8_t(State::CANDIDATE_DELAYED)),
            m_per_txhash(0) {}

        //! Compute this Entry's priority.
        uint64_t ComputePriority(const PriorityComputer& computer) const
        {
            return computer(m_txhash, m_peer, m_preferred, m_first);
        }

        //! Extract the EntryPeer from this Entry.
        EntryPeer ExtractPeer() const { return EntryPeer{m_peer, GetState() == State::CANDIDATE_BEST, m_txhash}; }

        //! Extract the EntryTxHash from this Entry.
        EntryTxHash ExtractTxid(const PriorityComputer& computer) const
        {
            return EntryTxHash{m_txhash, GetState(), GetState() == State::CANDIDATE_READY ? ComputePriority(computer) : 0};
        }

        //! Extract the EntryTime from this Entry.
        EntryTime ExtractTime() const { return EntryTime{IsWaiting() ? 0 : IsSelectable() ? 2 : 1, m_time}; }
    };

    //! This tracker's priority computer.
    const PriorityComputer m_computer;

    /** An extractor for EntryTxHashs (with encapsulated PriorityComputer reference).
     *
     * See https://www.boost.org/doc/libs/1_54_0/libs/multi_index/doc/reference/key_extraction.html#key_extractors
     * for more information about the key extraction concept.
     */
    struct EntryTxHashExtractor {
    private:
        const PriorityComputer& m_computer;
    public:
        EntryTxHashExtractor(const PriorityComputer& computer) : m_computer(computer) {}
        using result_type = EntryTxHash; // Needed to comply with key extractor interface
        result_type operator()(const Entry& entry) const { return entry.ExtractTxid(m_computer); }
    };

    //! Data type for the main data structure (Entry objects with ByPeer/ByTxHash/ByTime indexes).
    using Index = boost::multi_index_container<
        Entry,
        boost::multi_index::indexed_by<
            boost::multi_index::ordered_unique<
                boost::multi_index::tag<ByPeer>,
                boost::multi_index::const_mem_fun<Entry, EntryPeer, &Entry::ExtractPeer>
            >,
            boost::multi_index::ordered_non_unique<
                boost::multi_index::tag<ByTxHash>,
                EntryTxHashExtractor
            >,
            boost::multi_index::ordered_non_unique<
                boost::multi_index::tag<ByTime>,
                boost::multi_index::const_mem_fun<Entry, EntryTime, &Entry::ExtractTime>
            >
        >
    >;

    //! This tracker's main data structure.
    Index m_index;

    //! Per-peer statistics object.
    struct PeerInfo {
        size_t m_total = 0; //!< Total number of entries for this peer.
        size_t m_requested = 0; //!< Total number of REQUESTED entries for this peer.

        friend bool operator==(const PeerInfo& a, const PeerInfo& b)
        {
            return std::tie(a.m_total, a.m_requested) == std::tie(b.m_total, b.m_requested);
        }
    };

    //! Map with this tracker's per-peer statistics.
    std::unordered_map<uint64_t, PeerInfo> m_peerinfo;

    //! Wrapper around Index::...::erase that keeps m_peerinfo and m_per_txhash up to date.
    template<typename Tag>
    typename Index::index<Tag>::type::iterator Erase(typename Index::index<Tag>::type::iterator it);

    //! Wrapper around Index::...::modify that keeps m_peerinfo and m_per_txhash up to date.
    template<typename Tag, typename Modifier>
    void Modify(typename Index::index<Tag>::type::iterator it, Modifier modifier);

    //! Convert a CANDIDATE_DELAYED entry into a CANDIDATE_READY. If this makes it the new best CANDIDATE_READY (and no
    //! REQUESTED exists) and better than the CANDIDATE_BEST (if any), it becomes the new CANDIDATE_BEST.
    void PromoteCandidateNew(typename TxRequestTracker::Index::index<ByTxHash>::type::iterator it);

    //! Change the state of an entry to something non-IsSelected(). If it was IsSelected(), the next best entry will
    //! be marked CANDIDATE_BEST.
    void ChangeAndReselect(typename Index::index<ByTxHash>::type::iterator it, State new_state);

    //! Convert any entry to a COMPLETED one. If there are no non-COMPLETED entries left for this txid, they are all
    //! deleted. If this was a REQUESTED entry, and there are other CANDIDATEs left, the best one is made
    //! CANDIDATE_BEST. Returns whether the Entry still exists.
    bool MakeCompleted(typename Index::index<ByTxHash>::type::iterator it);

    //! Make the data structure consistent with a given point in time:
    //! - REQUESTED entries with exptime <= now are turned into COMPLETED.
    //! - CANDIDATE_DELAYED entries with reqtime <= now are turned into CANDIDATE_{READY,BEST}.
    //! - CANDIDATE_{READY,BEST} entries with reqtime > now are turned into CANDIDATE_DELAYED.
    void SetTimePoint(std::chrono::microseconds now);

public:
    //! Construct a TxRequestTracker.
    TxRequestTracker(bool deterministic = false);

    // Disable copying and assigning (a default copy won't work due the stateful EntryTxHashExtractor).
    TxRequestTracker(const TxRequestTracker&) = delete;
    TxRequestTracker& operator=(const TxRequestTracker&) = delete;

    //! A peer went offline, delete any data related to it.
    void DeletedPeer(uint64_t uint64_t);

    //! For whatever reason, we no longer need this txid. Delete any data related to it.
    void AlreadyHaveTx(const GenTxid& gtxid);

    //! We received a new inv, enter it into the data structure.
    void ReceivedInv(uint64_t peer, const GenTxid& txid, bool preferred, bool overloaded, std::chrono::microseconds reqtime);

    //! Find the txids to request now from peer.
    std::vector<GenTxid> GetRequestable(uint64_t peer, std::chrono::microseconds now);

    //! Inform the data structure that a txid was requested. This can only be called for txids returned by the last
    //! GetRequestable call (which must have been for the same peer), with only other RequestedTx and AlreadyHaveTx
    //! calls in between (which must have been for the same peer but different txids).
    void RequestedTx(uint64_t peer, const GenTxid& txid, std::chrono::microseconds exptime);

    //! We received a response (a tx, or a NOTFOUND) for txid from peer. Note that if a good tx is received (such
    //! that we don't need it anymore), AlreadyHaveTx should be called instead of (or in addition to)
    //! ReceivedResponse.
    void ReceivedResponse(uint64_t peer, const GenTxid& txid);

    //! Count how many in-flight transactions a peer has.
    size_t CountInFlight(uint64_t peer) const;

    //! Count how many transactions are being tracked for a peer (including timed-out ones and in-flight ones).
    size_t CountTracked(uint64_t peer) const;

    //! Count how many announcements are being tracked in total across all peers and transactions.
    size_t Size() const { return m_index.size(); }

    //! Access to the internal PriorityComputer (for testing)
    const PriorityComputer& GetPriorityComputer() const { return m_computer; }

    //! Run internal consistency check (test only)
    void SanityCheck() const;

    //! Run a time-dependent consistency check (only expected to hold after GetRequestable; test only)
    void TimeSanityCheck(std::chrono::microseconds now) const;
};

#endif // BITCOIN_TXREQUEST_H
