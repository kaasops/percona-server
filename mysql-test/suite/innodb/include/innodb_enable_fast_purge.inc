# One purge round under the current innodb_enable_fast_purge: the history is
# first purged to the end (so that only t1 is in the round), purge is
# stopped, the rows of t1 are deleted in one transaction (all undo records of
# one table), purge runs to the end, and the purge threads that took the
# index lock are counted. The rows are put back afterwards.
#
# In: $n_rows

SET GLOBAL innodb_purge_run_now = ON;
--source include/wait_innodb_all_purged.inc
SET GLOBAL innodb_purge_stop_now = ON;
TRUNCATE TABLE performance_schema.events_waits_history_long;
DELETE FROM t1;
--let $wait_condition = SELECT COUNT > 0 FROM INFORMATION_SCHEMA.INNODB_METRICS WHERE NAME = 'trx_rseg_history_len'
--source include/wait_condition.inc
SET GLOBAL innodb_purge_run_now = ON;
--source include/wait_innodb_all_purged.inc
--let $used = `SELECT COUNT(DISTINCT w.THREAD_ID) FROM performance_schema.events_waits_history_long w JOIN performance_schema.threads t USING (THREAD_ID) WHERE w.EVENT_NAME = 'wait/synch/sxlock/innodb/index_tree_rw_lock' AND t.NAME IN ('thread/innodb/srv_purge_thread', 'thread/innodb/srv_worker_thread')`
# The number of threads with ON depends on which worker takes which group,
# so only whether it is one or more is printed.
--disable_query_log
--eval SELECT $used = 1 AS one_thread, $used > 1 AS several_threads
--enable_query_log
--disable_query_log
--eval INSERT INTO t1 (a, b) WITH RECURSIVE n (i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < $n_rows) SELECT i, i FROM n
--enable_query_log
