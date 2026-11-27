// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "ExecutionSegmentsHost.hpp"

#include <iostream>

namespace {
using namespace segment_test;

void selectionAndRollback() {
    auto invalidHost = std::make_shared<Host>();
    try {
        App refused(invalidHost, options(0));
        throw std::runtime_error("zero segment allowance was accepted");
    } catch (const ExecutionFailure& error) {
        require(std::string(error.what()) == "evaluation steps per segment must be positive", "zero allowance diagnostic");
    }
    require(!invalidHost->marked(0), "invalid options entered authored construction");

    auto host = std::make_shared<Host>();
    App ordinary(host);
    expectPureExhaustion(ordinary, 10000);
    const auto failed = ordinary.Change(10000);
    require(failed.is_failure() && failed.error() == "source evaluation budget exceeded", "action exhaustion is not Failure");
    require(ordinary.snapshot().marker == 0, "source exhaustion published partial state");
    require(host->marked(7), "source exhaustion skipped finally");

    App selected(host, options(131072));
    require(selected.Query(10000) == 10000, "explicit profile did not reach pure root");
    bool offOwnerRefused = false;
    std::thread other([&] {
        try {
            (void)selected.Query(1);
        } catch (const ExecutionFailure&) {
            offOwnerRefused = true;
        }
    });
    other.join();
    require(offOwnerRefused, "normal pure query lost its existing owner contract");
    require(selected.Change(10000).value() == 10000 && selected.snapshot().marker == 41,
            "explicit profile did not reach action root");
    const auto shared = selected.Shared(10000);
    require(shared.is_failure() && shared.error() == "source evaluation budget exceeded",
            "nested source/interface/static calls minted fresh budgets");
    App larger(host, options(262144));
    require(larger.Shared(10000).value() == 20000, "larger finite host selection failed");
    App lower(host, options(8192));
    expectPureExhaustion(lower, 2000);
    const auto primary = lower.PrimaryFailure(2000);
    require(primary.is_failure() && primary.error() == "primary segment failure", "primary source failure changed");
    require(host->marked(2000), "cleanup lost its independent default reserve");
    const auto cleanup = larger.PrimaryFailure(10000);
    require(cleanup.is_failure() && cleanup.error() == "primary segment failure", "cleanup exhaustion replaced primary failure");
    require(!host->marked(10000), "normal selection incorrectly enlarged cleanup allowance");
}

void asyncSharingAndResets() {
    auto host = std::make_shared<Host>();
    App app(host, options(131072));
    auto immediate = app.ImmediateSegments(10000);
    finish(app, immediate);
    require(immediate.result().is_failure() && immediate.result().error() == "source evaluation budget exceeded",
            "immediate await refilled the segment or changed exhaustion identity");
    // Entering a local child commits pre-await state even on immediate completion.
    // Transaction publication and work-budget refill are separate boundaries.
    require(app.snapshot().marker == 51, "exhaustion rolled back state committed before the immediate child");
    auto shared = app.SharedAsync(10000);
    finish(app, shared);
    require(shared.result().is_failure() && shared.result().error() == "source evaluation budget exceeded",
            "awaited terminal exhaustion became a recoverable source Result");

    host->hold();
    auto first = app.HostSegments(10000, {});
    auto second = app.HostSegments(10000, {});
    require(!first.completed() && !second.completed(), "interleaved roots shared one allowance");
    pumpUntil([&] { app.poll(); }, [&] { return host->entered(); });
    host->release();
    finish(app, first);
    finish(app, second);
    require(first.result().is_success() && first.result().value() == 20000 &&
            second.result().is_success() && second.result().value() == 20000,
            "real host suspension did not restore each root's selected normal seed");
    require(host->marked(8), "host continuation cleanup did not run");

    auto semaphore = app.SemaphoreSegments(10000, {});
    require(!semaphore.completed(), "semaphore witness did not really suspend");
    require(app.ReleasePermit().is_success(), "semaphore release failed");
    finish(app, semaphore);
    require(semaphore.result().is_success() && semaphore.result().value() == 20000,
            "semaphore suspension refilled the wrong allowance");
}

void cleanupCancellationAndCommittedSegments() {
    auto host = std::make_shared<Host>();
    App app(host, options(131072));
    host->hold();
    auto cleanup = app.CleanupSegments();
    require(!cleanup.completed(), "cleanup witness did not really suspend");
    pumpUntil([&] { app.poll(); }, [&] { return host->entered(); });
    host->release();
    finish(app, cleanup);
    require(cleanup.result().is_failure() && cleanup.result().error() == "cleanup evaluation budget exceeded",
            "real cleanup await refilled its reserve");
    require(!host->marked(10000), "exhausted cleanup executed its final host effect");

    auto cancelledHost = std::make_shared<Host>();
    App cancelledApp(cancelledHost, options(131072));
    cancelledHost->hold();
    auto cancelled = cancelledApp.HostSegments(10000, {});
    pumpUntil([&] { cancelledApp.poll(); }, [&] { return cancelledHost->entered(); });
    cancelled.cancel();
    finish(cancelledApp, cancelled);
    require(cancelled.result().is_cancelled(), "task cancellation became budget failure/success");
    require(cancelledHost->marked(8), "task cancellation suppressed cleanup");
    require(cancelledApp.snapshot().marker == 1, "already committed pre-await state was falsely rolled back");

    auto failedHost = std::make_shared<Host>();
    App failedApp(failedHost, options(131072));
    failedHost->hold();
    auto failed = failedApp.HostSegments(10000, {});
    pumpUntil([&] { failedApp.poll(); }, [&] { return failedHost->entered(); });
    failedHost->release(true);
    finish(failedApp, failed);
    require(failed.result().is_failure() && failed.result().error() == "segment fixture host failure",
            "authoritative host failure was relabeled");
    require(failedApp.snapshot().marker == 1 && failedHost->marked(8), "host failure changed existing commit/cleanup rules");
}
} // namespace

int main() {
    try {
        selectionAndRollback();
        asyncSharingAndResets();
        cleanupCancellationAndCommittedSegments();
        std::cout << "source execution segment selection, rollback, async and cleanup contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
