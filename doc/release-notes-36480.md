Updated settings
----------------

- The spent output index (`-txospenderindex`) uses a new, smaller database
  format. An index built by a previous version keeps working and new blocks are
  written in the new format. After a downgrade, the previous version indexes
  again the blocks added since the upgrade. To reclaim disk space, stop the node, delete the
  `indexes/txospenderindex` directory and restart to rebuild the index. (#36480)

- The spent output index (`-txospenderindex`) can now be used with pruning
  (`-prune`). The index must be built from genesis. Indexes built in a version
  prior to v32 must be rebuilt first. The spent output index continues to grow
  and does not count toward the prune target. (#36480)

Updated RPCs
------------

- `gettxspendingprevout` no longer returns a spend from a block that was
  disconnected by a reorg, even before the index has processed the reorg.
  (#36480)

- With pruning enabled, `gettxspendingprevout` may return a
  `pruned_block_hashes` array for an output whose spend may be found in one or
  more pruned blocks, instead of reporting it as unspent. The blocks are not
  guaranteed to contain the spend. Use `getblockfrompeer` to fetch the blocks,
  then retry the lookup. (#36480)
