// Copyright (c) 2016-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/test/wallet_test_fixture.h>

#include <rpc/server.h>
#include <wallet/db.h>

WalletTestingSetup::WalletTestingSetup(const std::string& chainName):
    TestingSetup(chainName)
{
    bitdb.MakeMock();

    bool fFirstRun;
    g_address_type = OUTPUT_TYPE_DEFAULT;
    g_change_type = OUTPUT_TYPE_DEFAULT;
    std::unique_ptr<CWalletDBWrapper> dbw(new CWalletDBWrapper(&bitdb, "wallet_test.dat"));
    pwalletMain = MakeUnique<CWallet>(m_chain.get(), std::move(dbw));
    pwalletMain->LoadWallet(fFirstRun);
    pwalletMain->m_handler = m_chain->handleNotifications(*pwalletMain);

    RegisterWalletRPCCommands(tableRPC);
}

WalletTestingSetup::~WalletTestingSetup()
{
    pwalletMain->m_handler->disconnect();

    bitdb.Flush(true);
    bitdb.Reset();
}
