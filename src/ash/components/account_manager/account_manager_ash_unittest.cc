// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/components/account_manager/account_manager_ash.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ash/components/account_manager/access_token_fetcher.h"
#include "ash/components/account_manager/account_manager.h"
#include "ash/components/account_manager/account_manager_ui.h"
#include "base/bind.h"
#include "base/callback_forward.h"
#include "base/callback_helpers.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "chromeos/crosapi/mojom/account_manager.mojom-test-utils.h"
#include "chromeos/crosapi/mojom/account_manager.mojom.h"
#include "components/account_manager_core/account.h"
#include "components/account_manager_core/account_manager_util.h"
#include "components/prefs/testing_pref_service.h"
#include "google_apis/gaia/gaia_urls.h"
#include "google_apis/gaia/google_service_auth_error.h"
#include "google_apis/gaia/oauth2_access_token_fetcher.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace crosapi {

namespace {

const char kFakeGaiaId[] = "fake-gaia-id";
const char kFakeEmail[] = "fake_email@example.com";
const char kFakeToken[] = "fake-token";
const char kFakeOAuthConsumerName[] = "fake-oauth-consumer-name";
constexpr char kFakeAccessToken[] = "fake-access-token";
// Same access token value as above in `kFakeAccessToken`.
constexpr char kAccessTokenResponse[] = R"(
    {
      "access_token": "fake-access-token",
      "expires_in": 3600,
      "token_type": "Bearer",
      "id_token": "id_token"
    })";
const account_manager::Account kFakeAccount = account_manager::Account{
    account_manager::AccountKey{kFakeGaiaId,
                                account_manager::AccountType::kGaia},
    kFakeEmail};

}  // namespace

class TestAccountManagerObserver
    : public mojom::AccountManagerObserverInterceptorForTesting {
 public:
  TestAccountManagerObserver() : receiver_(this) {}

  TestAccountManagerObserver(const TestAccountManagerObserver&) = delete;
  TestAccountManagerObserver& operator=(const TestAccountManagerObserver&) =
      delete;
  ~TestAccountManagerObserver() override = default;

  void Observe(
      mojom::AccountManagerAsyncWaiter* const account_manager_async_waiter) {
    mojo::PendingReceiver<mojom::AccountManagerObserver> receiver;
    account_manager_async_waiter->AddObserver(&receiver);
    receiver_.Bind(std::move(receiver));
  }

  int GetNumOnTokenUpsertedCalls() const { return num_token_upserted_calls_; }

  account_manager::Account GetLastUpsertedAccount() const {
    return last_upserted_account_;
  }

  int GetNumOnAccountRemovedCalls() const { return num_account_removed_calls_; }

  account_manager::Account GetLastRemovedAccount() const {
    return last_removed_account_;
  }

 private:
  // mojom::AccountManagerObserverInterceptorForTesting override:
  AccountManagerObserver* GetForwardingInterface() override { return this; }

  // mojom::AccountManagerObserverInterceptorForTesting override:
  void OnTokenUpserted(mojom::AccountPtr account) override {
    ++num_token_upserted_calls_;
    last_upserted_account_ = account_manager::FromMojoAccount(account).value();
  }

  // mojom::AccountManagerObserverInterceptorForTesting override:
  void OnAccountRemoved(mojom::AccountPtr account) override {
    ++num_account_removed_calls_;
    last_removed_account_ = account_manager::FromMojoAccount(account).value();
  }

  int num_token_upserted_calls_ = 0;
  int num_account_removed_calls_ = 0;
  account_manager::Account last_upserted_account_;
  account_manager::Account last_removed_account_;
  mojo::Receiver<mojom::AccountManagerObserver> receiver_;
};

class FakeAccountManagerUI : public ash::AccountManagerUI {
 public:
  FakeAccountManagerUI() = default;
  FakeAccountManagerUI(const FakeAccountManagerUI&) = delete;
  FakeAccountManagerUI& operator=(const FakeAccountManagerUI&) = delete;
  ~FakeAccountManagerUI() override = default;

  void SetIsDialogShown(bool is_dialog_shown) {
    is_dialog_shown_ = is_dialog_shown;
  }

  void CloseDialog() {
    if (!close_dialog_closure_.is_null()) {
      std::move(close_dialog_closure_).Run();
    }
    is_dialog_shown_ = false;
  }

  int show_account_addition_dialog_calls() const {
    return show_account_addition_dialog_calls_;
  }

  int show_account_reauthentication_dialog_calls() const {
    return show_account_reauthentication_dialog_calls_;
  }

  int show_manage_accounts_settings_calls() const {
    return show_manage_accounts_settings_calls_;
  }

 private:
  // AccountManagerUI overrides:
  void ShowAddAccountDialog(base::OnceClosure close_dialog_closure) override {
    close_dialog_closure_ = std::move(close_dialog_closure);
    show_account_addition_dialog_calls_++;
    is_dialog_shown_ = true;
  }

  void ShowReauthAccountDialog(
      const std::string& email,
      base::OnceClosure close_dialog_closure) override {
    close_dialog_closure_ = std::move(close_dialog_closure);
    show_account_reauthentication_dialog_calls_++;
    is_dialog_shown_ = true;
  }

  bool IsDialogShown() override { return is_dialog_shown_; }

  void ShowManageAccountsSettings() override {
    show_manage_accounts_settings_calls_++;
  }

  base::OnceClosure close_dialog_closure_;
  bool is_dialog_shown_ = false;
  int show_account_addition_dialog_calls_ = 0;
  int show_account_reauthentication_dialog_calls_ = 0;
  int show_manage_accounts_settings_calls_ = 0;
};

// A test spy for intercepting AccountManager calls.
class AccountManagerSpy : public ash::AccountManager {
 public:
  AccountManagerSpy() = default;
  AccountManagerSpy(const AccountManagerSpy&) = delete;
  AccountManagerSpy& operator=(const AccountManagerSpy&) = delete;
  ~AccountManagerSpy() override = default;

  // ash::AccountManager override:
  std::unique_ptr<OAuth2AccessTokenFetcher> CreateAccessTokenFetcher(
      const ::account_manager::AccountKey& account_key,
      OAuth2AccessTokenConsumer* consumer) override {
    num_access_token_fetches_++;
    last_access_token_account_key_ = account_key;

    return ash::AccountManager::CreateAccessTokenFetcher(account_key, consumer);
  }

  int num_access_token_fetches() const { return num_access_token_fetches_; }

  account_manager::AccountKey last_access_token_account_key() const {
    return last_access_token_account_key_;
  }

 private:
  // Mutated by const CreateAccessTokenFetcher.
  mutable int num_access_token_fetches_ = 0;
  mutable account_manager::AccountKey last_access_token_account_key_;
};

class AccountManagerAshTest : public ::testing::Test {
 public:
  AccountManagerAshTest() = default;
  AccountManagerAshTest(const AccountManagerAshTest&) = delete;
  AccountManagerAshTest& operator=(const AccountManagerAshTest&) = delete;
  ~AccountManagerAshTest() override = default;

 protected:
  void SetUp() override {
    account_manager_ash_ =
        std::make_unique<AccountManagerAsh>(&account_manager_);
    account_manager_ash_->SetAccountManagerUI(
        std::make_unique<FakeAccountManagerUI>());
    account_manager_ash_->BindReceiver(remote_.BindNewPipeAndPassReceiver());
    account_manager_async_waiter_ =
        std::make_unique<mojom::AccountManagerAsyncWaiter>(
            account_manager_ash_.get());
  }

  void RunAllPendingTasks() { task_environment_.RunUntilIdle(); }

  void FlushMojoForTesting() { account_manager_ash_->FlushMojoForTesting(); }

  // Returns |true| if initialization was successful.
  bool InitializeAccountManager() {
    base::RunLoop run_loop;
    account_manager_.InitializeInEphemeralMode(
        test_url_loader_factory_.GetSafeWeakWrapper(), run_loop.QuitClosure());
    account_manager_.SetPrefService(&pref_service_);
    account_manager_.RegisterPrefs(pref_service_.registry());
    run_loop.Run();
    return account_manager_.IsInitialized();
  }

  FakeAccountManagerUI* GetFakeAccountManagerUI() {
    return static_cast<FakeAccountManagerUI*>(
        account_manager_ash_->account_manager_ui_.get());
  }

  mojom::AccountAdditionResultPtr ShowAddAccountDialog(
      base::OnceClosure quit_closure) {
    auto add_account_result = mojom::AccountAdditionResult::New();
    account_manager_ash_->ShowAddAccountDialog(base::BindOnce(
        [](base::OnceClosure quit_closure,
           mojom::AccountAdditionResultPtr* add_account_result,
           mojom::AccountAdditionResultPtr result) {
          (*add_account_result)->status = result->status;
          (*add_account_result)->account = std::move(result->account);
          std::move(quit_closure).Run();
        },
        std::move(quit_closure), &add_account_result));
    return add_account_result;
  }

  void ShowReauthAccountDialog(const std::string& email,
                               base::OnceClosure close_dialog_closure) {
    account_manager_ash_->ShowReauthAccountDialog(
        email, std::move(close_dialog_closure));
  }

  void CallAccountAdditionFinished(
      const account_manager::AccountAdditionResult& result) {
    account_manager_ash_->OnAccountAdditionFinished(result);
    GetFakeAccountManagerUI()->CloseDialog();
  }

  void ShowManageAccountsSettings() {
    account_manager_ash_->ShowManageAccountsSettings();
  }

  mojom::AccessTokenResultPtr FetchAccessToken(
      const account_manager::AccountKey& account_key) {
    return FetchAccessToken(account_key, /*scopes=*/{});
  }

  mojom::AccessTokenResultPtr FetchAccessToken(
      const account_manager::AccountKey& account_key,
      const std::vector<std::string>& scopes) {
    mojo::PendingRemote<mojom::AccessTokenFetcher> pending_remote;
    account_manager_async_waiter()->CreateAccessTokenFetcher(
        account_manager::ToMojoAccountKey(account_key), kFakeOAuthConsumerName,
        &pending_remote);
    mojo::Remote<mojom::AccessTokenFetcher> remote(std::move(pending_remote));

    base::RunLoop run_loop;
    mojom::AccessTokenResultPtr result;
    remote->Start(
        scopes,
        base::BindLambdaForTesting(
            [&run_loop, &result](mojom::AccessTokenResultPtr returned_result) {
              result = std::move(returned_result);
              run_loop.Quit();
            }));
    run_loop.Run();

    return result;
  }

  void AddFakeAccessTokenResponse() {
    GURL url(GaiaUrls::GetInstance()->oauth2_token_url());
    test_url_loader_factory_.AddResponse(url.spec(), kAccessTokenResponse,
                                         net::HTTP_OK);
  }

  int GetNumObservers() const {
    return account_manager_ash_->observers_.size();
  }

  int GetNumPendingAccessTokenRequests() const {
    return account_manager_ash_->GetNumPendingAccessTokenRequests();
  }

  mojom::AccountManagerAsyncWaiter* account_manager_async_waiter() {
    return account_manager_async_waiter_.get();
  }

  AccountManagerSpy* account_manager() { return &account_manager_; }

 private:
  base::test::SingleThreadTaskEnvironment task_environment_;

  network::TestURLLoaderFactory test_url_loader_factory_;
  TestingPrefServiceSimple pref_service_;
  AccountManagerSpy account_manager_;
  mojo::Remote<mojom::AccountManager> remote_;
  std::unique_ptr<AccountManagerAsh> account_manager_ash_;
  std::unique_ptr<mojom::AccountManagerAsyncWaiter>
      account_manager_async_waiter_;
};

TEST_F(AccountManagerAshTest,
       IsInitializedReturnsFalseForUninitializedAccountManager) {
  bool is_initialized = true;
  account_manager_async_waiter()->IsInitialized(&is_initialized);
  EXPECT_FALSE(is_initialized);
}

TEST_F(AccountManagerAshTest,
       IsInitializedReturnsTrueForInitializedAccountManager) {
  bool is_initialized = true;
  account_manager_async_waiter()->IsInitialized(&is_initialized);
  EXPECT_FALSE(is_initialized);
  ASSERT_TRUE(InitializeAccountManager());
  account_manager_async_waiter()->IsInitialized(&is_initialized);
  EXPECT_TRUE(is_initialized);
}

// Test that lacros remotes do not leak.
TEST_F(AccountManagerAshTest,
       LacrosRemotesAreAutomaticallyRemovedOnConnectionClose) {
  EXPECT_EQ(0, GetNumObservers());
  {
    mojo::PendingReceiver<mojom::AccountManagerObserver> receiver;
    account_manager_async_waiter()->AddObserver(&receiver);
    EXPECT_EQ(1, GetNumObservers());
  }
  // Wait for the disconnect handler to be called.
  RunAllPendingTasks();
  EXPECT_EQ(0, GetNumObservers());
}

TEST_F(AccountManagerAshTest, LacrosObserversAreNotifiedOnAccountUpdates) {
  const account_manager::AccountKey kTestAccountKey{
      kFakeGaiaId, account_manager::AccountType::kGaia};
  ASSERT_TRUE(InitializeAccountManager());
  TestAccountManagerObserver observer;
  observer.Observe(account_manager_async_waiter());
  ASSERT_EQ(1, GetNumObservers());

  EXPECT_EQ(0, observer.GetNumOnTokenUpsertedCalls());
  account_manager()->UpsertAccount(kTestAccountKey, kFakeEmail, kFakeToken);
  FlushMojoForTesting();
  EXPECT_EQ(1, observer.GetNumOnTokenUpsertedCalls());
  EXPECT_EQ(kTestAccountKey, observer.GetLastUpsertedAccount().key);
  EXPECT_EQ(kFakeEmail, observer.GetLastUpsertedAccount().raw_email);
}

TEST_F(AccountManagerAshTest, LacrosObserversAreNotifiedOnAccountRemovals) {
  const account_manager::AccountKey kTestAccountKey{
      kFakeGaiaId, account_manager::AccountType::kGaia};
  ASSERT_TRUE(InitializeAccountManager());
  TestAccountManagerObserver observer;
  observer.Observe(account_manager_async_waiter());
  ASSERT_EQ(1, GetNumObservers());
  account_manager()->UpsertAccount(kTestAccountKey, kFakeEmail, kFakeToken);
  FlushMojoForTesting();

  EXPECT_EQ(0, observer.GetNumOnAccountRemovedCalls());
  account_manager()->RemoveAccount(kTestAccountKey);
  FlushMojoForTesting();
  EXPECT_EQ(1, observer.GetNumOnAccountRemovedCalls());
  EXPECT_EQ(kTestAccountKey, observer.GetLastRemovedAccount().key);
  EXPECT_EQ(kFakeEmail, observer.GetLastRemovedAccount().raw_email);
}

TEST_F(AccountManagerAshTest, GetAccounts) {
  ASSERT_TRUE(InitializeAccountManager());
  {
    std::vector<mojom::AccountPtr> accounts;
    account_manager_async_waiter()->GetAccounts(&accounts);
    EXPECT_TRUE(accounts.empty());
  }

  const account_manager::AccountKey kTestAccountKey{
      kFakeGaiaId, account_manager::AccountType::kGaia};
  account_manager()->UpsertAccount(kTestAccountKey, kFakeEmail, kFakeToken);
  std::vector<mojom::AccountPtr> accounts;
  account_manager_async_waiter()->GetAccounts(&accounts);
  EXPECT_EQ(1UL, accounts.size());
  EXPECT_EQ(kFakeEmail, accounts[0]->raw_email);
  EXPECT_EQ(kFakeGaiaId, accounts[0]->key->id);
  EXPECT_EQ(mojom::AccountType::kGaia, accounts[0]->key->account_type);
}

TEST_F(AccountManagerAshTest,
       ShowAddAccountDialogReturnsInProgressIfDialogIsOpen) {
  EXPECT_EQ(0, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(true);
  mojom::AccountAdditionResultPtr account_addition_result;
  account_manager_async_waiter()->ShowAddAccountDialog(
      &account_addition_result);

  // Check status.
  EXPECT_EQ(mojom::AccountAdditionResult::Status::kAlreadyInProgress,
            account_addition_result->status);
  // Check that dialog was not called.
  EXPECT_EQ(0, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
}

TEST_F(AccountManagerAshTest,
       ShowAddAccountDialogReturnsCancelledAfterDialogIsClosed) {
  EXPECT_EQ(0, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(false);

  base::RunLoop run_loop;
  mojom::AccountAdditionResultPtr account_addition_result =
      ShowAddAccountDialog(run_loop.QuitClosure());
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();
  run_loop.Run();

  // Check status.
  EXPECT_EQ(mojom::AccountAdditionResult::Status::kCancelledByUser,
            account_addition_result->status);
  // Check that dialog was called once.
  EXPECT_EQ(1, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
}

TEST_F(AccountManagerAshTest,
       ShowAddAccountDialogReturnsSuccessAfterAccountIsAdded) {
  EXPECT_EQ(0, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(false);

  base::RunLoop run_loop;
  mojom::AccountAdditionResultPtr account_addition_result =
      ShowAddAccountDialog(run_loop.QuitClosure());
  // Simulate account addition.
  CallAccountAdditionFinished(account_manager::AccountAdditionResult(
      account_manager::AccountAdditionResult::Status::kSuccess, kFakeAccount));
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();
  run_loop.Run();

  // Check status.
  EXPECT_EQ(mojom::AccountAdditionResult::Status::kSuccess,
            account_addition_result->status);
  // Check account.
  base::Optional<account_manager::Account> account =
      account_manager::FromMojoAccount(account_addition_result->account);
  EXPECT_TRUE(account.has_value());
  EXPECT_EQ(kFakeAccount.key, account.value().key);
  EXPECT_EQ(kFakeAccount.raw_email, account.value().raw_email);
  // Check that dialog was called once.
  EXPECT_EQ(1, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
}

TEST_F(AccountManagerAshTest, ShowAddAccountDialogCanHandleMultipleCalls) {
  EXPECT_EQ(0, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(false);

  base::RunLoop run_loop;
  mojom::AccountAdditionResultPtr account_addition_result =
      ShowAddAccountDialog(run_loop.QuitClosure());

  base::RunLoop run_loop_2;
  mojom::AccountAdditionResultPtr account_addition_result_2 =
      ShowAddAccountDialog(run_loop_2.QuitClosure());
  run_loop_2.Run();
  // The second call gets 'kAlreadyInProgress' reply.
  EXPECT_EQ(mojom::AccountAdditionResult::Status::kAlreadyInProgress,
            account_addition_result_2->status);

  // Simulate account addition.
  CallAccountAdditionFinished(account_manager::AccountAdditionResult(
      account_manager::AccountAdditionResult::Status::kSuccess, kFakeAccount));
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();
  run_loop.Run();

  EXPECT_EQ(mojom::AccountAdditionResult::Status::kSuccess,
            account_addition_result->status);
  // Check account.
  base::Optional<account_manager::Account> account =
      account_manager::FromMojoAccount(account_addition_result->account);
  EXPECT_TRUE(account.has_value());
  EXPECT_EQ(kFakeAccount.key, account.value().key);
  EXPECT_EQ(kFakeAccount.raw_email, account.value().raw_email);
  // Check that dialog was called once.
  EXPECT_EQ(1, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
}

TEST_F(AccountManagerAshTest,
       ShowAddAccountDialogCanHandleMultipleSequentialCalls) {
  EXPECT_EQ(0, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(false);

  base::RunLoop run_loop;
  mojom::AccountAdditionResultPtr account_addition_result =
      ShowAddAccountDialog(run_loop.QuitClosure());
  // Simulate account addition.
  CallAccountAdditionFinished(account_manager::AccountAdditionResult(
      account_manager::AccountAdditionResult::Status::kSuccess, kFakeAccount));
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();
  run_loop.Run();
  EXPECT_EQ(mojom::AccountAdditionResult::Status::kSuccess,
            account_addition_result->status);
  // Check account.
  base::Optional<account_manager::Account> account =
      account_manager::FromMojoAccount(account_addition_result->account);
  EXPECT_TRUE(account.has_value());
  EXPECT_EQ(kFakeAccount.key, account.value().key);
  EXPECT_EQ(kFakeAccount.raw_email, account.value().raw_email);

  base::RunLoop run_loop_2;
  mojom::AccountAdditionResultPtr account_addition_result_2 =
      ShowAddAccountDialog(run_loop_2.QuitClosure());
  // Simulate account addition.
  CallAccountAdditionFinished(account_manager::AccountAdditionResult(
      account_manager::AccountAdditionResult::Status::kSuccess, kFakeAccount));
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();
  run_loop_2.Run();
  EXPECT_EQ(mojom::AccountAdditionResult::Status::kSuccess,
            account_addition_result_2->status);
  // Check account.
  base::Optional<account_manager::Account> account_2 =
      account_manager::FromMojoAccount(account_addition_result_2->account);
  EXPECT_TRUE(account_2.has_value());
  EXPECT_EQ(kFakeAccount.key, account_2.value().key);
  EXPECT_EQ(kFakeAccount.raw_email, account_2.value().raw_email);

  // Check that dialog was called 2 times.
  EXPECT_EQ(2, GetFakeAccountManagerUI()->show_account_addition_dialog_calls());
}

TEST_F(AccountManagerAshTest,
       ShowReauthAccountDialogDoesntCallTheDialogIfItsAlreadyShown) {
  EXPECT_EQ(
      0,
      GetFakeAccountManagerUI()->show_account_reauthentication_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(true);
  base::RunLoop run_loop;
  // Simulate account reauthentication.
  ShowReauthAccountDialog(kFakeEmail, run_loop.QuitClosure());
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();

  // Check that dialog was not called.
  EXPECT_EQ(
      0,
      GetFakeAccountManagerUI()->show_account_reauthentication_dialog_calls());
}

TEST_F(AccountManagerAshTest, ShowReauthAccountDialogOpensTheDialog) {
  EXPECT_EQ(
      0,
      GetFakeAccountManagerUI()->show_account_reauthentication_dialog_calls());
  GetFakeAccountManagerUI()->SetIsDialogShown(false);
  base::RunLoop run_loop;
  // Simulate account reauthentication.
  ShowReauthAccountDialog(kFakeEmail, run_loop.QuitClosure());
  // Simulate closing the dialog.
  GetFakeAccountManagerUI()->CloseDialog();

  // Check that dialog was called once.
  EXPECT_EQ(
      1,
      GetFakeAccountManagerUI()->show_account_reauthentication_dialog_calls());
}

TEST_F(AccountManagerAshTest, ShowManageAccountSettingsTest) {
  EXPECT_EQ(0,
            GetFakeAccountManagerUI()->show_manage_accounts_settings_calls());
  ShowManageAccountsSettings();
  EXPECT_EQ(1,
            GetFakeAccountManagerUI()->show_manage_accounts_settings_calls());
}

TEST_F(AccountManagerAshTest,
       FetchingAccessTokenResultsInErrorForInvalidAccountKey) {
  ASSERT_TRUE(InitializeAccountManager());
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
  account_manager::AccountKey account_key{std::string(),
                                          account_manager::AccountType::kGaia};
  mojom::AccessTokenResultPtr result = FetchAccessToken(account_key);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(mojom::GoogleServiceAuthError::State::kUserNotSignedUp,
            result->get_error()->state);

  // Check that requests are not leaking.
  RunAllPendingTasks();
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
}

TEST_F(AccountManagerAshTest,
       FetchingAccessTokenResultsInErrorForActiveDirectoryAccounts) {
  ASSERT_TRUE(InitializeAccountManager());
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
  account_manager::AccountKey account_key{
      kFakeGaiaId, account_manager::AccountType::kActiveDirectory};
  mojom::AccessTokenResultPtr result = FetchAccessToken(account_key);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(mojom::GoogleServiceAuthError::State::kUserNotSignedUp,
            result->get_error()->state);

  // Check that requests are not leaking.
  RunAllPendingTasks();
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
}

TEST_F(AccountManagerAshTest,
       FetchingAccessTokenResultsInErrorForUnknownAccountKey) {
  ASSERT_TRUE(InitializeAccountManager());
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
  account_manager::AccountKey account_key{kFakeGaiaId,
                                          account_manager::AccountType::kGaia};
  mojom::AccessTokenResultPtr result = FetchAccessToken(account_key);

  ASSERT_TRUE(result->is_error());
  EXPECT_EQ(mojom::GoogleServiceAuthError::State::kUserNotSignedUp,
            result->get_error()->state);

  // Check that requests are not leaking.
  RunAllPendingTasks();
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
}

TEST_F(AccountManagerAshTest, FetchAccessTokenRequestsCanBeCancelled) {
  // Setup.
  ASSERT_TRUE(InitializeAccountManager());
  account_manager::AccountKey account_key{kFakeGaiaId,
                                          account_manager::AccountType::kGaia};
  account_manager()->UpsertAccount(account_key, kFakeEmail, kFakeToken);
  mojo::PendingRemote<mojom::AccessTokenFetcher> pending_remote;
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
  account_manager_async_waiter()->CreateAccessTokenFetcher(
      account_manager::ToMojoAccountKey(account_key), kFakeOAuthConsumerName,
      &pending_remote);
  mojo::Remote<mojom::AccessTokenFetcher> remote(std::move(pending_remote));
  mojom::AccessTokenResultPtr result;
  EXPECT_TRUE(result.is_null());
  // Make a request to fetch access token. Since we haven't setup our test URL
  // loader factory via `AddFakeAccessTokenResponse`, this request will never be
  // completed.
  remote->Start(/*scopes=*/{},
                base::BindLambdaForTesting(
                    [&result](mojom::AccessTokenResultPtr returned_result) {
                      result = std::move(returned_result);
                    }));
  EXPECT_EQ(1, GetNumPendingAccessTokenRequests());

  // Test.
  // This should cancel the pending request.
  remote.reset();
  RunAllPendingTasks();
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
  // Verify that result is still unset - i.e. the pending request was cancelled,
  // and didn't complete normally.
  EXPECT_TRUE(result.is_null());
}

TEST_F(AccountManagerAshTest, FetchAccessToken) {
  constexpr char kFakeScope[] = "fake-scope";
  ASSERT_TRUE(InitializeAccountManager());
  account_manager::AccountKey account_key{kFakeGaiaId,
                                          account_manager::AccountType::kGaia};
  account_manager()->UpsertAccount(account_key, kFakeEmail, kFakeToken);
  AddFakeAccessTokenResponse();
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
  mojom::AccessTokenResultPtr result =
      FetchAccessToken(account_key, {kFakeScope});

  ASSERT_TRUE(result->is_access_token_info());
  EXPECT_EQ(kFakeAccessToken, result->get_access_token_info()->access_token);
  EXPECT_EQ(1, account_manager()->num_access_token_fetches());
  EXPECT_EQ(account_key, account_manager()->last_access_token_account_key());

  // Check that requests are not leaking.
  RunAllPendingTasks();
  EXPECT_EQ(0, GetNumPendingAccessTokenRequests());
}

}  // namespace crosapi
