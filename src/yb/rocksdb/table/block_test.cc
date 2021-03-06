//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#include <stdio.h>
#include <string>
#include <vector>

#include "yb/rocksdb/db/dbformat.h"
#include "yb/rocksdb/db/memtable.h"
#include "yb/rocksdb/db/write_batch_internal.h"
#include "yb/rocksdb/db.h"
#include "yb/rocksdb/env.h"
#include "yb/rocksdb/iterator.h"
#include "yb/rocksdb/table.h"
#include "yb/rocksdb/slice_transform.h"
#include "yb/rocksdb/table/block.h"
#include "yb/rocksdb/table/block_builder.h"
#include "yb/rocksdb/table/format.h"
#include "yb/rocksdb/table/block_hash_index.h"
#include "yb/rocksdb/util/random.h"
#include "yb/rocksdb/util/testharness.h"
#include "yb/rocksdb/util/testutil.h"

namespace rocksdb {

std::string GenerateKey(int primary_key, int secondary_key, int padding_size,
                        Random *rnd) {
  char buf[50];
  char *p = &buf[0];
  snprintf(buf, sizeof(buf), "%6d%4d", primary_key, secondary_key);
  std::string k(p);
  if (padding_size) {
    k += RandomString(rnd, padding_size);
  }

  return k;
}

// Generate random key value pairs.
// The generated key will be sorted. You can tune the parameters to generated
// different kinds of test key/value pairs for different scenario.
void GenerateRandomKVs(std::vector<std::string> *keys,
                       std::vector<std::string> *values, const int from,
                       const int len, const int step = 1,
                       const int padding_size = 0,
                       const int keys_share_prefix = 1) {
  Random rnd(302);

  // generate different prefix
  for (int i = from; i < from + len; i += step) {
    // generating keys that shares the prefix
    for (int j = 0; j < keys_share_prefix; ++j) {
      keys->emplace_back(GenerateKey(i, j, padding_size, &rnd));

      // 100 bytes values
      values->emplace_back(RandomString(&rnd, 100));
    }
  }
}

class BlockTest : public testing::Test {};

// block test
TEST_F(BlockTest, SimpleTest) {
  Random rnd(301);
  Options options = Options();
  std::unique_ptr<InternalKeyComparator> ic;
  ic.reset(new test::PlainInternalKeyComparator(options.comparator));

  std::vector<std::string> keys;
  std::vector<std::string> values;
  BlockBuilder builder(16);
  int num_records = 100000;

  GenerateRandomKVs(&keys, &values, 0, num_records);
  // add a bunch of records to a block
  for (int i = 0; i < num_records; i++) {
    builder.Add(keys[i], values[i]);
  }

  // read serialized contents of the block
  Slice rawblock = builder.Finish();

  // create block reader
  BlockContents contents;
  contents.data = rawblock;
  contents.cachable = false;
  Block reader(std::move(contents));

  // read contents of block sequentially
  int count = 0;
  InternalIterator *iter = reader.NewIterator(options.comparator);
  for (iter->SeekToFirst(); iter->Valid(); count++, iter->Next()) {

    // read kv from block
    Slice k = iter->key();
    Slice v = iter->value();

    // compare with lookaside array
    ASSERT_EQ(k.ToString().compare(keys[count]), 0);
    ASSERT_EQ(v.ToString().compare(values[count]), 0);
  }
  delete iter;

  // read block contents randomly
  iter = reader.NewIterator(options.comparator);
  for (int i = 0; i < num_records; i++) {

    // find a random key in the lookaside array
    int index = rnd.Uniform(num_records);
    Slice k(keys[index]);

    // search in block for this key
    iter->Seek(k);
    ASSERT_TRUE(iter->Valid());
    Slice v = iter->value();
    ASSERT_EQ(v.ToString().compare(values[index]), 0);
  }
  delete iter;
}

// return the block contents
BlockContents GetBlockContents(std::unique_ptr<BlockBuilder> *builder,
                               const std::vector<std::string> &keys,
                               const std::vector<std::string> &values,
                               const int prefix_group_size = 1) {
  builder->reset(new BlockBuilder(1 /* restart interval */));

  // Add only half of the keys
  for (size_t i = 0; i < keys.size(); ++i) {
    (*builder)->Add(keys[i], values[i]);
  }
  Slice rawblock = (*builder)->Finish();

  BlockContents contents;
  contents.data = rawblock;
  contents.cachable = false;

  return contents;
}

void CheckBlockContents(BlockContents contents, const int max_key,
                        const std::vector<std::string> &keys,
                        const std::vector<std::string> &values) {
  const size_t prefix_size = 6;
  // create block reader
  BlockContents contents_ref(contents.data, contents.cachable,
                             contents.compression_type);
  Block reader1(std::move(contents));
  Block reader2(std::move(contents_ref));

  std::unique_ptr<const SliceTransform> prefix_extractor(
      NewFixedPrefixTransform(prefix_size));

  {
    auto iter1 = reader1.NewIterator(nullptr);
    auto iter2 = reader1.NewIterator(nullptr);
    reader1.SetBlockHashIndex(CreateBlockHashIndexOnTheFly(
        iter1, iter2, static_cast<uint32_t>(keys.size()), BytewiseComparator(),
        prefix_extractor.get()));

    delete iter1;
    delete iter2;
  }

  std::unique_ptr<InternalIterator> hash_iter(
      reader1.NewIterator(BytewiseComparator(), nullptr, false));

  std::unique_ptr<InternalIterator> regular_iter(
      reader2.NewIterator(BytewiseComparator()));

  // Seek existent keys
  for (size_t i = 0; i < keys.size(); i++) {
    hash_iter->Seek(keys[i]);
    ASSERT_OK(hash_iter->status());
    ASSERT_TRUE(hash_iter->Valid());

    Slice v = hash_iter->value();
    ASSERT_EQ(v.ToString().compare(values[i]), 0);
  }

  // Seek non-existent keys.
  // For hash index, if no key with a given prefix is not found, iterator will
  // simply be set as invalid; whereas the binary search based iterator will
  // return the one that is closest.
  for (int i = 1; i < max_key - 1; i += 2) {
    auto key = GenerateKey(i, 0, 0, nullptr);
    hash_iter->Seek(key);
    ASSERT_TRUE(!hash_iter->Valid());

    regular_iter->Seek(key);
    ASSERT_TRUE(regular_iter->Valid());
  }
}

// In this test case, no two key share same prefix.
TEST_F(BlockTest, SimpleIndexHash) {
  const int kMaxKey = 100000;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  GenerateRandomKVs(&keys, &values, 0 /* first key id */,
                    kMaxKey /* last key id */, 2 /* step */,
                    8 /* padding size (8 bytes randomly generated suffix) */);

  std::unique_ptr<BlockBuilder> builder;
  auto contents = GetBlockContents(&builder, keys, values);

  CheckBlockContents(std::move(contents), kMaxKey, keys, values);
}

TEST_F(BlockTest, IndexHashWithSharedPrefix) {
  const int kMaxKey = 100000;
  // for each prefix, there will be 5 keys starts with it.
  const int kPrefixGroup = 5;
  std::vector<std::string> keys;
  std::vector<std::string> values;
  // Generate keys with same prefix.
  GenerateRandomKVs(&keys, &values, 0,  // first key id
                    kMaxKey,            // last key id
                    2,                  // step
                    10,                 // padding size,
                    kPrefixGroup);

  std::unique_ptr<BlockBuilder> builder;
  auto contents = GetBlockContents(&builder, keys, values, kPrefixGroup);

  CheckBlockContents(std::move(contents), kMaxKey, keys, values);
}

namespace {

std::string GetPaddedNum(int i) {
  return StringPrintf("%010d", i);
}

yb::Result<std::string> GetMiddleKey(const int num_keys, const int block_restart_interval) {
  BlockBuilder builder(block_restart_interval);

  for (int i = 1; i <= num_keys; ++i) {
    const auto padded_num = GetPaddedNum(i);
    builder.Add("k" + padded_num, "v" + padded_num);
  }

  BlockContents contents;
  contents.data = builder.Finish();
  contents.cachable = false;
  Block reader(std::move(contents));

  return VERIFY_RESULT(reader.GetMiddleKey()).ToString();
}

void CheckMiddleKey(
    const int num_keys, const int block_restart_interval, const int expected_middle_key) {
  const auto middle_key = ASSERT_RESULT(GetMiddleKey(num_keys, block_restart_interval));
  ASSERT_EQ(middle_key, "k" + GetPaddedNum(expected_middle_key)) << "For num_keys = " << num_keys;
}

} // namespace

TEST_F(BlockTest, GetMiddleKey) {
  const auto block_restart_interval = 1;

  const auto empty_block_middle_key = GetMiddleKey(/* num_keys =*/ 0, block_restart_interval);
  ASSERT_NOK(empty_block_middle_key) << empty_block_middle_key;
  ASSERT_TRUE(empty_block_middle_key.status().IsIncomplete()) << empty_block_middle_key;

  CheckMiddleKey(/* num_keys =*/ 1, block_restart_interval, /* expected_middle_key =*/ 1);
  CheckMiddleKey(/* num_keys =*/ 2, block_restart_interval, /* expected_middle_key =*/ 2);
  CheckMiddleKey(/* num_keys =*/ 3, block_restart_interval, /* expected_middle_key =*/ 2);
  CheckMiddleKey(/* num_keys =*/ 15, block_restart_interval, /* expected_middle_key =*/ 8);
  CheckMiddleKey(/* num_keys =*/ 16, block_restart_interval, /* expected_middle_key =*/ 9);
}

}  // namespace rocksdb

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
