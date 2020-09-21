// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txrequest.h>

#include <crypto/siphash.h>
#include <net.h>
#include <primitives/transaction.h>
#include <random.h>
#include <uint256.h>

#include <chrono>
#include <utility>

#include <assert.h>

TxRequestTracker::PriorityComputer::PriorityComputer(bool deterministic) :
    m_k0{deterministic ? 0 : GetRand(0xFFFFFFFFFFFFFFFF)},
    m_k1{deterministic ? 0 : GetRand(0xFFFFFFFFFFFFFFFF)} {}

uint64_t TxRequestTracker::PriorityComputer::operator()(const uint256& txhash, uint64_t peer, bool preferred, bool first) const
{
    uint64_t low_bits = 0;
    if (!first) {
        low_bits = CSipHasher(m_k0, m_k1).Write(txhash.begin(), txhash.size()).Write(peer).Finalize() >> 1;
    }
    return low_bits | uint64_t{!preferred} << 63;
}

TxRequestTracker::TxRequestTracker(bool deterministic) :
    m_computer(deterministic),
    // Explicitly initialize m_index as we need to pass a reference to m_computer to
    // EntryTxHashExtractor.
    m_index(boost::make_tuple(
        boost::make_tuple(
            boost::multi_index::const_mem_fun<Entry, EntryPeer, &Entry::ExtractPeer>(),
            std::less<EntryPeer>()
        ),
        boost::make_tuple(
            EntryTxHashExtractor(m_computer),
            std::less<EntryTxHash>()
        ),
        boost::make_tuple(
            boost::multi_index::const_mem_fun<Entry, EntryTime, &Entry::ExtractTime>(),
            std::less<EntryTime>()
        )
    )) {}

template<typename Tag>
typename TxRequestTracker::Index::index<Tag>::type::iterator
TxRequestTracker::Erase(typename Index::index<Tag>::type::iterator it)
{
    auto peerit = m_peerinfo.find(it->m_peer);
    peerit->second.m_requested -= it->GetState() == State::REQUESTED;
    if (--peerit->second.m_total == 0) m_peerinfo.erase(peerit);
    // As it may possibly be the last-sorted Entry for a given txhash, propagate its per-txhash
    // flags to its predecessor (if it belongs to the same txhash).
    auto bytxhashit = m_index.project<ByTxHash>(it);
    if (bytxhashit != m_index.get<ByTxHash>().begin() &&
        bytxhashit->m_txhash == std::prev(bytxhashit)->m_txhash) {
        std::prev(bytxhashit)->m_per_txhash |= bytxhashit->m_per_txhash;
    }
    return m_index.get<Tag>().erase(it);
}

template<typename Tag, typename Modifier>
void TxRequestTracker::Modify(typename Index::index<Tag>::type::iterator it, Modifier modifier)
{
    auto peerit = m_peerinfo.find(it->m_peer);
    peerit->second.m_requested -= it->GetState() == State::REQUESTED;
    // It's possible that it used to be the last-sorted Entry for its txhash, so propagate its
    // flags to its predecessor (which would then become the new last-sorted Entry).
    auto bytxhashit = m_index.project<ByTxHash>(it);
    if (bytxhashit != m_index.get<ByTxHash>().begin() &&
        bytxhashit->m_txhash == std::prev(bytxhashit)->m_txhash) {
        std::prev(bytxhashit)->m_per_txhash |= bytxhashit->m_per_txhash;
    }
    m_index.get<Tag>().modify(it, std::move(modifier));
    // It's possible that it is now the new last-sorted Entry for its txhash, so propagate flags
    // from its predecessor to it.
    bytxhashit = m_index.project<ByTxHash>(it);
    if (bytxhashit != m_index.get<ByTxHash>().begin() &&
        bytxhashit->m_txhash == std::prev(bytxhashit)->m_txhash) {
        bytxhashit->m_per_txhash |= std::prev(bytxhashit)->m_per_txhash;
    }
    peerit->second.m_requested += it->GetState() == State::REQUESTED;
}

void TxRequestTracker::PromoteCandidateNew(typename TxRequestTracker::Index::index<ByTxHash>::type::iterator it)
{
    assert(it->GetState() == State::CANDIDATE_DELAYED);
    // Convert CANDIDATE_DELAYED to CANDIDATE_READY first.
    Modify<ByTxHash>(it, [](Entry& entry){ entry.SetState(State::CANDIDATE_READY); });
    // The following code relies on the fact that the ByTxHash is sorted by txid, and then by state (first _DELAYED,
    // then _BEST/REQUESTED, then _READY). Within the _READY entries, the best one (lowest priority) comes first.
    // Thus, if an existing _BEST exists for the same txid that this entry may be preferred over, it must immediately
    // precede the newly created _READY.
    if (it == m_index.get<ByTxHash>().begin() || std::prev(it)->m_txhash != it->m_txhash ||
        std::prev(it)->GetState() == State::CANDIDATE_DELAYED) {
        // This is the new best CANDIDATE_READY, and there is no IsSelected() entry for this txid already.
        Modify<ByTxHash>(it, [](Entry& entry){ entry.SetState(State::CANDIDATE_BEST); });
    } else if (std::prev(it)->GetState() == State::CANDIDATE_BEST) {
        uint64_t priority_old = std::prev(it)->ComputePriority(m_computer);
        uint64_t priority_new = it->ComputePriority(m_computer);
        if (priority_new < priority_old) {
            // There is a CANDIDATE_BEST entry already, but this one is better.
            auto new_ready_it = std::prev(it);
            Modify<ByTxHash>(new_ready_it, [](Entry& entry){ entry.SetState(State::CANDIDATE_READY); });
            Modify<ByTxHash>(it, [](Entry& entry){ entry.SetState(State::CANDIDATE_BEST); });
        }
    }
}

void TxRequestTracker::ChangeAndReselect(typename TxRequestTracker::Index::index<ByTxHash>::type::iterator it,
    TxRequestTracker::State new_state)
{
    if (it->IsSelected()) {
        auto it_next = std::next(it);
        // The next best CANDIDATE_READY, if any, immediately follows the REQUESTED or CANDIDATE_BEST entry in the
        // ByTxHash index.
        if (it_next != m_index.get<ByTxHash>().end() && it_next->m_txhash == it->m_txhash &&
            it_next->GetState() == State::CANDIDATE_READY) {
            // If one such CANDIDATE_READY exists (for this txhash), convert it to CANDIDATE_BEST.
            Modify<ByTxHash>(it_next, [](Entry& entry){ entry.SetState(State::CANDIDATE_BEST); });
        }
    }
    Modify<ByTxHash>(it, [new_state](Entry& entry){ entry.SetState(new_state); });
    assert(!it->IsSelected());
}

bool TxRequestTracker::MakeCompleted(typename TxRequestTracker::Index::index<ByTxHash>::type::iterator it)
{
    // Nothing to be done if it's already COMPLETED.
    if (it->GetState() == State::COMPLETED) return true;

    if ((it == m_index.get<ByTxHash>().begin() || std::prev(it)->m_txhash != it->m_txhash) &&
        (std::next(it) == m_index.get<ByTxHash>().end() || std::next(it)->m_txhash != it->m_txhash ||
        std::next(it)->GetState() == State::COMPLETED)) {
        // This is the first entry for this txid, and the last non-COMPLETED one. There are only COMPLETED ones left.
        // Delete them all.
        uint256 txhash = it->m_txhash;
        do {
            it = Erase<ByTxHash>(it);
        } while (it != m_index.get<ByTxHash>().end() && it->m_txhash == txhash);
        return false;
    }

    // Mark the entry COMPLETED, and select the next best entry if needed.
    ChangeAndReselect(it, State::COMPLETED);

    return true;
}

void TxRequestTracker::SetTimePoint(std::chrono::microseconds now)
{
    // Iterate over all CANDIDATE_DELAYED and REQUESTED from old to new, as long as they're in the past,
    // and convert them to CANDIDATE_READY and COMPLETED respectively.
    while (!m_index.empty()) {
        auto it = m_index.get<ByTime>().begin();
        if (it->GetState() == State::CANDIDATE_DELAYED && it->m_time <= now) {
            PromoteCandidateNew(m_index.project<ByTxHash>(it));
        } else if (it->GetState() == State::REQUESTED && it->m_time <= now) {
            MakeCompleted(m_index.project<ByTxHash>(it));
        } else {
            break;
        }
    }

    while (!m_index.empty()) {
        // If time went backwards, we may need to demote CANDIDATE_BEST and CANDIDATE_READY entries back
        // to CANDIDATE_DELAYED. This is an unusual edge case, and unlikely to matter in production. However,
        // it makes it much easier to specify and test TxRequestTracker's behaviour.
        auto it = std::prev(m_index.get<ByTime>().end());
        if (it->IsSelectable() && it->m_time > now) {
            ChangeAndReselect(m_index.project<ByTxHash>(it), State::CANDIDATE_DELAYED);
        } else {
            break;
        }
    }
}


void TxRequestTracker::AlreadyHaveTx(const GenTxid& gtxid)
{
    auto it = m_index.get<ByTxHash>().lower_bound(EntryTxHash{gtxid.GetHash(), State::CANDIDATE_DELAYED, 0});
    while (it != m_index.get<ByTxHash>().end() && it->m_txhash == gtxid.GetHash()) {
        it = Erase<ByTxHash>(it);
    }
}

static const uint256 UINT256_ZERO;

void TxRequestTracker::DeletedPeer(uint64_t peer)
{
    auto& index = m_index.get<ByPeer>();
    auto it = index.lower_bound(EntryPeer{peer, false, UINT256_ZERO});
    while (it != index.end() && it->m_peer == peer) {
        // Check what to continue with after this iteration. Note that 'it' may change position, and std::next(it)
        // may be deleted in the process, so this needs to be decided beforehand.
        auto it_next = (std::next(it) == index.end() || std::next(it)->m_peer != peer) ? index.end() : std::next(it);
        // If the entry isn't already COMPLETED, first make it COMPLETED (which will mark other CANDIDATEs as
        // CANDIDATE_BEST, or delete all of a txid's entries if no non-COMPLETED ones are left).
        if (MakeCompleted(m_index.project<ByTxHash>(it))) {
            // Then actually delete the entry (unless it was already deleted by MakeCompleted).
            Erase<ByPeer>(it);
        }
        it = it_next;
    }
}

void TxRequestTracker::ReceivedInv(uint64_t peer, const GenTxid& gtxid, bool preferred, bool overloaded,
    std::chrono::microseconds reqtime)
{
    // Bail out if we already have a CANDIDATE_BEST entry for this (txid, peer) combination. The case where there is
    // a non-CANDIDATE_BEST entry already will be caught by the uniqueness property of the ByPeer index
    // automatically.
    if (m_index.get<ByPeer>().count(EntryPeer{peer, true, gtxid.GetHash()})) return;

    // Find last entry, and extract per_txhash information from it.
    uint8_t per_txhash = 0;
    typename TxRequestTracker::Index::index<ByTxHash>::type::iterator it_last = m_index.get<ByTxHash>().end();
    if (Size()) {
        it_last = std::prev(m_index.get<ByTxHash>().lower_bound(EntryTxHash{gtxid.GetHash(), State::TOO_LARGE, 0}));
        if (it_last->m_txhash == gtxid.GetHash()) {
            per_txhash |= it_last->m_per_txhash;
        } else {
            it_last = m_index.get<ByTxHash>().end();
        }
    }

    // Determine whether the new announcement's Entry will get the first marker, and update
    // the per_txhash information to be stored (but note that per_txhash isn't actually stored
    // until after the emplace below succeeds).
    bool first = false;
    if (!overloaded) {
        if (preferred && !(per_txhash & TXHASHINFO_NO_MORE_PREFERRED_FIRST)) {
            first = true;
            per_txhash |= TXHASHINFO_NO_MORE_PREFERRED_FIRST;
        } else if (!preferred && !(per_txhash & TXHASHINFO_NO_MORE_NONPREFERRED_FIRST)) {
            first = true;
            per_txhash |= TXHASHINFO_NO_MORE_NONPREFERRED_FIRST;
        }
    }

    // Try creating the entry with CANDIDATE_DELAYED state (which will fail due to the uniqueness
    // of the ByPeer index if a non-CANDIDATE_BEST entry already exists with the same txhash and peer).
    // Bail out in that case.
    auto ret = m_index.get<ByPeer>().emplace(gtxid, peer, preferred, reqtime, m_sequence, first);
    if (!ret.second) return;

    // Update accounting metadata.
    ++m_peerinfo[peer].m_total;
    ++m_sequence;

    // Update m_per_txhash of the new last Entry (either the newly created one, or it_last).
    auto it = m_index.project<ByTxHash>(ret.first);
    if (it_last == m_index.get<ByTxHash>().end() || std::next(it_last) == it) it_last = it;
    it_last->m_per_txhash |= per_txhash;
}

void TxRequestTracker::RequestedTx(uint64_t peer, const GenTxid& gtxid, std::chrono::microseconds exptime)
{
    auto it = m_index.get<ByPeer>().find(EntryPeer{peer, true, gtxid.GetHash()});
    // RequestedTx can only be called on CANDIDATE_BEST entries (this is implied by its condition that it can only be
    // called on txids returned by GetRequestable (and only AlreadyHave and RequestedTx can be called in between,
    // which preserve the state of other txids).
    assert(it != m_index.get<ByPeer>().end());
    assert(it->GetState() == State::CANDIDATE_BEST);
    Modify<ByPeer>(it, [exptime](Entry& entry) {
        entry.SetState(State::REQUESTED);
        entry.m_time = exptime;
    });

    // Update the m_per_txhash (of the last Entry for this txhash) to reflect that new ones are no longer
    // eligible for the "first" marker.
    auto it_last = std::prev(m_index.get<ByTxHash>().lower_bound(EntryTxHash{gtxid.GetHash(), State::TOO_LARGE, 0}));
    it_last->m_per_txhash |= TXHASHINFO_NO_MORE_PREFERRED_FIRST | TXHASHINFO_NO_MORE_NONPREFERRED_FIRST;
}

void TxRequestTracker::ReceivedResponse(uint64_t peer, const GenTxid& gtxid)
{
    // We need to search the ByPeer index for both (peer, false, txid) and (peer, true, txid).
    auto it = m_index.get<ByPeer>().find(EntryPeer{peer, false, gtxid.GetHash()});
    if (it == m_index.get<ByPeer>().end()) it = m_index.get<ByPeer>().find(EntryPeer{peer, true, gtxid.GetHash()});
    if (it != m_index.get<ByPeer>().end()) MakeCompleted(m_index.project<ByTxHash>(it));
}

size_t TxRequestTracker::CountInFlight(uint64_t peer) const
{
    auto it = m_peerinfo.find(peer);
    if (it != m_peerinfo.end()) return it->second.m_requested;
    return 0;
}

size_t TxRequestTracker::CountTracked(uint64_t peer) const
{
    auto it = m_peerinfo.find(peer);
    if (it != m_peerinfo.end()) return it->second.m_total;
    return 0;
}

std::vector<GenTxid> TxRequestTracker::GetRequestable(uint64_t peer, std::chrono::microseconds now)
{
    // Move time.
    SetTimePoint(now);

    // Find all CANDIDATE_BEST entries for this peer.
    std::vector<std::pair<uint64_t, const Entry*>> selected;
    auto it_peer = m_index.get<ByPeer>().lower_bound(EntryPeer{peer, true, UINT256_ZERO});
    while (it_peer != m_index.get<ByPeer>().end() && it_peer->m_peer == peer &&
        it_peer->GetState() == State::CANDIDATE_BEST) {
        selected.emplace_back(it_peer->m_sequence, &*it_peer);
        ++it_peer;
    }

    // Return them, sorted by sequence number.
    std::sort(selected.begin(), selected.end());
    std::vector<GenTxid> ret;
    for (const auto& item : selected) {
        ret.emplace_back(item.second->m_is_wtxid, item.second->m_txhash);
    }
    return ret;
}

void TxRequestTracker::SanityCheck() const
{
    // Recompute m_peerdata.
    // This verifies the data in it, including the invariant
    // that no entries with m_total_announcements==0 exist.
    std::unordered_map<uint64_t, PeerInfo> peerinfo;
    for (const auto& a : m_index) {
        auto& entry = peerinfo[a.m_peer];
        ++entry.m_total;
        entry.m_requested += (a.GetState() == State::REQUESTED);
    }
    assert(m_peerinfo == peerinfo);

    struct Counts {
        //! Number of CANDIDATE_DELAYED entries for this txhash.
        size_t m_candidate_delayed = 0;
        //! Number of CANDIDATE_READY entries for this txhash.
        size_t m_candidate_ready = 0;
        //! Number of CANDIDATE_BEST entries for this txhash (at most one).
        size_t m_candidate_best = 0;
        //! Number of REQUESTED entries for this txhash.
        size_t m_requested = 0;
        //! The priority of the CANDIDATE_BEST entry if one exists, or 0 otherwise.
        uint64_t m_priority_candidate_best = 0;
        //! The lowest priority of all CANDIDATE_READY entries (or max() if none exist).
        uint64_t m_priority_best_candidate_ready = std::numeric_limits<uint64_t>::max();
        //! All peers we have an entry for this txhash for.
        std::vector<uint64_t> m_peers;
        //! Whether any preferred first entry exists.
        bool m_any_preferred_first = false;
        //! Whether any non-preferred first entry exists.
        bool m_any_nonpreferred_first = false;
        //! OR of all m_per_txhash flags.
        uint8_t m_or_all_per_txhash = 0;
    };

    std::map<uint256, Counts> table;
    for (const auto& a : m_index) {
        auto& entry = table[a.m_txhash];
        // Classify how many types peers we have for this txid.
        entry.m_candidate_delayed += (a.GetState() == State::CANDIDATE_DELAYED);
        entry.m_candidate_ready += (a.GetState() == State::CANDIDATE_READY);
        entry.m_candidate_best += (a.GetState() == State::CANDIDATE_BEST);
        entry.m_requested += (a.GetState() == State::REQUESTED);
        // And track the priority of the best CANDIDATE_READY/CANDIDATE_BEST entries.
        if (a.GetState() == State::CANDIDATE_BEST) entry.m_priority_candidate_best = a.ComputePriority(m_computer);
        if (a.GetState() == State::CANDIDATE_READY) {
            entry.m_priority_best_candidate_ready = std::min(entry.m_priority_best_candidate_ready,
                a.ComputePriority(m_computer));
        }
        // Also keep track of which peers this txid has an entry for (so we can detect duplicates).
        entry.m_peers.push_back(a.m_peer);
        // Track preferred/first.
        entry.m_any_preferred_first |= (a.m_first && a.m_preferred);
        entry.m_any_nonpreferred_first |= (a.m_first && !a.m_preferred);
        entry.m_or_all_per_txhash |= a.m_per_txhash;
    }
    for (auto& entry : table) {
        Counts& c{entry.second};
        // Cannot have only COMPLETED peers (txid should have been deleted)
        assert(c.m_candidate_delayed + c.m_candidate_ready + c.m_candidate_best + c.m_requested > 0);
        // Can have at most 1 CANDIDATE_BEST/REQUESTED peer
        assert(c.m_candidate_best + c.m_requested <= 1);
        // If there are any CANDIDATE_READY entries, there must be exactly one CANDIDATE_BEST or REQUESTED entry.
        if (c.m_candidate_ready > 0) {
            assert(c.m_candidate_best + c.m_requested == 1);
        }
        // If there is both a CANDIDATE_READY and a CANDIDATE_BEST entry, the CANDIDATE_BEST one must be at least
        // as good as the best CANDIDATE_READY.
        if (c.m_candidate_ready && c.m_candidate_best) {
            assert(c.m_priority_candidate_best <= c.m_priority_best_candidate_ready);
        }
        // Detect duplicate (peer, txid) entries
        std::sort(c.m_peers.begin(), c.m_peers.end());
        assert(std::adjacent_find(c.m_peers.begin(), c.m_peers.end()) == c.m_peers.end());
        // Verify all per_txhash flags.
        uint8_t expected_per_txhash = 0;
        if (c.m_any_preferred_first || c.m_requested) {
            expected_per_txhash |= TXHASHINFO_NO_MORE_PREFERRED_FIRST;
        }
        if (c.m_any_nonpreferred_first || c.m_requested) {
            expected_per_txhash |= TXHASHINFO_NO_MORE_NONPREFERRED_FIRST;
        }
        // All expected flags must be present, but there can be more. If a node went from REQUESTED to COMPLETED,
        // or was deleted, our expected_per_txhash may miss the relevant bits.
        assert((expected_per_txhash & ~c.m_or_all_per_txhash) == 0);
        // No entry can have flags that are a superset of the actual ones (they're always ORed into the actual one).
        auto it_last = std::prev(m_index.get<ByTxHash>().lower_bound(EntryTxHash{entry.first, State::TOO_LARGE, 0}));
        assert(it_last->m_txhash == entry.first);
        assert(c.m_or_all_per_txhash == it_last->m_per_txhash);
    }
}

void TxRequestTracker::TimeSanityCheck(std::chrono::microseconds now) const
{
    for (const auto& entry : m_index) {
        if (entry.IsWaiting()) {
            // REQUESTED and CANDIDATE_DELAYED must have a time in the future (they should have been converted to
            // COMPLETED/CANDIDATE_READY respectively).
            assert(entry.m_time > now);
        } else if (entry.IsSelectable()) {
            // CANDIDATE_READY and CANDIDATE_BEST cannot have a time in the future (they should have remained
            // CANDIDATE_DELAYED, or should have been converted back to it if time went backwards).
            assert(entry.m_time <= now);
        }
    }
}
