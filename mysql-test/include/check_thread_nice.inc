# $nice_expected: writers, checkpoint, io, purge, receiver, coordinator, workers.
# Each entry is a numeric nice or "inherit". Run MTR under a nice lower than
# every value the caller requests: raising a thread's nice needs no privilege,
# and an omitted option then differs from a configured one.
--let $wait_condition = SELECT COUNT(DISTINCT CASE NAME WHEN 'thread/innodb/log_writer_thread' THEN 'writers' WHEN 'thread/innodb/log_flusher_thread' THEN 'writers' WHEN 'thread/innodb/log_write_notifier_thread' THEN 'writers' WHEN 'thread/innodb/log_flush_notifier_thread' THEN 'writers' WHEN 'thread/innodb/log_checkpointer_thread' THEN 'checkpoint' WHEN 'thread/innodb/log_files_governor_thread' THEN 'checkpoint' WHEN 'thread/innodb/io_ibuf_thread' THEN 'io' WHEN 'thread/innodb/io_read_thread' THEN 'io' WHEN 'thread/innodb/io_write_thread' THEN 'io' WHEN 'thread/innodb/srv_purge_thread' THEN 'purge' WHEN 'thread/innodb/srv_worker_thread' THEN 'purge' WHEN 'thread/sql/replica_io' THEN 'receiver' WHEN 'thread/sql/replica_sql' THEN 'coordinator' WHEN 'thread/sql/replica_worker' THEN 'workers' END) = 7 FROM performance_schema.threads
--source include/wait_condition.inc
SET SESSION group_concat_max_len=65535;
--let nice_pid_file = `SELECT @@GLOBAL.pid_file`
--let nice_threads = `SELECT GROUP_CONCAT(CONCAT(CASE NAME WHEN 'thread/innodb/log_writer_thread' THEN 'writers' WHEN 'thread/innodb/log_flusher_thread' THEN 'writers' WHEN 'thread/innodb/log_write_notifier_thread' THEN 'writers' WHEN 'thread/innodb/log_flush_notifier_thread' THEN 'writers' WHEN 'thread/innodb/log_checkpointer_thread' THEN 'checkpoint' WHEN 'thread/innodb/log_files_governor_thread' THEN 'checkpoint' WHEN 'thread/innodb/io_ibuf_thread' THEN 'io' WHEN 'thread/innodb/io_read_thread' THEN 'io' WHEN 'thread/innodb/io_write_thread' THEN 'io' WHEN 'thread/innodb/srv_purge_thread' THEN 'purge' WHEN 'thread/innodb/srv_worker_thread' THEN 'purge' WHEN 'thread/sql/replica_io' THEN 'receiver' WHEN 'thread/sql/replica_sql' THEN 'coordinator' WHEN 'thread/sql/replica_worker' THEN 'workers' END, ':', THREAD_OS_ID) SEPARATOR ',') FROM performance_schema.threads WHERE NAME IN ('thread/innodb/log_writer_thread', 'thread/innodb/log_flusher_thread', 'thread/innodb/log_write_notifier_thread', 'thread/innodb/log_flush_notifier_thread', 'thread/innodb/log_checkpointer_thread', 'thread/innodb/log_files_governor_thread', 'thread/innodb/io_ibuf_thread', 'thread/innodb/io_read_thread', 'thread/innodb/io_write_thread', 'thread/innodb/srv_purge_thread', 'thread/innodb/srv_worker_thread', 'thread/sql/replica_io', 'thread/sql/replica_sql', 'thread/sql/replica_worker')`
--let nice_expected = $nice_expected
--let $nice_base_file = $MYSQLTEST_VARDIR/tmp/thread_nice_base.txt
--let nice_base_file = $nice_base_file
--perl
use strict;
use warnings;
open(my $pf, '<', $ENV{nice_pid_file}) or die "pid file: $!";
my $pid = <$pf>;
close($pf);
chomp $pid;
my $read_nice = sub {
  my ($tid) = @_;
  open(my $f, '<', "/proc/$pid/task/$tid/stat") or die "tid $tid: $!";
  my $line = <$f>;
  close($f);
  my @fields = split(' ', substr($line, rindex($line, ')') + 1));
  return $fields[16];
};
my @groups = qw(writers checkpoint io purge receiver coordinator workers);
my @values = split /,/, $ENV{nice_expected};
die "expected seven priority values" unless @values == @groups;
my %expected;
@expected{@groups} = @values;
my $baseline = $read_nice->($pid);
open(my $bo, '>', $ENV{nice_base_file}) or die "base file: $!";
print $bo $baseline;
close($bo);
my %seen;
for my $entry (split /,/, $ENV{nice_threads}) {
  my ($group, $tid) = split /:/, $entry;
  die "missing OS tid for $group" unless $tid =~ /^\d+$/;
  my $want = $expected{$group} eq 'inherit' ? $baseline : $expected{$group};
  my $actual = $read_nice->($tid);
  die "$group tid $tid: expected nice $want, got $actual (main $baseline)"
    unless $actual == $want;
  $seen{$group}++;
}
die "missing thread group $_" for grep { !$seen{$_} } @groups;
EOF
--let $process_nice = `SELECT LOAD_FILE('$nice_base_file')`
--remove_file $nice_base_file
