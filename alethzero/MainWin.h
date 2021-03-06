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
/** @file MainWin.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#pragma once


#include <map>
#include <QtNetwork/QNetworkAccessManager>
#include <QtCore/QAbstractListModel>
#include <QtCore/QMutex>
#include <QtWidgets/QMainWindow>
#include <libethential/RLP.h>
#include <libethcore/CommonEth.h>
#include <libethereum/State.h>
#include <libqethereum/QEthereum.h>

namespace Ui {
class Main;
}

namespace eth {
class Client;
class State;
class MessageFilter;
}

class QQuickView;

struct WorldState
{
	uint64_t steps;
	eth::Address cur;
	eth::u256 curPC;
	eth::Instruction inst;
	eth::bigint newMemSize;
	eth::u256 gas;
	eth::h256 code;
	eth::h256 callData;
	eth::u256s stack;
	eth::bytes memory;
	eth::bigint gasCost;
	std::map<eth::u256, eth::u256> storage;
	std::vector<WorldState const*> levels;
};

class Main : public QMainWindow
{
	Q_OBJECT
	
public:
	explicit Main(QWidget *parent = 0);
	~Main();

	eth::Client* client() { return m_client.get(); }

	QList<eth::KeyPair> const& owned() const { return m_myKeys; }
	
public slots:
	void load(QString _file);
	void note(QString _entry);
	void debug(QString _entry);
	void warn(QString _entry);

	void onKeysChanged();

private slots:
	void eval(QString const& _js);

	void on_connect_triggered();
	void on_mine_triggered();
	void on_send_clicked();
	void on_create_triggered();
	void on_net_triggered();
	void on_verbosity_valueChanged();
	void on_ourAccounts_doubleClicked();
	void ourAccountsRowsMoved();
	void on_accounts_doubleClicked();
	void on_destination_currentTextChanged();
	void on_data_textChanged();
	void on_idealPeers_valueChanged();
	void on_value_valueChanged() { updateFee(); }
	void on_gas_valueChanged() { updateFee(); }
	void on_valueUnits_currentIndexChanged() { updateFee(); }
	void on_gasPriceUnits_currentIndexChanged() { updateFee(); }
	void on_gasPrice_valueChanged() { updateFee(); }
	void on_log_doubleClicked();
	void on_blocks_currentItemChanged();
	void on_contracts_doubleClicked();
	void on_contracts_currentItemChanged();
	void on_transactionQueue_currentItemChanged();
	void on_about_triggered();
	void on_paranoia_triggered();
	void on_nameReg_textChanged();
	void on_preview_triggered();
	void on_quit_triggered() { close(); }
	void on_urlEdit_returnPressed();
	void on_debugStep_triggered();
	void on_debugStepBack_triggered();
	void on_debug_clicked();
	void on_debugTimeline_valueChanged();
	void on_jsInput_returnPressed();
	void on_killBlockchain_triggered();
	void on_importKey_triggered();
	void on_exportKey_triggered();
	void on_inject_triggered();
	void on_showAll_triggered() { refreshBlockChain(); }
	void on_showAllAccounts_triggered() { refreshAccounts(); }
	void on_loadJS_triggered();
	void on_blockChainFilter_textChanged();
	void on_forceMining_triggered();
	void on_dumpTrace_triggered();
	void on_dumpTraceStorage_triggered();
	void on_dumpTracePretty_triggered();
	void on_debugStepInto_triggered();
	void on_debugStepOut_triggered();
	void on_debugStepBackOut_triggered();
	void on_debugStepBackInto_triggered();
	void on_callStack_currentItemChanged();
	void on_debugCurrent_triggered();
	void on_debugDumpState_triggered(int _add = 1);
	void on_debugDumpStatePre_triggered();
	void on_refresh_triggered();
    void on_usePrivate_triggered();
	void on_enableOptimizer_triggered();
	void on_turboMining_triggered();

signals:
	void poll();

private:
	QString pretty(eth::Address _a) const;
	QString prettyU256(eth::u256 _n) const;

	QString lookup(QString const& _n) const;

	void populateDebugger(eth::bytesConstRef r);
	void initDebugger();
	void updateDebugger();
	void debugFinished();
	QString render(eth::Address _a) const;
	eth::Address fromString(QString const& _a) const;
	std::string renderDiff(eth::StateDiff const& _d) const;

	void alterDebugStateGroup(bool _enable) const;

	void updateFee();
	void readSettings(bool _skipGeometry = false);
	void writeSettings();

	bool isCreation() const;
	eth::u256 fee() const;
	eth::u256 total() const;
	eth::u256 value() const;
	eth::u256 gasPrice() const;

	unsigned installWatch(eth::MessageFilter const& _tf, std::function<void()> const& _f);
	unsigned installWatch(eth::h256 _tf, std::function<void()> const& _f);

	void onNewPending();
	void onNewBlock();
	void onNameRegChange();
	void onCurrenciesChange();
	void onBalancesChange();

	void installWatches();
	void installCurrenciesWatch();
	void installNameRegWatch();
	void installBalancesWatch();

	virtual void timerEvent(QTimerEvent*);

	void refreshNetwork();
	void refreshMining();

	void refreshAll();
	void refreshPending();
	void refreshAccounts();
	void refreshDestination();
	void refreshBlockChain();
	void refreshBlockCount();
	void refreshBalances();

	std::unique_ptr<Ui::Main> ui;

	std::unique_ptr<eth::Client> m_client;
	std::map<unsigned, std::function<void()>> m_handlers;
	unsigned m_nameRegFilter = (unsigned)-1;
	unsigned m_currenciesFilter = (unsigned)-1;
	unsigned m_balancesFilter = (unsigned)-1;

	QByteArray m_peers;
	QStringList m_servers;
	QList<eth::KeyPair> m_myKeys;
	QString m_privateChain;
	bool m_keysChanged = false;
	eth::bytes m_data;
	eth::Address m_nameReg;

	unsigned m_backupGas;

	eth::State m_executiveState;
	std::unique_ptr<eth::Executive> m_currentExecution;
	eth::h256 m_lastCode;
	eth::h256 m_lastData;
	std::vector<WorldState const*> m_lastLevels;

	QMap<unsigned, unsigned> m_pcWarp;
	QList<WorldState> m_history;
	std::map<eth::u256, eth::bytes> m_codes;	// and pcWarps
	bool m_enableOptimizer = true;

	QNetworkAccessManager m_webCtrl;

	QList<QPair<QString, QString>> m_consoleHistory;
	QMutex m_logLock;
	QString m_logHistory;
	bool m_logChanged = true;

	QEthereum* m_ethereum = nullptr;
};
