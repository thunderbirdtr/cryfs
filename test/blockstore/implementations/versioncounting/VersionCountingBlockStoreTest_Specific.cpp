#include <gtest/gtest.h>
#include "blockstore/implementations/versioncounting/VersionCountingBlockStore.h"
#include "blockstore/implementations/testfake/FakeBlockStore.h"
#include "blockstore/utils/BlockStoreUtils.h"
#include <cpp-utils/data/DataFixture.h>
#include <cpp-utils/tempfile/TempFile.h>

using ::testing::Test;

using cpputils::DataFixture;
using cpputils::Data;
using cpputils::unique_ref;
using cpputils::make_unique_ref;
using cpputils::TempFile;
using boost::none;

using blockstore::testfake::FakeBlockStore;

using namespace blockstore::versioncounting;

class VersionCountingBlockStoreTest: public Test {
public:
  static constexpr unsigned int BLOCKSIZE = 1024;
  VersionCountingBlockStoreTest():
    stateFile(false),
    baseBlockStore(new FakeBlockStore),
    blockStore(make_unique_ref<VersionCountingBlockStore>(std::move(cpputils::nullcheck(std::unique_ptr<FakeBlockStore>(baseBlockStore)).value()), stateFile.path())),
    data(DataFixture::generate(BLOCKSIZE)) {
  }
  TempFile stateFile;
  FakeBlockStore *baseBlockStore;
  unique_ref<VersionCountingBlockStore> blockStore;
  Data data;

  blockstore::Key CreateBlockReturnKey() {
    return CreateBlockReturnKey(data);
  }

  blockstore::Key CreateBlockReturnKey(const Data &initData) {
    return blockStore->create(initData)->key();
  }

  Data loadBaseBlock(const blockstore::Key &key) {
    auto block = baseBlockStore->load(key).value();
    Data result(block->size());
    std::memcpy(result.data(), block->data(), data.size());
    return result;
  }

  Data loadBlock(const blockstore::Key &key) {
    auto block = blockStore->load(key).value();
    Data result(block->size());
    std::memcpy(result.data(), block->data(), data.size());
    return result;
  }

  void modifyBlock(const blockstore::Key &key) {
    auto block = blockStore->load(key).value();
    uint64_t data = 5;
    block->write(&data, 0, sizeof(data));
  }

  void rollbackBaseBlock(const blockstore::Key &key, const Data &data) {
    auto block = baseBlockStore->load(key).value();
    block->resize(data.size());
    block->write(data.data(), 0, data.size());
  }

  void decreaseVersionNumber(const blockstore::Key &key) {
    auto baseBlock = baseBlockStore->load(key).value();
    uint64_t version = *(uint64_t*)((uint8_t*)baseBlock->data()+VersionCountingBlock::VERSION_HEADER_OFFSET);
    ASSERT(version > 1, "Can't decrease the lowest allowed version number");
    version -= 1;
    baseBlock->write((char*)&version, VersionCountingBlock::VERSION_HEADER_OFFSET, sizeof(version));
  }

  void changeClientId(const blockstore::Key &key) {
    auto baseBlock = baseBlockStore->load(key).value();
    uint32_t clientId = *(uint32_t*)((uint8_t*)baseBlock->data()+VersionCountingBlock::CLIENTID_HEADER_OFFSET);
    clientId += 1;
    baseBlock->write((char*)&clientId, VersionCountingBlock::CLIENTID_HEADER_OFFSET, sizeof(clientId));
  }

  void deleteBlock(const blockstore::Key &key) {
    blockStore->remove(blockStore->load(key).value());
  }

  void insertBaseBlock(const blockstore::Key &key, Data data) {
    EXPECT_NE(none, baseBlockStore->tryCreate(key, std::move(data)));
  }

private:
  DISALLOW_COPY_AND_ASSIGN(VersionCountingBlockStoreTest);
};

// Test that a decreasing version number is not allowed
TEST_F(VersionCountingBlockStoreTest, RollbackPrevention_DoesntAllowDecreasingVersionNumberForSameClient_1) {
  auto key = CreateBlockReturnKey();
  Data oldBaseBlock = loadBaseBlock(key);
  modifyBlock(key);
  rollbackBaseBlock(key, oldBaseBlock);
  EXPECT_EQ(boost::none, blockStore->load(key));
}

TEST_F(VersionCountingBlockStoreTest, RollbackPrevention_DoesntAllowDecreasingVersionNumberForSameClient_2) {
  auto key = CreateBlockReturnKey();
  // Increase the version number
  modifyBlock(key);
  // Decrease the version number again
  decreaseVersionNumber(key);
  EXPECT_EQ(boost::none, blockStore->load(key));
}

// Test that a different client doesn't need to have a higher version number (i.e. version numbers are per client).
TEST_F(VersionCountingBlockStoreTest, RollbackPrevention_DoesAllowDecreasingVersionNumberForDifferentClient) {
  auto key = CreateBlockReturnKey();
  // Increase the version number
  modifyBlock(key);
  // Fake a modification by a different client with lower version numbers
  changeClientId(key);
  decreaseVersionNumber(key);
  EXPECT_NE(boost::none, blockStore->load(key));
}

// Test that it doesn't allow a rollback to the "newest" block of a client, when this block was superseded by a version of a different client
TEST_F(VersionCountingBlockStoreTest, RollbackPrevention_DoesntAllowSameVersionNumberForOldClient) {
  auto key = CreateBlockReturnKey();
  // Increase the version number
  modifyBlock(key);
  Data oldBaseBlock = loadBaseBlock(key);
  // Fake a modification by a different client with lower version numbers
  changeClientId(key);
  loadBlock(key); // make the block store know about this other client's modification
  // Rollback to old client
  rollbackBaseBlock(key, oldBaseBlock);
  EXPECT_EQ(boost::none, blockStore->load(key));
}

// Test that deleted blocks cannot be re-introduced
TEST_F(VersionCountingBlockStoreTest, RollbackPrevention_DoesntAllowReintroducingDeletedBlocks) {
  auto key = CreateBlockReturnKey();
  Data oldBaseBlock = loadBaseBlock(key);
  deleteBlock(key);
  insertBaseBlock(key, std::move(oldBaseBlock));
  EXPECT_EQ(boost::none, blockStore->load(key));
}

TEST_F(VersionCountingBlockStoreTest, PhysicalBlockSize_zerophysical) {
  EXPECT_EQ(0u, blockStore->blockSizeFromPhysicalBlockSize(0));
}

TEST_F(VersionCountingBlockStoreTest, PhysicalBlockSize_zerovirtual) {
  auto key = CreateBlockReturnKey(Data(0));
  auto base = baseBlockStore->load(key).value();
  EXPECT_EQ(0u, blockStore->blockSizeFromPhysicalBlockSize(base->size()));
}

TEST_F(VersionCountingBlockStoreTest, PhysicalBlockSize_negativeboundaries) {
  // This tests that a potential if/else in blockSizeFromPhysicalBlockSize that catches negative values has the
  // correct boundary set. We test the highest value that is negative and the smallest value that is positive.
  auto physicalSizeForVirtualSizeZero = baseBlockStore->load(CreateBlockReturnKey(Data(0))).value()->size();
  if (physicalSizeForVirtualSizeZero > 0) {
    EXPECT_EQ(0u, blockStore->blockSizeFromPhysicalBlockSize(physicalSizeForVirtualSizeZero - 1));
  }
  EXPECT_EQ(0u, blockStore->blockSizeFromPhysicalBlockSize(physicalSizeForVirtualSizeZero));
  EXPECT_EQ(1u, blockStore->blockSizeFromPhysicalBlockSize(physicalSizeForVirtualSizeZero + 1));
}

TEST_F(VersionCountingBlockStoreTest, PhysicalBlockSize_positive) {
  auto key = CreateBlockReturnKey(Data(10*1024));
  auto base = baseBlockStore->load(key).value();
  EXPECT_EQ(10*1024u, blockStore->blockSizeFromPhysicalBlockSize(base->size()));
}
