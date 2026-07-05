// Copyright (c) 2024-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_ERROR_H
#define BITCOIN_KERNEL_ERROR_H

#include <cassert>
#include <string_view>

namespace kernel {

//! Errors that are fatal to the operation of the node, reported through
//! Notifications::fatalError(). The description returned by
//! FatalErrorDescription() is a static, untranslated summary of the error;
//! any dynamic context (a path, an OS error message, a block hash) is carried
//! separately, positionally, in the notification's `args` vector so that the
//! application can translate the message outside of the kernel. Each value
//! below documents the exact contents expected in `args`.
enum class FatalError {
    //! args: {nested error string from ActivateBestChains()}
    ACTIVATE_BEST_CHAINS_FAILED,
    //! args: {block hash, as returned by uint256::ToString()}
    ASSUMEUTXO_DATA_NOT_FOUND,
    //! args: {} (no dynamic data)
    BLOCK_DISCONNECT_FAILED,
    //! args: {} (no dynamic data)
    BLOCK_FILE_CLOSE_FAILED,
    //! args: {} (no dynamic data)
    BLOCK_READ_FAILED,
    //! args: {} (no dynamic data)
    BLOCK_WRITE_FAILED,
    //! args: {} (no dynamic data)
    CORRUPT_BLOCK_FOUND,
    //! args: {} (no dynamic data)
    DISK_SPACE_TOO_LOW,
    //! args: {} (no dynamic data)
    FAILED_TO_START_INDEXES,
    //! args: {directory path, as returned by fs::PathToString()}
    SNAPSHOT_CHAINSTATE_DIR_REMOVAL_FAILED,
    //! args: {old directory path, new directory path}, both via fs::PathToString()
    SNAPSHOT_CHAINSTATE_RENAME_FAILED,
    //! args: {height the snapshot forked from, height the snapshot claims to
    //! reach, both stringified}, optionally followed by a third element
    //! carrying a nested chainstate-rename error string, only present when
    //! that rename also failed
    SNAPSHOT_VALIDATION_FAILED,
    //! args: {message from the caught std::exception's what(), while calling FlushStateToDisk()}
    SYSTEM_ERROR_WHILE_FLUSHING,
    //! args: {message from the caught std::exception's what(), while reading an external block file}
    SYSTEM_ERROR_WHILE_LOADING_EXTERNAL_BLOCK_FILE,
    //! args: {message from the caught std::exception's what(), while writing a block to disk}
    SYSTEM_ERROR_WHILE_SAVING_BLOCK,
    //! args: {} (no dynamic data)
    UNDO_DATA_WRITE_FAILED,
    //! args: {} (no dynamic data)
    UNDO_FILE_CLOSE_FAILED,
};

//! Errors that occur while flushing block data to disk, reported through
//! Notifications::flushError().
enum class FlushError {
    BLOCK_FILE_FLUSH_FAILED,
    UNDO_FILE_FLUSH_FAILED,
};

//! Static, untranslated one-line description of a fatal error. Used by the
//! kernel itself where a string is unavoidable (e.g. BlockValidationState and
//! the C API), without pulling translation into the kernel.
constexpr std::string_view FatalErrorDescription(FatalError error)
{
    switch (error) {
    case FatalError::ACTIVATE_BEST_CHAINS_FAILED: return "Failed to connect best block.";
    case FatalError::ASSUMEUTXO_DATA_NOT_FOUND: return "Assumeutxo data not found for the given blockhash.";
    case FatalError::BLOCK_DISCONNECT_FAILED: return "Failed to disconnect block.";
    case FatalError::BLOCK_FILE_CLOSE_FAILED: return "Failed to close file when writing block.";
    case FatalError::BLOCK_READ_FAILED: return "Failed to read block.";
    case FatalError::BLOCK_WRITE_FAILED: return "Failed to write block.";
    case FatalError::CORRUPT_BLOCK_FOUND: return "Corrupt block found indicating potential hardware failure.";
    case FatalError::DISK_SPACE_TOO_LOW: return "Disk space is too low!";
    case FatalError::FAILED_TO_START_INDEXES: return "Failed to start indexes, shutting down.";
    case FatalError::SNAPSHOT_CHAINSTATE_DIR_REMOVAL_FAILED: return "Failed to remove snapshot chainstate dir.";
    case FatalError::SNAPSHOT_CHAINSTATE_RENAME_FAILED: return "Failed to rename the background chainstate directory.";
    case FatalError::SNAPSHOT_VALIDATION_FAILED: return "Failed to validate the assumeutxo snapshot state.";
    case FatalError::SYSTEM_ERROR_WHILE_FLUSHING: return "System error while flushing.";
    case FatalError::SYSTEM_ERROR_WHILE_LOADING_EXTERNAL_BLOCK_FILE: return "System error while loading external block file.";
    case FatalError::SYSTEM_ERROR_WHILE_SAVING_BLOCK: return "System error while saving block to disk.";
    case FatalError::UNDO_DATA_WRITE_FAILED: return "Failed to write undo data.";
    case FatalError::UNDO_FILE_CLOSE_FAILED: return "Failed to close block undo file.";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

//! Static, untranslated one-line description of a flush error.
constexpr std::string_view FlushErrorDescription(FlushError error)
{
    switch (error) {
    case FlushError::BLOCK_FILE_FLUSH_FAILED: return "Flushing block file to disk failed. This is likely the result of an I/O error.";
    case FlushError::UNDO_FILE_FLUSH_FAILED: return "Flushing undo file to disk failed. This is likely the result of an I/O error.";
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

} // namespace kernel

#endif // BITCOIN_KERNEL_ERROR_H
