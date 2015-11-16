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
/** @file BlockChainHelper.cpp
 * @author Dimitry Khokhlov <dimitry@ethdev.com>
 * @date 2015
 */

#include <libdevcore/TransientDirectory.h>
#include <libethereum/Block.h>
#include <libethereum/BlockChain.h>
#include <libethereum/TransactionQueue.h>
#include <libethereum/GenesisInfo.h>
#include <libethashseal/GenesisInfo.h>
#include <test/BlockChainHelper.h>
#include <test/TestHelper.h>
using namespace std;
using namespace json_spirit;
using namespace dev;
using namespace dev::eth;

namespace dev
{
namespace test
{

TestTransaction::TestTransaction(mObject const& _o):
	m_jsonTransaction(_o)
{
	ImportTest::importTransaction(_o, m_transaction); //check that json structure is valid
}

TestBlock::TestBlock()
{
	m_sealEngine.reset(SealEngineRegistrar::create(ChainParams()));
}

TestBlock::TestBlock(mObject const& _blockObj, mObject const& _stateObj, RecalcBlockHeader _verify):
	TestBlock()
{
	m_tempDirState = std::unique_ptr<TransientDirectory>(new TransientDirectory());

	m_state = std::unique_ptr<State>(new State(0, OverlayDB(State::openDB(m_tempDirState.get()->path(), h256{}, WithExisting::Kill)), BaseState::Empty));
	ImportTest::importState(_stateObj, *m_state.get());
	m_state.get()->commit();
	m_accountMap = jsonToAccountMap(json_spirit::write_string(json_spirit::mValue(_stateObj), false));

	m_blockHeader = constructBlock(_blockObj, _stateObj.size() ? m_state.get()->rootHash() : h256{});
	recalcBlockHeaderBytes(_verify);
}

TestBlock::TestBlock(std::string const& _blockRLP):
	TestBlock()
{
	m_bytes = importByteArray(_blockRLP);

	RLP root(m_bytes);
	m_blockHeader = BlockHeader(m_bytes);
	// TODO: do we want to bother verifying stuff here?
	m_sealEngine->verify(IgnoreSeal, m_blockHeader);

	m_transactionQueue.clear();
	m_testTransactions.clear();
	for (auto const& tr: root[1])
	{
		Transaction tx(tr.data(), CheckTransaction::Everything);
		TestTransaction testTx(tx);
		m_transactionQueue.import(tx.rlp());
		m_testTransactions.push_back(testTx);
	}

	for (auto const& uRLP: root[2])
	{
		BlockHeader uBl(uRLP.data(), HeaderData);
		m_sealEngine->verify(IgnoreSeal, uBl);
		TestBlock uncle;
		//uncle goes without transactions and uncles but
		//it's hash could contain hashsum of transactions/uncles
		//thus it won't need verification
		uncle.setBlockHeader(uBl, RecalcBlockHeader::SkipVerify);
		m_uncles.push_back(uncle);
	}
}

TestBlock::TestBlock(TestBlock const& _original)
{
	populateFrom(_original);
}

TestBlock& TestBlock::operator=(TestBlock const& _original)
{
	populateFrom(_original);
	return *this;
}

void TestBlock::setState(State const& _state)
{
	copyStateFrom(_state);
}

void TestBlock::addTransaction(TestTransaction const& _tr)
{
	try
	{
		m_testTransactions.push_back(_tr);
		if (m_transactionQueue.import(_tr.getTransaction().rlp()) != ImportResult::Success)
			cnote << TestOutputHelper::testName() + "Test block failed importing transaction\n";
	}
	catch (Exception const& _e)
	{
		BOOST_ERROR(TestOutputHelper::testName() + "Failed transaction constructor with Exception: " << diagnostic_information(_e));
	}
	catch (exception const& _e)
	{
		cnote << _e.what();
	}
}

void TestBlock::addUncle(TestBlock const& _uncle)
{
	m_uncles.push_back(_uncle);
}

void TestBlock::setUncles(vector<TestBlock> const& _uncles)
{
	m_uncles.clear();
	m_uncles = _uncles;
}

void TestBlock::mine(TestBlockChain const& bc)
{
	TestBlock const& genesisBlock = bc.getTestGenesis();
	OverlayDB const& genesisDB = genesisBlock.getState().db();

	BlockChain const& blockchain = bc.getInterface();

	Block block = blockchain.genesisBlock(genesisDB);
	block.setAuthor(genesisBlock.getBeneficiary());

	//set some header data before mining from original blockheader
	BlockHeader& blockInfo = *const_cast<BlockHeader*>(&block.info());

	try
	{
		ZeroGasPricer gp;
		block.sync(blockchain);

		if (m_premineUpdate.count("parentHash") > 0)
			blockInfo.setParentHash(m_blockHeader.parentHash());
		if (m_premineUpdate.count("coinbase") > 0)
			blockInfo.setAuthor(m_blockHeader.author());

		if (m_premineUpdate.count("uncleHash") > 0 || m_premineUpdate.count("stateRoot") > 0 ||
			m_premineUpdate.count("transactionsTrie") > 0 || m_premineUpdate.count("receiptTrie") > 0)
			blockInfo.setRoots(m_premineUpdate.count("transactionsTrie") > 0 ? m_blockHeader.transactionsRoot() : blockInfo.transactionsRoot(),
							m_premineUpdate.count("receiptTrie") > 0 ? m_blockHeader.receiptsRoot() : blockInfo.receiptsRoot(),
							m_premineUpdate.count("uncleHash") > 0 ? m_blockHeader.sha3Uncles() : blockInfo.sha3Uncles(),
							m_premineUpdate.count("stateRoot") > 0 ? m_blockHeader.stateRoot() : blockInfo.stateRoot());

		if (m_premineUpdate.count("bloom") > 0)
			blockInfo.setLogBloom(m_blockHeader.logBloom());
		if (m_premineUpdate.count("difficulty") > 0)
			blockInfo.setDifficulty(m_blockHeader.difficulty());
		if (m_premineUpdate.count("number") > 0)
			blockInfo.setNumber(m_blockHeader.number());
		if (m_premineUpdate.count("gasLimit") > 0)
			blockInfo.setGasLimit(m_blockHeader.gasLimit());
		if (m_premineUpdate.count("gasUsed") > 0)
			blockInfo.setGasUsed(m_blockHeader.gasUsed());
		if (m_premineUpdate.count("timestamp") > 0)
			blockInfo.setTimestamp(m_blockHeader.timestamp());
		if (m_premineUpdate.count("extraData") > 0)
			blockInfo.setExtraData(m_blockHeader.extraData());

		block.sync(blockchain, m_transactionQueue, gp);

		//Get only valid transactions
		//Transactions const& trs = block.pending();
		//m_transactionQueue.clear();
		//for (auto const& tr : trs)
		//	m_transactionQueue.import(tr.rlp());

		dev::eth::mine(block, blockchain, m_sealEngine.get());
//		cdebug << "Block mined" << Ethash::boundary(block.info()).hex() << Ethash::nonce(block.info()) << block.info().hash(WithoutSeal).hex();
		m_sealEngine->verify(JustSeal, block.info());
	}
	catch (Exception const& _e)
	{
		cnote << TestOutputHelper::testName() + "block sync or mining did throw an exception: " << diagnostic_information(_e);
		return;
	}
	catch (std::exception const& _e)
	{
		cnote << TestOutputHelper::testName() + "block sync or mining did throw an exception: " << _e.what();
		return;
	}

	m_blockHeader = BlockHeader(block.blockData());		// NOTE no longer checked at this point in new API. looks like it was unimportant anyway
	copyStateFrom(block.state());

	//Update block hashes cause we would fill block with uncles and transactions that
	//actually might have been dropped because they are invalid
	recalcBlockHeaderBytes(RecalcBlockHeader::UpdateAndVerify);
}

void TestBlock::setBlockHeader(BlockHeader const& _header, RecalcBlockHeader _recalculate)
{
	m_blockHeader = _header;
	recalcBlockHeaderBytes(_recalculate);
}

///Test Block Private
BlockHeader TestBlock::constructBlock(mObject const& _o, h256 const& _stateRoot)
{
	BlockHeader ret;
	try
	{
		const bytes c_blockRLP = createBlockRLPFromFields(_o, _stateRoot);
		ret = BlockHeader(c_blockRLP, HeaderData);
//		cdebug << "Block constructed of hash" << ret.hash() << "(without:" << ret.hash(WithoutSeal) << ")";
	}
	catch (Exception const& _e)
	{
		cnote << TestOutputHelper::testName() + "block population did throw an exception: " << diagnostic_information(_e);
	}
	catch (std::exception const& _e)
	{
		BOOST_ERROR(TestOutputHelper::testName() + "Failed block population with Exception: " << _e.what());
	}
	catch(...)
	{
		BOOST_ERROR(TestOutputHelper::testName() + "block population did throw an unknown exception\n");
	}
	return ret;
}

bytes TestBlock::createBlockRLPFromFields(mObject const& _tObj, h256 const& _stateRoot)
{
	RLPStream rlpStream;
	rlpStream.appendList(_tObj.count("hash") > 0 ? (_tObj.size() - 1) : _tObj.size());

	if (_tObj.count("parentHash"))
		rlpStream << importByteArray(_tObj.at("parentHash").get_str());

	if (_tObj.count("uncleHash"))
		rlpStream << importByteArray(_tObj.at("uncleHash").get_str());

	if (_tObj.count("coinbase"))
		rlpStream << importByteArray(_tObj.at("coinbase").get_str());

	if (_stateRoot)
		rlpStream << _stateRoot;
	else if (_tObj.count("stateRoot"))
		rlpStream << importByteArray(_tObj.at("stateRoot").get_str());

	if (_tObj.count("transactionsTrie"))
		rlpStream << importByteArray(_tObj.at("transactionsTrie").get_str());

	if (_tObj.count("receiptTrie"))
		rlpStream << importByteArray(_tObj.at("receiptTrie").get_str());

	if (_tObj.count("bloom"))
		rlpStream << importByteArray(_tObj.at("bloom").get_str());

	if (_tObj.count("difficulty"))
		rlpStream << bigint(_tObj.at("difficulty").get_str());

	if (_tObj.count("number"))
		rlpStream << bigint(_tObj.at("number").get_str());

	if (_tObj.count("gasLimit"))
		rlpStream << bigint(_tObj.at("gasLimit").get_str());

	if (_tObj.count("gasUsed"))
		rlpStream << bigint(_tObj.at("gasUsed").get_str());

	if (_tObj.count("timestamp"))
		rlpStream << bigint(_tObj.at("timestamp").get_str());

	if (_tObj.count("extraData"))
		rlpStream << fromHex(_tObj.at("extraData").get_str());

	if (_tObj.count("mixHash"))
		rlpStream << importByteArray(_tObj.at("mixHash").get_str());

	if (_tObj.count("nonce"))
		rlpStream << importByteArray(_tObj.at("nonce").get_str());

	return rlpStream.out();
}

//Form bytestream of a block with [header transactions uncles]
void TestBlock::recalcBlockHeaderBytes(RecalcBlockHeader _recalculate)
{
	Transactions txList;
	for (auto const& txi: m_transactionQueue.topTransactions(std::numeric_limits<unsigned>::max()))
		txList.push_back(txi);
	RLPStream txStream;
	txStream.appendList(txList.size());
	for (unsigned i = 0; i < txList.size(); ++i)
	{
		RLPStream txrlp;
		txList[i].streamRLP(txrlp);
		txStream.appendRaw(txrlp.out());
	}

	RLPStream uncleStream;
	uncleStream.appendList(m_uncles.size());
	for (unsigned i = 0; i < m_uncles.size(); ++i)
	{
		RLPStream uncleRlp;
		m_uncles[i].getBlockHeader().streamRLP(uncleRlp);
		uncleStream.appendRaw(uncleRlp.out());
	}

	if (_recalculate == RecalcBlockHeader::Update || _recalculate == RecalcBlockHeader::UpdateAndVerify)
	{
		//update hashes correspong to block contents
		if (m_uncles.size())
			m_blockHeader.setSha3Uncles(sha3(uncleStream.out()));

		//if (txList.size())
		//	m_blockHeader.setRoots(sha3(txStream.out()), m_blockHeader.receiptsRoot(), m_blockHeader.sha3Uncles(), m_blockHeader.stateRoot());

		if (((BlockHeader)m_blockHeader).difficulty() == 0)
			BOOST_ERROR("Trying to mine a block with 0 difficulty!");

		dev::eth::mine(m_blockHeader, m_sealEngine.get());
		m_blockHeader.noteDirty();
	}

	RLPStream blHeaderStream;
	m_blockHeader.streamRLP(blHeaderStream, WithSeal);

	RLPStream ret(3);
	ret.appendRaw(blHeaderStream.out()); //block header
	ret.appendRaw(txStream.out());		 //transactions
	ret.appendRaw(uncleStream.out());	 //uncles

	if (_recalculate == RecalcBlockHeader::Verify || _recalculate == RecalcBlockHeader::UpdateAndVerify)
	{
		try
		{
			// TODO: CheckNothingNew -> CheckBlock.
			m_sealEngine->verify(CheckNothingNew, m_blockHeader, BlockHeader(), &ret.out());
		}
		catch (Exception const& _e)
		{
			BOOST_ERROR(TestOutputHelper::testName() + "BlockHeader Verification failed: " << diagnostic_information(_e));
		}
		catch(...)
		{
			BOOST_ERROR(TestOutputHelper::testName() + "BlockHeader Verification failed");
		}
	}
	m_bytes = ret.out();
}

void TestBlock::copyStateFrom(State const& _state)
{
	//WEIRD WAY TO COPY STATE AS COPY CONSTRUCTOR FOR STATE NOT IMPLEMENTED CORRECTLY (they would share the same DB)
	m_tempDirState.reset(new TransientDirectory());
	m_state.reset(new State(0, OverlayDB(State::openDB(m_tempDirState.get()->path(), h256{}, WithExisting::Kill)), BaseState::Empty));
	json_spirit::mObject obj = fillJsonWithState(_state);
	ImportTest::importState(obj, *m_state.get());
}

void TestBlock::clearState()
{
	m_state.reset(0);
	m_tempDirState.reset(0);
	for (size_t i = 0; i < m_uncles.size(); i++)
		m_uncles.at(i).clearState();
}

void TestBlock::setPremine(std::string const& _parameter)
{
	m_premineUpdate[_parameter] = true;
}

void TestBlock::populateFrom(TestBlock const& _original)
{
	try
	{
		copyStateFrom(_original.getState()); //copy state if it is defined in _original
	}
	catch (BlockStateUndefined const& _ex)
	{
		cnote << _ex.what() << "copying block with null state";
	}
	m_testTransactions = _original.getTestTransactions();
	m_transactionQueue.clear();
	TransactionQueue const& trQueue = _original.getTransactionQueue();
	for (auto const& txi: trQueue.topTransactions(std::numeric_limits<unsigned>::max()))
		m_transactionQueue.import(txi.rlp());

	m_uncles = _original.getUncles();
	m_blockHeader = _original.getBlockHeader();
	m_bytes = _original.getBytes();
	m_sealEngine = _original.m_sealEngine;
}

TestBlockChain::TestBlockChain(TestBlock const& _genesisBlock)
{
	reset(_genesisBlock);
}

void TestBlockChain::reset(TestBlock const& _genesisBlock)
{
	m_tempDirBlockchain.reset(new TransientDirectory);
	ChainParams p(/*genesisInfo(Network::Test), */_genesisBlock.getBytes(), _genesisBlock.accountMap());
	m_blockChain.reset(new BlockChain(p, m_tempDirBlockchain.get()->path(), WithExisting::Kill));
	if (!m_blockChain->isKnown(BlockHeader::headerHashFromBlock(_genesisBlock.getBytes())))
	{
		cdebug << "Not known:" << BlockHeader::headerHashFromBlock(_genesisBlock.getBytes()) << BlockHeader(p.genesisBlock()).hash();
		cdebug << "Genesis block not known!";
		throw 0;
	}
	m_lastBlock = m_genesisBlock = _genesisBlock;
}

void TestBlockChain::addBlock(TestBlock const& _block)
{
	while (true)
	{
		try
		{
			m_blockChain.get()->import(_block.getBytes(), m_genesisBlock.getState().db());
			break;
		}
		catch (FutureTime)
		{
			this_thread::sleep_for(chrono::milliseconds(100));
		}
	}

	//Imported and best
	if (_block.getBytes() == m_blockChain.get()->block())
	{
		m_lastBlock = _block;

		//overwrite state in case _block had no State defined (e.x. created from RLP)
		OverlayDB const& genesisDB = m_genesisBlock.getState().db();
		BlockChain const& blockchain = getInterface();
		Block block = (blockchain.genesisBlock(genesisDB));
		block.sync(blockchain);
		m_lastBlock.setState(block.state());
	}
}

vector<TestBlock> TestBlockChain::syncUncles(vector<TestBlock> const& uncles)
{	
	vector<TestBlock> validUncles;
	if (uncles.size() == 0)
		return validUncles;

	BlockQueue uncleBlockQueue;
	BlockChain& blockchain = *m_blockChain.get();
	uncleBlockQueue.setChain(blockchain);

	for (size_t i = 0; i < uncles.size(); i++)
	{
		try
		{
			uncleBlockQueue.import(&uncles.at(i).getBytes(), false);
			this_thread::sleep_for(chrono::seconds(1)); // wait until block is verified
			validUncles.push_back(uncles.at(i));
		}
		catch(...)
		{
			cnote << "error in importing uncle! This produces an invalid block (May be by purpose for testing).";
		}
	}

	blockchain.sync(uncleBlockQueue, m_genesisBlock.getState().db(), (unsigned)4);
	return validUncles;
}

TestTransaction TestTransaction::getDefaultTransaction()
{
	json_spirit::mObject txObj;
	txObj["data"] = "";
	txObj["gasLimit"] = "50000";
	txObj["gasPrice"] = "1";
	txObj["nonce"] = "0";
	txObj["secretKey"] = "45a915e4d060149eb4365960e6a7a45f334393093061116b197e3240065ff2d8";
	txObj["to"] = "095e7baea6a6c7c4c2dfeb977efac326af552d87";
	txObj["value"] = "100";

	return TestTransaction(txObj);
}

AccountMap TestBlockChain::getDefaultAccountMap()
{
	AccountMap ret;
	ret[Address("a94f5374fce5edbc8e2a8697c15331677e6ebf0b")] = Account(0, 10000000000);
	return ret;
}

TestBlock TestBlockChain::getDefaultGenesisBlock()
{
	json_spirit::mObject blockObj;
	blockObj["bloom"] = "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
	blockObj["coinbase"] = "0x8888f1f195afa192cfee860698584c030f4c9db1";
	blockObj["difficulty"] = "131072";
	blockObj["extraData"] = "0x42";
	blockObj["gasLimit"] = "3141592";
	blockObj["gasUsed"] = "0";
	blockObj["mixHash"] = "0x56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421";
	blockObj["nonce"] = "0x0102030405060708";
	blockObj["number"] = "0";
	blockObj["parentHash"] = "0x0000000000000000000000000000000000000000000000000000000000000000";
	blockObj["receiptTrie"] = "0x56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421";
	blockObj["stateRoot"] = "0xf99eb1626cfa6db435c0836235942d7ccaa935f1ae247d3f1c21e495685f903a";
	blockObj["timestamp"] = "0x54c98c81";
	blockObj["transactionsTrie"] = "0x56e81f171bcc55a6ff8345e692c0f86e5b48e01b996cadc001622fb5e363b421";
	blockObj["uncleHash"] = "0x1dcc4de8dec75d7aab85b567b6ccd41ad312451b948a7413f0a142fd40d49347";

	json_spirit::mObject accountObj;
	accountObj["balance"] = "10000000000";
	accountObj["nonce"] = "0";
	accountObj["code"] = "";
	accountObj["storage"] = json_spirit::mObject();

	json_spirit::mObject accountMapObj;
	accountMapObj["a94f5374fce5edbc8e2a8697c15331677e6ebf0b"] = accountObj;

	return TestBlock(blockObj, accountMapObj, RecalcBlockHeader::UpdateAndVerify);
}

}}
