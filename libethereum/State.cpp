/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file State.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "State.h"

#include <boost/filesystem.hpp>
#include <time.h>
#include <random>
#include <secp256k1/secp256k1.h>
#include <libevmface/Instruction.h>
#include <libethcore/Exceptions.h>
#include <libethcore/Dagger.h>
#include <libevm/VM.h>
#include "BlockChain.h"
#include "Defaults.h"
#include "ExtVM.h"
using namespace std;
using namespace eth;

#define ctrace clog(StateTrace)

static const u256 c_blockReward = 1500 * finney;

OverlayDB State::openDB(std::string _path, bool _killExisting)
{
	if (_path.empty())
		_path = Defaults::get()->m_dbPath;
	boost::filesystem::create_directory(_path);

	if (_killExisting)
		boost::filesystem::remove_all(_path + "/state");

	ldb::Options o;
	o.create_if_missing = true;
	ldb::DB* db = nullptr;
	ldb::DB::Open(o, _path + "/state", &db);
	cnote << "Opened state DB.";
	return OverlayDB(db);
}

State::State(Address _coinbaseAddress, OverlayDB const& _db):
	m_db(_db),
	m_state(&m_db),
	m_ourAddress(_coinbaseAddress),
	m_blockReward(c_blockReward)
{
	secp256k1_start();

	// Initialise to the state entailed by the genesis block; this guarantees the trie is built correctly.
	m_state.init();

	paranoia("beginning of normal construction.", true);

	eth::commit(genesisState(), m_db, m_state);
	m_db.commit();

	paranoia("after DB commit of normal construction.", true);

	m_previousBlock = BlockChain::genesis();
	resetCurrent();

	assert(m_state.root() == m_previousBlock.stateRoot);

	paranoia("end of normal construction.", true);
}

State::State(OverlayDB const& _db, BlockChain const& _bc, h256 _h):
	m_db(_db),
	m_state(&m_db),
	m_blockReward(c_blockReward)
{
	secp256k1_start();

	// TODO THINK: is this necessary?
	m_state.init();

	auto b = _bc.block(_h);
	BlockInfo bi;
	BlockInfo bip;
	if (_h)
		bi.populate(b);
	if (bi && bi.number)
		bip.populate(_bc.block(bi.parentHash));
	if (!_h || !bip)
		return;
	m_ourAddress = bi.coinbaseAddress;

	sync(_bc, bi.parentHash, bip);
	enact(&b);
}

State::State(State const& _s):
	m_db(_s.m_db),
	m_state(&m_db, _s.m_state.root()),
	m_transactions(_s.m_transactions),
	m_transactionSet(_s.m_transactionSet),
	m_cache(_s.m_cache),
	m_previousBlock(_s.m_previousBlock),
	m_currentBlock(_s.m_currentBlock),
	m_ourAddress(_s.m_ourAddress),
	m_blockReward(_s.m_blockReward)
{
	paranoia("after state cloning (copy cons).", true);
}

void State::paranoia(std::string const& _when, bool _enforceRefs) const
{
#if ETH_PARANOIA
	// TODO: variable on context; just need to work out when there should be no leftovers
	// [in general this is hard since contract alteration will result in nodes in the DB that are no directly part of the state DB].
	if (!isTrieGood(_enforceRefs, false))
	{
		cwarn << "BAD TRIE" << _when;
		throw InvalidTrie();
	}
#else
	(void)_when;
	(void)_enforceRefs;
#endif
}

State& State::operator=(State const& _s)
{
	m_db = _s.m_db;
	m_state.open(&m_db, _s.m_state.root());
	m_transactions = _s.m_transactions;
	m_transactionSet = _s.m_transactionSet;
	m_cache = _s.m_cache;
	m_previousBlock = _s.m_previousBlock;
	m_currentBlock = _s.m_currentBlock;
	m_ourAddress = _s.m_ourAddress;
	m_blockReward = _s.m_blockReward;
	paranoia("after state cloning (assignment op)", true);
	return *this;
}

struct CachedAddressState
{
	CachedAddressState(std::string const& _rlp, AddressState const* _s, OverlayDB const* _o): rS(_rlp), r(rS), s(_s), o(_o) {}

	bool exists() const
	{
		return (r && (!s || s->isAlive())) || (s && s->isAlive());
	}

	u256 balance() const
	{
		return r ? s ? s->balance() : r[1].toInt<u256>() : 0;
	}

	u256 nonce() const
	{
		return r ? s ? s->nonce() : r[0].toInt<u256>() : 0;
	}

	bytes code() const
	{
		if (s && s->codeCacheValid())
			return s->code();
		h256 h = r ? s ? s->codeHash() : r[3].toHash<h256>() : EmptySHA3;
		return h == EmptySHA3 ? bytes() : asBytes(o->lookup(h));
	}

	std::map<u256, u256> storage() const
	{
		std::map<u256, u256> ret;
		if (r)
		{
			TrieDB<h256, OverlayDB> memdb(const_cast<OverlayDB*>(o), r[2].toHash<h256>());		// promise we won't alter the overlay! :)
			for (auto const& j: memdb)
				ret[j.first] = RLP(j.second).toInt<u256>();
		}
		if (s)
			for (auto const& j: s->storage())
				if ((!ret.count(j.first) && j.second) || (ret.count(j.first) && ret.at(j.first) != j.second))
					ret[j.first] = j.second;
		return ret;
	}

	AccountDiff diff(CachedAddressState const& _c)
	{
		AccountDiff ret;
		ret.exist = Diff<bool>(exists(), _c.exists());
		ret.balance = Diff<u256>(balance(), _c.balance());
		ret.nonce = Diff<u256>(nonce(), _c.nonce());
		ret.code = Diff<bytes>(code(), _c.code());
		auto st = storage();
		auto cst = _c.storage();
		auto it = st.begin();
		auto cit = cst.begin();
		while (it != st.end() || cit != cst.end())
		{
			if (it != st.end() && cit != cst.end() && it->first == cit->first && (it->second || cit->second) && (it->second != cit->second))
				ret.storage[it->first] = Diff<u256>(it->second, cit->second);
			else if (it != st.end() && (cit == cst.end() || it->first < cit->first) && it->second)
				ret.storage[it->first] = Diff<u256>(it->second, 0);
			else if (cit != cst.end() && (it == st.end() || it->first > cit->first) && cit->second)
				ret.storage[cit->first] = Diff<u256>(0, cit->second);
			if (it == st.end())
				++cit;
			else if (cit == cst.end())
				++it;
			else if (it->first < cit->first)
				++it;
			else if (it->first > cit->first)
				++cit;
			else
				++it, ++cit;
		}
		return ret;
	}

	std::string rS;
	RLP r;
	AddressState const* s;
	OverlayDB const* o;
};

StateDiff State::diff(State const& _c) const
{
	StateDiff ret;

	std::set<Address> ads;
	std::set<Address> trieAds;
	std::set<Address> trieAdsD;

	auto trie = TrieDB<Address, OverlayDB>(const_cast<OverlayDB*>(&m_db), rootHash());
	auto trieD = TrieDB<Address, OverlayDB>(const_cast<OverlayDB*>(&_c.m_db), _c.rootHash());

	for (auto i: trie)
		ads.insert(i.first), trieAds.insert(i.first);
	for (auto i: trieD)
		ads.insert(i.first), trieAdsD.insert(i.first);
	for (auto i: m_cache)
		ads.insert(i.first);
	for (auto i: _c.m_cache)
		ads.insert(i.first);

	for (auto i: ads)
	{
		auto it = m_cache.find(i);
		auto itD = _c.m_cache.find(i);
		CachedAddressState source(trieAds.count(i) ? trie.at(i) : "", it != m_cache.end() ? &it->second : nullptr, &m_db);
		CachedAddressState dest(trieAdsD.count(i) ? trieD.at(i) : "", itD != _c.m_cache.end() ? &itD->second : nullptr, &_c.m_db);
		AccountDiff acd = source.diff(dest);
		if (acd.changed())
			ret.accounts[i] = acd;
	}

	return ret;
}

void State::ensureCached(Address _a, bool _requireCode, bool _forceCreate) const
{
	ensureCached(m_cache, _a, _requireCode, _forceCreate);
}

void State::ensureCached(std::map<Address, AddressState>& _cache, Address _a, bool _requireCode, bool _forceCreate) const
{
	auto it = _cache.find(_a);
	if (it == _cache.end())
	{
		// populate basic info.
		string stateBack = m_state.at(_a);
		if (stateBack.empty() && !_forceCreate)
			return;
		RLP state(stateBack);
		AddressState s;
		if (state.isNull())
			s = AddressState(0, 0, h256(), EmptySHA3);
		else
			s = AddressState(state[0].toInt<u256>(), state[1].toInt<u256>(), state[2].toHash<h256>(), state[3].toHash<h256>());
		bool ok;
		tie(it, ok) = _cache.insert(make_pair(_a, s));
	}
	if (_requireCode && it != _cache.end() && !it->second.isFreshCode() && !it->second.codeCacheValid())
		it->second.noteCode(it->second.codeHash() == EmptySHA3 ? bytesConstRef() : bytesConstRef(m_db.lookup(it->second.codeHash())));
}

void State::commit()
{
	eth::commit(m_cache, m_db, m_state);
	m_cache.clear();
}

bool State::sync(BlockChain const& _bc)
{
	return sync(_bc, _bc.currentHash());
}

bool State::sync(BlockChain const& _bc, h256 _block, BlockInfo const& _bi)
{
	bool ret = false;
	// BLOCK
	BlockInfo bi = _bi;
	try
	{
		if (!bi)
		{
			auto b = _bc.block(_block);
			bi.populate(b);
//			bi.verifyInternals(_bc.block(_block));	// Unneeded - we already verify on import into the blockchain.
		}
	}
	catch (...)
	{
		// TODO: Slightly nicer handling? :-)
		cerr << "ERROR: Corrupt block-chain! Delete your block-chain DB and restart." << endl;
		exit(1);
	}

	if (bi == m_currentBlock)
	{
		// We mined the last block.
		// Our state is good - we just need to move on to next.
		m_previousBlock = m_currentBlock;
		resetCurrent();
		ret = true;
	}
	else if (bi == m_previousBlock)
	{
		// No change since last sync.
		// Carry on as we were.
	}
	else
	{
		// New blocks available, or we've switched to a different branch. All change.
		// Find most recent state dump and replay what's left.
		// (Most recent state dump might end up being genesis.)

		std::vector<h256> chain;
		while (bi.stateRoot != BlockChain::genesis().hash && m_db.lookup(bi.stateRoot).empty())	// while we don't have the state root of the latest block...
		{
			chain.push_back(bi.hash);				// push back for later replay.
			bi.populate(_bc.block(bi.parentHash));	// move to parent.
		}

		m_previousBlock = bi;
		resetCurrent();

		// Iterate through in reverse, playing back each of the blocks.
		try
		{
			for (auto it = chain.rbegin(); it != chain.rend(); ++it)
			{
				auto b = _bc.block(*it);
				enact(&b);
				cleanup(true);
			}
		}
		catch (...)
		{
			// TODO: Slightly nicer handling? :-)
			cerr << "ERROR: Corrupt block-chain! Delete your block-chain DB and restart." << endl;
			exit(1);
		}

		resetCurrent();
		ret = true;
	}
	return ret;
}

u256 State::enactOn(bytesConstRef _block, BlockInfo const& _bi, BlockChain const& _bc)
{
	// Check family:
	BlockInfo biParent(_bc.block(_bi.parentHash));
	_bi.verifyParent(biParent);
	BlockInfo biGrandParent;
	if (biParent.number)
		biGrandParent.populate(_bc.block(biParent.parentHash));
	sync(_bc, _bi.parentHash);
	resetCurrent();
	m_previousBlock = biParent;
	return enact(_block, biGrandParent);
}

map<Address, u256> State::addresses() const
{
	map<Address, u256> ret;
	for (auto i: m_cache)
		if (i.second.isAlive())
			ret[i.first] = i.second.balance();
	for (auto const& i: m_state)
		if (m_cache.find(i.first) == m_cache.end())
			ret[i.first] = RLP(i.second)[1].toInt<u256>();
	return ret;
}

void State::resetCurrent()
{
	m_transactions.clear();
	m_transactionSet.clear();
	m_cache.clear();
	m_currentBlock = BlockInfo();
	m_currentBlock.coinbaseAddress = m_ourAddress;
	m_currentBlock.timestamp = time(0);
	m_currentBlock.transactionsRoot = h256();
	m_currentBlock.sha3Uncles = h256();
	m_currentBlock.minGasPrice = 10 * szabo;
	m_currentBlock.populateFromParent(m_previousBlock);

	// Update timestamp according to clock.
	// TODO: check.

	m_lastTx = m_db;
	m_state.setRoot(m_previousBlock.stateRoot);

	paranoia("begin resetCurrent", true);
}

bool State::cull(TransactionQueue& _tq) const
{
	bool ret = false;
	auto ts = _tq.transactions();
	for (auto const& i: ts)
	{
		if (!m_transactionSet.count(i.first))
		{
			try
			{
				Transaction t(i.second);
				if (t.nonce <= transactionsFrom(t.sender()))
				{
					_tq.drop(i.first);
					ret = true;
				}
			}
			catch (...)
			{
				_tq.drop(i.first);
				ret = true;
			}
		}
	}
	return ret;
}

h256s State::sync(TransactionQueue& _tq, bool* o_transactionQueueChanged)
{
	// TRANSACTIONS
	h256s ret;
	auto ts = _tq.transactions();

	for (int goodTxs = 1; goodTxs;)
	{
		goodTxs = 0;
		for (auto const& i: ts)
			if (!m_transactionSet.count(i.first))
			{
				// don't have it yet! Execute it now.
				try
				{
					uncommitToMine();
					execute(i.second);
					ret.push_back(m_transactions.back().changes.bloom());
					_tq.noteGood(i);
					++goodTxs;
				}
				catch (InvalidNonce const& in)
				{
					if (in.required > in.candidate)
					{
						// too old
						_tq.drop(i.first);
						if (o_transactionQueueChanged)
							*o_transactionQueueChanged = true;
					}
					else
						_tq.setFuture(i);
				}
				catch (std::exception const&)
				{
					// Something else went wrong - drop it.
					_tq.drop(i.first);
					if (o_transactionQueueChanged)
						*o_transactionQueueChanged = true;
				}
			}
	}
	return ret;
}

u256 State::enact(bytesConstRef _block, BlockInfo const& _grandParent, bool _checkNonce)
{
	// m_currentBlock is assumed to be prepopulated and reset.

#if !ETH_RELEASE
	BlockInfo bi(_block);
	assert(m_previousBlock.hash == bi.parentHash);
	assert(m_currentBlock.parentHash == bi.parentHash);
	assert(rootHash() == m_previousBlock.stateRoot);
#endif

	if (m_currentBlock.parentHash != m_previousBlock.hash)
		throw InvalidParentHash();

	// Populate m_currentBlock with the correct values.
	m_currentBlock.populate(_block, _checkNonce);
	m_currentBlock.verifyInternals(_block);

//	cnote << "playback begins:" << m_state.root();
//	cnote << m_state;

	MemoryDB tm;
	GenericTrieDB<MemoryDB> transactionManifest(&tm);
	transactionManifest.init();

	// All ok with the block generally. Play back the transactions now...
	unsigned i = 0;
	for (auto const& tr: RLP(_block)[1])
	{
//		cnote << m_state.root() << m_state;
//		cnote << *this;
		execute(tr[0].data());
		if (tr[1].toHash<h256>() != m_state.root())
		{
			// Invalid state root
			cnote << m_state.root() << "\n" << m_state;
			cnote << *this;
			cnote << "INVALID: " << tr[1].toHash<h256>();
			throw InvalidTransactionStateRoot();
		}
		if (tr[2].toInt<u256>() != gasUsed())
			throw InvalidTransactionGasUsed();
		bytes k = rlp(i);
		transactionManifest.insert(&k, tr.data());
		++i;
	}

	if (m_currentBlock.transactionsRoot && transactionManifest.root() != m_currentBlock.transactionsRoot)
	{
		cwarn << "Bad transactions state root!";
		throw InvalidTransactionStateRoot();
	}

	// Initialise total difficulty calculation.
	u256 tdIncrease = m_currentBlock.difficulty;

	// Check uncles & apply their rewards to state.
	set<h256> nonces = { m_currentBlock.nonce };
	Addresses rewarded;
	for (auto const& i: RLP(_block)[2])
	{
		BlockInfo uncle = BlockInfo::fromHeader(i.data());

		if (m_previousBlock.parentHash != uncle.parentHash)
			throw UncleNotAnUncle();
		if (nonces.count(uncle.nonce))
			throw DuplicateUncleNonce();
		if (_grandParent)
			uncle.verifyParent(_grandParent);

		nonces.insert(uncle.nonce);
		tdIncrease += uncle.difficulty;
		rewarded.push_back(uncle.coinbaseAddress);
	}
	applyRewards(rewarded);

	// Commit all cached state changes to the state trie.
	commit();

	// Hash the state trie and check against the state_root hash in m_currentBlock.
	if (m_currentBlock.stateRoot != m_previousBlock.stateRoot && m_currentBlock.stateRoot != rootHash())
	{
		cwarn << "Bad state root!";
		cnote << "Given to be:" << m_currentBlock.stateRoot;
		cnote << TrieDB<Address, OverlayDB>(&m_db, m_currentBlock.stateRoot);
		cnote << "Calculated to be:" << rootHash();
		cnote << m_state;
		cnote << *this;
		// Rollback the trie.
		m_db.rollback();
		throw InvalidStateRoot();
	}

	return tdIncrease;
}

void State::cleanup(bool _fullCommit)
{
	if (_fullCommit)
	{
		paranoia("immediately before database commit", true);

		// Commit the new trie to disk.
		m_db.commit();

		paranoia("immediately after database commit", true);
		m_previousBlock = m_currentBlock;
	}
	else
	{
		m_db.rollback();
	}

	resetCurrent();
}

void State::uncommitToMine()
{
	if (m_currentBlock.sha3Uncles)
	{
		m_cache.clear();
		if (!m_transactions.size())
			m_state.setRoot(m_previousBlock.stateRoot);
		else
			m_state.setRoot(m_transactions[m_transactions.size() - 1].stateRoot);
		m_db = m_lastTx;
		paranoia("Uncommited to mine", true);
		m_currentBlock.sha3Uncles = h256();
	}
}

bool State::amIJustParanoid(BlockChain const& _bc)
{
	commitToMine(_bc);

	// Update difficulty according to timestamp.
	m_currentBlock.difficulty = m_currentBlock.calculateDifficulty(m_previousBlock);

	// Compile block:
	RLPStream block;
	block.appendList(3);
	m_currentBlock.fillStream(block, true);
	block.appendRaw(m_currentTxs);
	block.appendRaw(m_currentUncles);

	State s(*this);
	s.resetCurrent();
	try
	{
		cnote << "PARANOIA root:" << s.rootHash();
//		s.m_currentBlock.populate(&block.out(), false);
//		s.m_currentBlock.verifyInternals(&block.out());
		s.enact(&block.out(), BlockInfo(), false);	// don't check nonce for this since we haven't mined it yet.
		s.cleanup(false);
		return true;
	}
	catch (Exception const& e)
	{
		cwarn << "Bad block: " << e.description();
	}
	catch (std::exception const& e)
	{
		cwarn << "Bad block: " << e.what();
	}

	return false;
}

h256 State::bloom() const
{
	h256 ret = m_currentBlock.coinbaseAddress.bloom();
	for (auto const& i: m_transactions)
		ret |= i.changes.bloom();
	return ret;
}

// @returns the block that represents the difference between m_previousBlock and m_currentBlock.
// (i.e. all the transactions we executed).
void State::commitToMine(BlockChain const& _bc)
{
	uncommitToMine();

	cnote << "Committing to mine on block" << m_previousBlock.hash;
#ifdef ETH_PARANOIA
	commit();
	cnote << "Pre-reward stateRoot:" << m_state.root();
#endif

	m_lastTx = m_db;

	RLPStream uncles;
	Addresses uncleAddresses;

	if (m_previousBlock != BlockChain::genesis())
	{
		// Find uncles if we're not a direct child of the genesis.
//		cout << "Checking " << m_previousBlock.hash << ", parent=" << m_previousBlock.parentHash << endl;
		auto us = _bc.details(m_previousBlock.parentHash).children;
		assert(us.size() >= 1);	// must be at least 1 child of our grandparent - it's our own parent!
		uncles.appendList(us.size() - 1);	// one fewer - uncles precludes our parent from the list of grandparent's children.
		for (auto const& u: us)
			if (u != m_previousBlock.hash)	// ignore our own parent - it's not an uncle.
			{
				BlockInfo ubi(_bc.block(u));
				ubi.fillStream(uncles, true);
				uncleAddresses.push_back(ubi.coinbaseAddress);
			}
	}
	else
		uncles.appendList(0);

	MemoryDB tm;
	GenericTrieDB<MemoryDB> transactionReceipts(&tm);
	transactionReceipts.init();

	RLPStream txs;
	txs.appendList(m_transactions.size());

	for (unsigned i = 0; i < m_transactions.size(); ++i)
	{
		RLPStream k;
		k << i;
		RLPStream v;
		m_transactions[i].fillStream(v);
		transactionReceipts.insert(&k.out(), &v.out());
		txs.appendRaw(v.out());
	}

	txs.swapOut(m_currentTxs);
	uncles.swapOut(m_currentUncles);

	m_currentBlock.transactionsRoot = transactionReceipts.root();
	m_currentBlock.sha3Uncles = sha3(m_currentUncles);

	// Apply rewards last of all.
	applyRewards(uncleAddresses);

	// Commit any and all changes to the trie that are in the cache, then update the state root accordingly.
	commit();

	cnote << "Post-reward stateRoot:" << m_state.root();
//	cnote << m_state;
//	cnote << *this;

	m_currentBlock.gasUsed = gasUsed();
	m_currentBlock.stateRoot = m_state.root();
	m_currentBlock.parentHash = m_previousBlock.hash;
}

MineInfo State::mine(uint _msTimeout, bool _turbo)
{
	// Update difficulty according to timestamp.
	m_currentBlock.difficulty = m_currentBlock.calculateDifficulty(m_previousBlock);

	// TODO: Miner class that keeps dagger between mine calls (or just non-polling mining).
	auto ret = m_dagger.mine(/*out*/m_currentBlock.nonce, m_currentBlock.headerHashWithoutNonce(), m_currentBlock.difficulty, _msTimeout, true, _turbo);

	if (!ret.completed)
		m_currentBytes.clear();

	return ret;
}

void State::completeMine()
{
	cdebug << "Completing mine!";
	// Got it!

	// Commit to disk.
	m_db.commit();

	// Compile block:
	RLPStream ret;
	ret.appendList(3);
	m_currentBlock.fillStream(ret, true);
	ret.appendRaw(m_currentTxs);
	ret.appendRaw(m_currentUncles);
	ret.swapOut(m_currentBytes);
	m_currentBlock.hash = sha3(m_currentBytes);
	cnote << "Mined " << m_currentBlock.hash << "(parent: " << m_currentBlock.parentHash << ")";

	// Quickly reset the transactions.
	// TODO: Leave this in a better state than this limbo, or at least record that it's in limbo.
	m_transactions.clear();
	m_transactionSet.clear();
	m_lastTx = m_db;
}

bool State::addressInUse(Address _id) const
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return false;
	return true;
}

bool State::addressHasCode(Address _id) const
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return false;
	return it->second.isFreshCode() || it->second.codeHash() != EmptySHA3;
}

u256 State::balance(Address _id) const
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return 0;
	return it->second.balance();
}

void State::noteSending(Address _id)
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		m_cache[_id] = AddressState(1, 0, h256(), EmptySHA3);
	else
		it->second.incNonce();
}

void State::addBalance(Address _id, u256 _amount)
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		m_cache[_id] = AddressState(0, _amount, h256(), EmptySHA3);
	else
		it->second.addBalance(_amount);
}

void State::subBalance(Address _id, bigint _amount)
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end() || (bigint)it->second.balance() < _amount)
		throw NotEnoughCash();
	else
		it->second.addBalance(-_amount);
}

u256 State::transactionsFrom(Address _id) const
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it == m_cache.end())
		return 0;
	else
		return it->second.nonce();
}

u256 State::storage(Address _id, u256 _memory) const
{
	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);

	// Account doesn't exist - exit now.
	if (it == m_cache.end())
		return 0;

	// See if it's in the account's storage cache.
	auto mit = it->second.storage().find(_memory);
	if (mit != it->second.storage().end())
		return mit->second;

	// Not in the storage cache - go to the DB.
	TrieDB<h256, OverlayDB> memdb(const_cast<OverlayDB*>(&m_db), it->second.baseRoot());			// promise we won't change the overlay! :)
	string payload = memdb.at(_memory);
	u256 ret = payload.size() ? RLP(payload).toInt<u256>() : 0;
	it->second.setStorage(_memory, ret);
	return ret;
}

map<u256, u256> State::storage(Address _id) const
{
	map<u256, u256> ret;

	ensureCached(_id, false, false);
	auto it = m_cache.find(_id);
	if (it != m_cache.end())
	{
		// Pull out all values from trie storage.
		if (it->second.baseRoot())
		{
			TrieDB<h256, OverlayDB> memdb(const_cast<OverlayDB*>(&m_db), it->second.baseRoot());		// promise we won't alter the overlay! :)
			for (auto const& i: memdb)
				ret[i.first] = RLP(i.second).toInt<u256>();
		}

		// Then merge cached storage over the top.
		for (auto const& i: it->second.storage())
			if (i.second)
				ret[i.first] = i.second;
			else
				ret.erase(i.first);
	}
	return ret;
}

h256 State::storageRoot(Address _id) const
{
	string s = m_state.at(_id);
	if (s.size())
	{
		RLP r(s);
		return r[2].toHash<h256>();
	}
	return h256();
}

bytes const& State::code(Address _contract) const
{
	if (!addressHasCode(_contract))
		return NullBytes;
	ensureCached(_contract, true, false);
	return m_cache[_contract].code();
}

bool State::isTrieGood(bool _enforceRefs, bool _requireNoLeftOvers) const
{
	for (int e = 0; e < (_enforceRefs ? 2 : 1); ++e)
		try
		{
			EnforceRefs r(m_db, !!e);
			auto lo = m_state.leftOvers();
			if (!lo.empty() && _requireNoLeftOvers)
			{
				cwarn << "LEFTOVERS" << (e ? "[enforced" : "[unenforced") << "refs]";
				cnote << "Left:" << lo;
				cnote << "Keys:" << m_db.keys();
				m_state.debugStructure(cerr);
				return false;
			}
			// TODO: Enable once fixed.
			for (auto const& i: m_state)
			{
				RLP r(i.second);
				TrieDB<h256, OverlayDB> storageDB(const_cast<OverlayDB*>(&m_db), r[2].toHash<h256>());	// promise not to alter OverlayDB.
				for (auto const& j: storageDB) { (void)j; }
				if (!e && r[3].toHash<h256>() != EmptySHA3 &&  m_db.lookup(r[3].toHash<h256>()).empty())
					return false;
			}
		}
		catch (InvalidTrie)
		{
			cwarn << "BAD TRIE" << (e ? "[enforced" : "[unenforced") << "refs]";
			cnote << m_db.keys();
			m_state.debugStructure(cerr);
			return false;
		}
	return true;
}

// TODO: maintain node overlay revisions for stateroots -> each commit gives a stateroot + OverlayDB; allow overlay copying for rewind operations.

u256 State::execute(bytesConstRef _rlp, bytes* o_output, bool _commit)
{
#ifndef ETH_RELEASE
	commit();	// get an updated hash
#endif

	paranoia("start of execution.", true);

	State old(*this);
#if ETH_PARANOIA
	auto h = rootHash();
#endif

	Manifest ms;

	Executive e(*this, &ms);
	e.setup(_rlp);

	u256 startGasUsed = gasUsed();

#if ETH_PARANOIA
	ctrace << "Executing" << e.t() << "on" << h;
	ctrace << toHex(e.t().rlp(true));
#endif

	e.go();
	e.finalize();

#if ETH_PARANOIA
	ctrace << "Ready for commit;";
	ctrace << old.diff(*this);
#endif

	if (o_output)
		*o_output = e.out().toBytes();

	if (!_commit)
	{
		m_cache.clear();
		return e.gasUsed();
	}

	commit();

#if ETH_PARANOIA
	ctrace << "Executed; now" << rootHash();
	ctrace << old.diff(*this);

	paranoia("after execution commit.", true);

	if (e.t().receiveAddress)
	{
		EnforceRefs r(m_db, true);
		if (storageRoot(e.t().receiveAddress) && m_db.lookup(storageRoot(e.t().receiveAddress)).empty())
		{
			cwarn << "TRIE immediately after execution; no node for receiveAddress";
			throw InvalidTrie();
		}
	}
#endif

	// TODO: CHECK TRIE after level DB flush to make sure exactly the same.

	// Add to the user-originated transactions that we've executed.
	m_transactions.push_back(TransactionReceipt(e.t(), rootHash(), startGasUsed + e.gasUsed(), ms));
	m_transactionSet.insert(e.t().sha3());
	return e.gasUsed();
}

bool State::call(Address _receiveAddress, Address _senderAddress, u256 _value, u256 _gasPrice, bytesConstRef _data, u256* _gas, bytesRef _out, Address _originAddress, std::set<Address>* o_suicides, Manifest* o_ms, OnOpFunc const& _onOp, unsigned _level)
{
	if (!_originAddress)
		_originAddress = _senderAddress;

//	cnote << "Transferring" << formatBalance(_value) << "to receiver.";
	addBalance(_receiveAddress, _value);

	if (o_ms)
	{
		o_ms->from = _senderAddress;
		o_ms->to = _receiveAddress;
		o_ms->value = _value;
		o_ms->input = _data.toBytes();
	}

	if (addressHasCode(_receiveAddress))
	{
		VM vm(*_gas);
		ExtVM evm(*this, _receiveAddress, _senderAddress, _originAddress, _value, _gasPrice, _data, &code(_receiveAddress), o_ms, _level);
		bool revert = false;

		try
		{
			auto out = vm.go(evm, _onOp);
			memcpy(_out.data(), out.data(), std::min(out.size(), _out.size()));
			if (o_suicides)
				for (auto i: evm.suicides)
					o_suicides->insert(i);
			if (o_ms)
				o_ms->output = out.toBytes();
		}
		catch (OutOfGas const& /*_e*/)
		{
			clog(StateChat) << "Out of Gas! Reverting.";
			revert = true;
		}
		catch (VMException const& _e)
		{
			clog(StateChat) << "VM Exception: " << _e.description();
		}
		catch (Exception const& _e)
		{
			clog(StateChat) << "Exception in VM: " << _e.description();
		}
		catch (std::exception const& _e)
		{
			clog(StateChat) << "std::exception in VM: " << _e.what();
		}

		// Write state out only in the case of a non-excepted transaction.
		if (revert)
			evm.revert();

		*_gas = vm.gas();

		return !revert;
	}
	return true;
}

h160 State::create(Address _sender, u256 _endowment, u256 _gasPrice, u256* _gas, bytesConstRef _code, Address _origin, std::set<Address>* o_suicides, Manifest* o_ms, OnOpFunc const& _onOp, unsigned _level)
{
	if (!_origin)
		_origin = _sender;

	if (o_ms)
	{
		o_ms->from = _sender;
		o_ms->to = Address();
		o_ms->value = _endowment;
		o_ms->input = _code.toBytes();
	}

	Address newAddress = right160(sha3(rlpList(_sender, transactionsFrom(_sender) - 1)));
	while (addressInUse(newAddress))
		newAddress = (u160)newAddress + 1;

	// Set up new account...
	m_cache[newAddress] = AddressState(0, _endowment, h256(), h256());

	// Execute init code.
	VM vm(*_gas);
	ExtVM evm(*this, newAddress, _sender, _origin, _endowment, _gasPrice, bytesConstRef(), _code, o_ms, _level);
	bool revert = false;
	bytesConstRef out;

	try
	{
		out = vm.go(evm, _onOp);
		if (o_ms)
			o_ms->output = out.toBytes();
		if (o_suicides)
			for (auto i: evm.suicides)
				o_suicides->insert(i);
	}
	catch (OutOfGas const& /*_e*/)
	{
		clog(StateChat) << "Out of Gas! Reverting.";
		revert = true;
	}
	catch (VMException const& _e)
	{
		clog(StateChat) << "VM Exception: " << _e.description();
	}
	catch (Exception const& _e)
	{
		clog(StateChat) << "Exception in VM: " << _e.description();
	}
	catch (std::exception const& _e)
	{
		clog(StateChat) << "std::exception in VM: " << _e.what();
	}

	// TODO: CHECK: IS THIS CORRECT?! (esp. given account created prior to revertion init.)

	// Write state out only in the case of a non-out-of-gas transaction.
	if (revert)
		evm.revert();

	// Set code.
	if (addressInUse(newAddress))
		m_cache[newAddress].setCode(out);

	*_gas = vm.gas();

	return newAddress;
}

State State::fromPending(unsigned _i) const
{
	State ret = *this;
	ret.m_cache.clear();
	_i = min<unsigned>(_i, m_transactions.size());
	if (!_i)
		ret.m_state.setRoot(m_previousBlock.stateRoot);
	else
		ret.m_state.setRoot(m_transactions[_i - 1].stateRoot);
	while (ret.m_transactions.size() > _i)
	{
		ret.m_transactionSet.erase(ret.m_transactions.back().transaction.sha3());
		ret.m_transactions.pop_back();
	}
	return ret;
}

void State::applyRewards(Addresses const& _uncleAddresses)
{
	u256 r = m_blockReward;
	for (auto const& i: _uncleAddresses)
	{
		addBalance(i, m_blockReward * 3 / 4);
		r += m_blockReward / 8;
	}
	addBalance(m_currentBlock.coinbaseAddress, r);
}

std::ostream& eth::operator<<(std::ostream& _out, State const& _s)
{
	_out << "--- " << _s.rootHash() << std::endl;
	std::set<Address> d;
	std::set<Address> dtr;
	auto trie = TrieDB<Address, OverlayDB>(const_cast<OverlayDB*>(&_s.m_db), _s.rootHash());
	for (auto i: trie)
		d.insert(i.first), dtr.insert(i.first);
	for (auto i: _s.m_cache)
		d.insert(i.first);

	for (auto i: d)
	{
		auto it = _s.m_cache.find(i);
		AddressState* cache = it != _s.m_cache.end() ? &it->second : nullptr;
		auto rlpString = trie.at(i);
		RLP r(dtr.count(i) ? rlpString : "");
		assert(cache || r);

		if (cache && !cache->isAlive())
			_out << "XXX  " << i << std::endl;
		else
		{
			string lead = (cache ? r ? " *   " : " +   " : "     ");
			if (cache && r && cache->nonce() == r[0].toInt<u256>() && cache->balance() == r[1].toInt<u256>())
				lead = " .   ";

			stringstream contout;

			if ((!cache || cache->codeBearing()) && (!r || r[3].toHash<h256>() != EmptySHA3))
			{
				std::map<u256, u256> mem;
				std::set<u256> back;
				std::set<u256> delta;
				std::set<u256> cached;
				if (r)
				{
					TrieDB<h256, OverlayDB> memdb(const_cast<OverlayDB*>(&_s.m_db), r[2].toHash<h256>());		// promise we won't alter the overlay! :)
					for (auto const& j: memdb)
						mem[j.first] = RLP(j.second).toInt<u256>(), back.insert(j.first);
				}
				if (cache)
					for (auto const& j: cache->storage())
					{
						if ((!mem.count(j.first) && j.second) || (mem.count(j.first) && mem.at(j.first) != j.second))
							mem[j.first] = j.second, delta.insert(j.first);
						else if (j.second)
							cached.insert(j.first);
					}
				if (delta.size())
					lead = (lead == " .   ") ? "*.*  " : "***  ";

				contout << " @:";
				if (delta.size())
					contout << "???";
				else
					contout << r[2].toHash<h256>();
				if (cache && cache->isFreshCode())
					contout << " $" << cache->code();
				else
					contout << " $" << (cache ? cache->codeHash() : r[3].toHash<h256>());

				for (auto const& j: mem)
					if (j.second)
						contout << std::endl << (delta.count(j.first) ? back.count(j.first) ? " *     " : " +     " : cached.count(j.first) ? " .     " : "       ") << std::hex << nouppercase << std::setw(64) << j.first << ": " << std::setw(0) << j.second ;
					else
						contout << std::endl << "XXX    " << std::hex << nouppercase << std::setw(64) << j.first << "";
			}
			else
				contout << " [SIMPLE]";
			_out << lead << i << ": " << std::dec << (cache ? cache->nonce() : r[0].toInt<u256>()) << " #:" << (cache ? cache->balance() : r[1].toInt<u256>()) << contout.str() << std::endl;
		}
	}
	return _out;
}

AccountChange AccountDiff::changeType() const
{
	bool bn = (balance || nonce);
	bool sc = (!storage.empty() || code);
	return exist ? exist.from() ? AccountChange::Deletion : AccountChange::Creation : (bn && sc) ? AccountChange::All : bn ? AccountChange::Intrinsic: sc ? AccountChange::CodeStorage : AccountChange::None;
}

char const* AccountDiff::lead() const
{
	bool bn = (balance || nonce);
	bool sc = (!storage.empty() || code);
	return exist ? exist.from() ? "XXX" : "+++" : (bn && sc) ? "***" : bn ? " * " : sc ? "* *" : "   ";
}

std::ostream& eth::operator<<(std::ostream& _out, AccountDiff const& _s)
{
	if (!_s.exist.to())
		return _out;

	if (_s.nonce)
	{
		_out << std::dec << "#" << _s.nonce.to() << " ";
		if (_s.nonce.from())
			_out << "(" << std::showpos << (((bigint)_s.nonce.to()) - ((bigint)_s.nonce.from())) << std::noshowpos << ") ";
	}
	if (_s.balance)
	{
		_out << std::dec << _s.balance.to() << " ";
		if (_s.balance.from())
			_out << "(" << std::showpos << (((bigint)_s.balance.to()) - ((bigint)_s.balance.from())) << std::noshowpos << ") ";
	}
	if (_s.code)
		_out << "$" << std::hex << nouppercase << _s.code.to() << " (" << _s.code.from() << ") ";
	for (pair<u256, Diff<u256>> const& i: _s.storage)
		if (!i.second.from())
			_out << endl << " +     " << (h256)i.first << ": " << std::hex << nouppercase << i.second.to();
		else if (!i.second.to())
			_out << endl << "XXX    " << (h256)i.first << " (" << std::hex << nouppercase << i.second.from() << ")";
		else
			_out << endl << " *     " << (h256)i.first << ": " << std::hex << nouppercase << i.second.to() << " (" << i.second.from() << ")";
	return _out;
}

std::ostream& eth::operator<<(std::ostream& _out, StateDiff const& _s)
{
	_out << _s.accounts.size() << " accounts changed:" << endl;
	for (auto const& i: _s.accounts)
		_out << i.second.lead() << "  " << i.first << ": " << i.second << endl;
	return _out;
}
