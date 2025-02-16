// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/host/native_messaging/native_messaging_writer.h"

#include <stdint.h>

#include <memory>
#include <utility>

#include "base/json/json_reader.h"
#include "base/stl_util.h"
#include "base/values.h"
#include "remoting/host/setup/test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace remoting {

class NativeMessagingWriterTest : public testing::Test {
 public:
  NativeMessagingWriterTest();
  ~NativeMessagingWriterTest() override;

  void SetUp() override;

 protected:
  std::unique_ptr<NativeMessagingWriter> writer_;
  base::File read_file_;
  base::File write_file_;
};

NativeMessagingWriterTest::NativeMessagingWriterTest() = default;
NativeMessagingWriterTest::~NativeMessagingWriterTest() = default;

void NativeMessagingWriterTest::SetUp() {
  ASSERT_TRUE(MakePipe(&read_file_, &write_file_));
  writer_ = std::make_unique<NativeMessagingWriter>(std::move(write_file_));
}

TEST_F(NativeMessagingWriterTest, GoodMessage) {
  base::DictionaryValue message;
  message.SetInteger("foo", 42);
  EXPECT_TRUE(writer_->WriteMessage(message));

  // Read from the pipe and verify the content.
  uint32_t length;
  int read = read_file_.ReadAtCurrentPos(reinterpret_cast<char*>(&length), 4);
  EXPECT_EQ(4, read);
  std::string content(length, '\0');
  read = read_file_.ReadAtCurrentPos(base::data(content), length);
  EXPECT_EQ(static_cast<int>(length), read);

  // |content| should now contain serialized |message|.
  std::unique_ptr<base::Value> written_message =
      base::JSONReader::ReadDeprecated(content);
  EXPECT_TRUE(message.Equals(written_message.get()));

  // Nothing more should have been written. Close the write-end of the pipe,
  // and verify the read end immediately hits EOF.
  writer_.reset(nullptr);
  char unused;
  read = read_file_.ReadAtCurrentPos(&unused, 1);
  EXPECT_LE(read, 0);
}

TEST_F(NativeMessagingWriterTest, SecondMessage) {
  base::DictionaryValue message1;
  base::DictionaryValue message2;
  message2.SetInteger("foo", 42);
  EXPECT_TRUE(writer_->WriteMessage(message1));
  EXPECT_TRUE(writer_->WriteMessage(message2));
  writer_.reset(nullptr);

  // Read two messages.
  uint32_t length;
  int read;
  std::string content;
  for (int i = 0; i < 2; i++) {
    read = read_file_.ReadAtCurrentPos(reinterpret_cast<char*>(&length), 4);
    EXPECT_EQ(4, read) << "i = " << i;
    content.resize(length);
    read = read_file_.ReadAtCurrentPos(base::data(content), length);
    EXPECT_EQ(static_cast<int>(length), read) << "i = " << i;
  }

  // |content| should now contain serialized |message2|.
  std::unique_ptr<base::Value> written_message2 =
      base::JSONReader::ReadDeprecated(content);
  EXPECT_TRUE(message2.Equals(written_message2.get()));
}

TEST_F(NativeMessagingWriterTest, FailedWrite) {
  // Close the read end so that writing fails immediately.
  read_file_.Close();

  base::DictionaryValue message;
  EXPECT_FALSE(writer_->WriteMessage(message));
}

}  // namespace remoting
