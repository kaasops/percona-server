/*****************************************************************************

Copyright (c) 2020, 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/
#include <gtest/gtest.h>
#include <cerrno>
#include <chrono>

#include "os0file.h"

extern bool srv_use_fdatasync;

class os0file_t : public ::testing::Test {
 protected:
  void SetUp() override {
    bool success;
    os_file_delete_if_exists_func(TEST_FILE_NAME, nullptr);
    test_file = os_file_create_func(TEST_FILE_NAME, OS_FILE_CREATE,
                                    OS_BUFFERED_FILE, false, &success);
    EXPECT_TRUE(success);
  }

  void TearDown() override {
    os_file_close_func(test_file.m_file);
    os_file_delete_func(TEST_FILE_NAME);
  }

  dberr_t write_test_data(const void *data, size_t len,
                          std::chrono::nanoseconds &duration) {
    IORequest request(IORequest::WRITE);

    auto name = TEST_FILE_NAME;
    auto begin = std::chrono::high_resolution_clock::now();
    auto db_err =
        os_file_write_func(request, name, test_file.m_file, data, 0, len);
    auto end = std::chrono::high_resolution_clock::now();

    duration = end - begin;
    return db_err;
  }

  dberr_t read_test_data(void *data, size_t len,
                         std::chrono::nanoseconds &duration) {
    IORequest read_request(IORequest::READ);
    read_request.disable_compression();
    read_request.clear_encrypted();

    auto name = TEST_FILE_NAME;
    auto begin = std::chrono::high_resolution_clock::now();
    auto db_err = os_file_read_func(read_request, name, test_file.m_file, data,
                                    0, len, nullptr);
    auto end = std::chrono::high_resolution_clock::now();

    duration = end - begin;
    return db_err;
  }

  bool flush_test_data(std::chrono::nanoseconds &duration) {
    auto begin = std::chrono::high_resolution_clock::now();
    auto success = os_file_flush_func(test_file.m_file);
    auto end = std::chrono::high_resolution_clock::now();

    duration = end - begin;
    return success;
  }

  void write_read_flush(const void *data, void *buffer, const size_t len,
                        std::chrono::nanoseconds &write_duration_total,
                        std::chrono::nanoseconds &read_duration_total,
                        std::chrono::nanoseconds &flush_duration_total) {
    using namespace std::chrono;
    nanoseconds write_duration(0);
    auto write_err = write_test_data(data, len, write_duration);
    EXPECT_EQ(write_err, DB_SUCCESS);
    write_duration_total += write_duration;

    nanoseconds read_duration(0);
    auto read_err = read_test_data(buffer, len, read_duration);
    EXPECT_EQ(read_err, DB_SUCCESS);
    EXPECT_FALSE(memcmp(buffer, data, len));
    read_duration_total += read_duration;

    nanoseconds flush_duration(0);
    auto success = flush_test_data(flush_duration);
    EXPECT_TRUE(success);
    flush_duration_total += flush_duration;
  }

  void write_read_flush_loop(const void *data, void *buffer, const size_t len,
                             const int loops) {
    using namespace std::chrono;
    nanoseconds flushes(0);
    nanoseconds writes(0);
    nanoseconds reads(0);

    for (int i = 0; i < loops; ++i) {
      write_read_flush(data, buffer, len, writes, reads, flushes);
    }
    auto writes_ms = duration_cast<milliseconds>(writes).count();
    auto reads_ms = duration_cast<milliseconds>(reads).count();
    auto flushes_ms = duration_cast<milliseconds>(flushes).count();
    std::cout << "Write duration total: " << writes_ms << " ms" << std::endl;
    std::cout << "Read duration total: " << reads_ms << " ms" << std::endl;
    std::cout << "Flush duration total: " << flushes_ms << " ms" << std::endl;
  }

  pfs_os_file_t test_file;
  static constexpr char TEST_FILE_NAME[] = "os0file-t-temp.txt";
};

/* SyncFileIO::os_error() must report the OS error code
captured at the failing syscall, not whatever errno happens to hold at the
time it is read - these tests exercise the class directly, below the
os_file_io() retry loop. */

TEST_F(os0file_t, sync_file_io_os_error_reports_failure_of_closed_fd) {
  char buffer[16] = {};
  IORequest write_request(IORequest::WRITE);
  /* OS_FILE_CLOSED is an invalid handle by construction, so the write is
  guaranteed to fail at the syscall - no real file needed for this case. */
  SyncFileIO sync_file_io(OS_FILE_CLOSED, buffer, sizeof(buffer), 0);

  auto n_bytes = sync_file_io.execute(write_request);

  ASSERT_LT(n_bytes, 0);
#ifndef _WIN32
  EXPECT_EQ(sync_file_io.os_error(), EBADF);
#else
  /* The contract does not commit to a specific Windows GetLastError() value for
  this case - only that a failure is captured at all. */
  EXPECT_NE(sync_file_io.os_error(), 0);
#endif
}

TEST_F(os0file_t, sync_file_io_os_error_survives_errno_reset) {
  char buffer[16] = {};
  IORequest write_request(IORequest::WRITE);
  SyncFileIO sync_file_io(OS_FILE_CLOSED, buffer, sizeof(buffer), 0);

  auto n_bytes = sync_file_io.execute(write_request);
  ASSERT_LT(n_bytes, 0);

#ifndef _WIN32
  auto captured = sync_file_io.os_error();
  ASSERT_EQ(captured, EBADF);
  /* Simulate a syscall clobbering the global errno between the failure and
  the point where a diagnostic would read it - the captured value must not
  depend on the global at all (stale-immunity). */
  errno = 0;
  EXPECT_EQ(sync_file_io.os_error(), captured);
#endif
}

TEST_F(os0file_t, sync_file_io_os_error_is_zero_on_success) {
  static constexpr char TEST_DATA[] = "testdata42";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  IORequest write_request(IORequest::WRITE);
  SyncFileIO sync_file_io(test_file.m_file, const_cast<char *>(TEST_DATA), LEN,
                          0);

  auto n_bytes = sync_file_io.execute(write_request);

  ASSERT_EQ(static_cast<size_t>(n_bytes), LEN);
  EXPECT_EQ(sync_file_io.os_error(), 0);
}

TEST_F(os0file_t, sync_file_io_os_error_is_zero_on_short_read_without_failure) {
  /* The file created by SetUp() is empty: a pread() past EOF returns a
  short (zero-byte) result, which is not a syscall failure -
  requires os_error() to report 0 here, not a stale/unrelated code. */
  char buffer[16];
  IORequest read_request(IORequest::READ);
  read_request.disable_compression();
  read_request.clear_encrypted();
  SyncFileIO sync_file_io(test_file.m_file, buffer, sizeof(buffer), 0);

  auto n_bytes = sync_file_io.execute(read_request);

  ASSERT_EQ(n_bytes, 0);
  EXPECT_EQ(sync_file_io.os_error(), 0);
}

TEST_F(os0file_t, hundred_10_byte_writes_reads_flushes_with_fsync) {
  srv_use_fdatasync = false;
  static constexpr char TEST_DATA[] = "testdata42";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  char buffer[LEN];
  write_read_flush_loop(TEST_DATA, buffer, LEN, 100);
}

TEST_F(os0file_t, hundred_10_byte_writes_reads_flushes_with_fdatasync) {
  srv_use_fdatasync = true;
  static constexpr char TEST_DATA[] = "testdata42";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  char buffer[LEN];
  write_read_flush_loop(TEST_DATA, buffer, LEN, 100);
}

/* The tests below were used to measure execution times in various scenarios.
They perform loops of large writes and many fsyncs/fdatasyncs so they last a
while. Disabled prefix makes it so that they don't execute with the
merge_innodb_tests-t suite, but we can manually run them by providing the:
--gtest_also_run_disabled_tests flag during execution */
TEST_F(os0file_t,
       DISABLED_ten_thousand_1_byte_writes_reads_flushes_with_fsync) {
  srv_use_fdatasync = false;
  static constexpr char TEST_DATA[] = "!";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  char buffer[LEN];
  write_read_flush_loop(TEST_DATA, buffer, LEN, 10000);
}

TEST_F(os0file_t,
       DISABLED_ten_thousand_1_byte_writes_reads_flushes_with_fdatasync) {
  srv_use_fdatasync = true;
  static constexpr char TEST_DATA[] = "testdata42";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  char buffer[LEN];
  write_read_flush_loop(TEST_DATA, buffer, LEN, 10000);
}

TEST_F(os0file_t, DISABLED_thousand_10_byte_writes_reads_flushes_with_fsync) {
  srv_use_fdatasync = false;
  static constexpr char TEST_DATA[] = "testdata42";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  char buffer[LEN];
  write_read_flush_loop(TEST_DATA, buffer, LEN, 1000);
}

TEST_F(os0file_t,
       DISABLED_thousand_10_byte_writes_reads_flushes_with_fdatasync) {
  srv_use_fdatasync = true;
  static constexpr char TEST_DATA[] = "testdata42";
  static constexpr size_t LEN = sizeof(TEST_DATA);
  char buffer[LEN];
  write_read_flush_loop(TEST_DATA, buffer, LEN, 1000);
}

TEST_F(os0file_t, DISABLED_thousand_1000_byte_writes_reads_flushes_with_fsync) {
  srv_use_fdatasync = false;

  constexpr int LEN = 1000;
  char data[LEN];
  char buffer[LEN];
  for (int i = 0; i < LEN; ++i) {
    data[i] = 'a' + i % ('z' - 'a' + 1);
  }

  write_read_flush_loop(data, buffer, LEN, 1000);
}

TEST_F(os0file_t,
       DISABLED_thousand_1000_byte_writes_reads_flushes_with_fdatasync) {
  srv_use_fdatasync = true;

  constexpr int LEN = 1000;
  char data[LEN];
  char buffer[LEN];
  for (int i = 0; i < LEN; ++i) {
    data[i] = 'a' + i % ('z' - 'a' + 1);
  }

  write_read_flush_loop(data, buffer, LEN, 1000);
}

TEST_F(os0file_t, DISABLED_thousand_1M_byte_writes_reads_flushes_with_fsync) {
  srv_use_fdatasync = false;

  constexpr int LEN = 1000000;
  char data[LEN];
  char buffer[LEN];
  for (int i = 0; i < LEN; ++i) {
    data[i] = 'a' + i % ('z' - 'a' + 1);
  }

  write_read_flush_loop(data, buffer, LEN, 1000);
}

TEST_F(os0file_t,
       DISABLED_thousand_1M_byte_writes_reads_flushes_with_fdatasync) {
  srv_use_fdatasync = true;

  constexpr int LEN = 1000000;
  char data[LEN];
  char buffer[LEN];
  for (int i = 0; i < LEN; ++i) {
    data[i] = 'a' + i % ('z' - 'a' + 1);
  }

  write_read_flush_loop(data, buffer, LEN, 1000);
}
