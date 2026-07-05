// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/kernel_notifications.h>

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <chain.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/system.h>
#include <kernel/context.h>
#include <kernel/error.h>
#include <kernel/warning.h>
#include <node/abort.h>
#include <node/interface_ui.h>
#include <node/warnings.h>
#include <util/check.h>
#include <util/log.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/translation.h>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using util::ReplaceAll;

static void AlertNotify(const std::string& strMessage)
{
#if HAVE_SYSTEM
    std::string strCmd = gArgs.GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote+safeStatus+singleQuote;
    ReplaceAll(strCmd, "%s", safeStatus);

    std::thread t(runCommand, strCmd);
    t.detach(); // thread runs free
#endif
}

namespace node {

kernel::InterruptResult KernelNotifications::blockTip(SynchronizationState state, const CBlockIndex& index, double verification_progress)
{
    {
        LOCK(m_tip_block_mutex);
        Assume(index.GetBlockHash() != uint256::ZERO);
        m_state.tip_block = index.GetBlockHash();
        m_tip_block_cv.notify_all();
    }

    uiInterface.NotifyBlockTip(state, index, verification_progress);
    if (m_stop_at_height && index.nHeight >= m_stop_at_height) {
        if (!m_shutdown_request()) {
            LogError("Failed to send shutdown signal after reaching stop height\n");
        }
        return kernel::Interrupted{};
    }
    return {};
}

void KernelNotifications::headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync)
{
    uiInterface.NotifyHeaderTip(state, height, timestamp, presync);
}

void KernelNotifications::progress(const bilingual_str& title, int progress_percent, bool resume_possible)
{
    uiInterface.ShowProgress(title.translated, progress_percent, resume_possible);
}

void KernelNotifications::warningSet(kernel::Warning id, const bilingual_str& message)
{
    if (m_warnings.Set(id, message)) {
        AlertNotify(message.original);
    }
}

void KernelNotifications::warningUnset(kernel::Warning id)
{
    m_warnings.Unset(id);
}

//! Translate a kernel flush error into a user-facing message. This is where
//! translation happens, outside of the kernel.
static bilingual_str FlushErrorMessage(kernel::FlushError error)
{
    switch (error) {
    case kernel::FlushError::BLOCK_FILE_FLUSH_FAILED:
        return _("Flushing block file to disk failed. This is likely the result of an I/O error.");
    case kernel::FlushError::UNDO_FILE_FLUSH_FAILED:
        return _("Flushing undo file to disk failed. This is likely the result of an I/O error.");
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

//! Translate a kernel fatal error into a user-facing message, filling the
//! translated template with the raw dynamic arguments provided by the kernel.
//! This is where translation happens, outside of the kernel.
static bilingual_str FatalErrorMessage(kernel::FatalError error, const std::vector<std::string>& args)
{
    switch (error) {
    case kernel::FatalError::FAILED_TO_START_INDEXES:
        return _("Failed to start indexes, shutting down…");
    case kernel::FatalError::ASSUMEUTXO_DATA_NOT_FOUND:
        return strprintf(_("Assumeutxo data not found for the given blockhash '%s'."), args.at(0));
    case kernel::FatalError::DISK_SPACE_TOO_LOW:
        return _("Disk space is too low!");
    case kernel::FatalError::BLOCK_WRITE_FAILED:
        return _("Failed to write block.");
    case kernel::FatalError::BLOCK_FILE_CLOSE_FAILED:
        return _("Failed to close file when writing block.");
    case kernel::FatalError::UNDO_DATA_WRITE_FAILED:
        return _("Failed to write undo data.");
    case kernel::FatalError::UNDO_FILE_CLOSE_FAILED:
        return _("Failed to close block undo file.");
    case kernel::FatalError::CORRUPT_BLOCK_FOUND:
        return _("Corrupt block found indicating potential hardware failure.");
    case kernel::FatalError::BLOCK_READ_FAILED:
        return _("Failed to read block.");
    case kernel::FatalError::BLOCK_DISCONNECT_FAILED:
        return _("Failed to disconnect block.");
    case kernel::FatalError::SYSTEM_ERROR_WHILE_FLUSHING:
        return strprintf(_("System error while flushing: %s"), args.at(0));
    case kernel::FatalError::SYSTEM_ERROR_WHILE_SAVING_BLOCK:
        return strprintf(_("System error while saving block to disk: %s"), args.at(0));
    case kernel::FatalError::SYSTEM_ERROR_WHILE_LOADING_EXTERNAL_BLOCK_FILE:
        return strprintf(_("System error while loading external block file: %s"), args.at(0));
    case kernel::FatalError::SNAPSHOT_CHAINSTATE_DIR_REMOVAL_FAILED:
        return strprintf(_("Failed to remove snapshot chainstate dir (%s). "
                           "Manually remove it before restarting.\n"), args.at(0));
    case kernel::FatalError::SNAPSHOT_CHAINSTATE_RENAME_FAILED:
        return strprintf(_("Rename of '%s' -> '%s' failed. "
                           "Cannot clean up the background chainstate leveldb directory."),
                         args.at(0), args.at(1));
    case kernel::FatalError::SNAPSHOT_VALIDATION_FAILED: {
        bilingual_str message = strprintf(_(
            "%s failed to validate the -assumeutxo snapshot state. "
            "This indicates a hardware problem, or a bug in the software, or a "
            "bad software modification that allowed an invalid snapshot to be "
            "loaded. As a result of this, the node will shut down and stop using any "
            "state that was built on the snapshot, resetting the chain height "
            "from %s to %s. On the next "
            "restart, the node will resume syncing from %s "
            "without using any snapshot data. "
            "Please report this incident to %s, including how you obtained the snapshot. "
            "The invalid snapshot chainstate will be left on disk in case it is "
            "helpful in diagnosing the issue that caused this error."),
            CLIENT_NAME, args.at(0), args.at(1), args.at(1), CLIENT_BUGREPORT);
        // Optional nested error from InvalidateCoinsDBOnDisk() (a util::Result
        // message produced elsewhere; see the ⚠ bridge note in the plan).
        if (args.size() > 2) message += Untranslated("\n") + Untranslated(args.at(2));
        return message;
    }
    case kernel::FatalError::ACTIVATE_BEST_CHAINS_FAILED:
        // The dynamic argument is a util::Result message produced elsewhere in
        // the kernel; the outer message is translated, the nested text is not.
        return strprintf(_("Failed to connect best block (%s)."), args.at(0));
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

void KernelNotifications::flushError(kernel::FlushError error)
{
    AbortNode(m_shutdown_request, m_exit_status, FlushErrorMessage(error), &m_warnings);
}

void KernelNotifications::fatalError(kernel::FatalError error, std::vector<std::string> args)
{
    node::AbortNode(m_shutdown_on_fatal_error ? m_shutdown_request : nullptr,
                    m_exit_status, FatalErrorMessage(error, args), &m_warnings);
}

std::optional<uint256> KernelNotifications::TipBlock()
{
    AssertLockHeld(m_tip_block_mutex);
    return m_state.tip_block;
};


void ReadNotificationArgs(const ArgsManager& args, KernelNotifications& notifications)
{
    if (auto value{args.GetIntArg("-stopatheight")}) notifications.m_stop_at_height = *value;
}

} // namespace node
