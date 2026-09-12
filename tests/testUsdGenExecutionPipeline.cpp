#include "usdGen/executionPipeline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace usdGen;
using Pipeline = UsdGenExecutionPipeline;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); return 1; } } while(false)

// Bounded test rendezvous, not production scheduling or mutual exclusion.
template<class Predicate> bool Until(Predicate predicate) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

int main() {
    UsdGenExecutionRuntime runtime(4);
    Pipeline pipeline(runtime);
    std::atomic<int> completions{0}, publications{0};
    std::atomic<uint64_t> published{0};
    std::atomic<bool> orderError{false};
    auto work = [&](Pipeline::Cancellation const& cancel) -> Pipeline::Publish {
        return [&, epoch=cancel.epoch] {
            auto old = published.exchange(epoch);
            if (old >= epoch) orderError = true;
            ++publications;
        };
    };
    auto complete = [&](uint64_t, Pipeline::Outcome, std::exception_ptr) { ++completions; };
    std::vector<std::thread> submitters;
    for (int t=0;t<4;++t) submitters.emplace_back([&] {
        for (int i=0;i<32;++i) pipeline.Submit(work,complete);
    });
    for (auto& thread : submitters) thread.join();
    pipeline.Drain();
    CHECK(completions == 128 && publications > 0 && !orderError);
    CHECK(published == pipeline.AcceptedEpoch() && published == 128);
    std::vector<int> commands;
    for (int i=0;i<100;++i) CHECK(pipeline.PostCommand([&,i] { commands.push_back(i); }));
    pipeline.Drain();
    CHECK(commands.size() == 100 && pipeline.AcceptedEpoch() == 128);
    for (int i=0;i<100;++i) CHECK(commands[i] == i);

    // A new command is accepted while an older work item is still executing.
    std::atomic<bool> started{false}, observedCancellation{false}, timedOut{false};
    std::atomic<int> stalePublications{0}, superseded{0};
    pipeline.Submit([&](Pipeline::Cancellation const& cancel) -> Pipeline::Publish {
        started = true;
        observedCancellation = Until([&] { return cancel.Superseded(); });
        timedOut = !observedCancellation.load();
        return [&] { ++stalePublications; };
    }, [&](uint64_t, Pipeline::Outcome outcome, std::exception_ptr) {
        if (outcome == Pipeline::Outcome::Superseded) ++superseded;
    });
    CHECK(Until([&] { return started.load(); }));
    auto newer = pipeline.Submit(work);
    pipeline.Drain();
    CHECK(!timedOut && observedCancellation && superseded == 1 && stalePublications == 0);
    CHECK(published == newer);

    // Two independently owned descriptions actually execute simultaneously.
    Pipeline other(runtime);
    std::atomic<int> rendezvous{0}, parallelPublished{0};
    auto parallelWork = [&](Pipeline::Cancellation const&) -> Pipeline::Publish {
        ++rendezvous;
        if (!Until([&] { return rendezvous.load() == 2; })) timedOut = true;
        return [&] { ++parallelPublished; };
    };
    pipeline.Submit(parallelWork);
    other.Submit(parallelWork);
    pipeline.Drain(); other.Drain();
    CHECK(!timedOut && parallelPublished == 2);

    // A callback schedules later work without waiting or recursively cooking.
    bool rejectedWait = false;
    int childPublications = 0;
    pipeline.Submit([&](auto const&) -> Pipeline::Publish {
        return [&] {
            try { pipeline.Drain(); }
            catch (std::logic_error const&) { rejectedWait = true; }
            pipeline.Submit([&](auto const&) -> Pipeline::Publish {
                return [&] { ++childPublications; };
            });
        };
    });
    pipeline.Drain();
    CHECK(rejectedWait && childPublications == 1);

    int failures = 0;
    pipeline.Submit([](auto const&) -> Pipeline::Publish {
        throw std::runtime_error("intentional work failure");
    }, [&](uint64_t, Pipeline::Outcome outcome, std::exception_ptr error) {
        if (outcome == Pipeline::Outcome::Failed && error) ++failures;
        throw std::runtime_error("intentional completion failure");
    });
    pipeline.Drain();
    CHECK(failures == 1 && pipeline.CallbackFailures() == 1);
    CHECK(pipeline.Submit(work) != 0); pipeline.Drain();

    started = false; observedCancellation = false;
    pipeline.Submit([&](Pipeline::Cancellation const& cancel) -> Pipeline::Publish {
        started = true;
        observedCancellation = Until([&] { return cancel.Superseded(); });
        return [&] { ++stalePublications; };
    });
    CHECK(Until([&] { return started.load(); }));
    auto cancellation = pipeline.CancelPending(); pipeline.Drain();
    CHECK(observedCancellation && pipeline.AcceptedEpoch() == cancellation && stalePublications == 0);

    // Shutdown invalidates in-flight work before draining; its captured
    // resources stay alive until the work/completion messages retire.
    auto shutdown = std::make_unique<Pipeline>(runtime);
    started = false; observedCancellation = false;
    shutdown->Submit([&](Pipeline::Cancellation const& cancel) -> Pipeline::Publish {
        started = true;
        observedCancellation = Until([&] { return cancel.Superseded(); });
        return [&] { ++stalePublications; };
    });
    CHECK(Until([&] { return started.load(); }));
    shutdown.reset();
    CHECK(observedCancellation && stalePublications == 0);

    // Concurrent external compatibility callers each own a reply graph;
    // none concurrently wait on the shared execution graph.
    int ownerValue = 0;
    std::atomic<int> replyFailures{0};
    submitters.clear();
    for (int t=0;t<8;++t) submitters.emplace_back([&] {
        for (int i=0;i<32;++i) {
            int mine = 0;
            pipeline.InvokeOwner([&] { mine = ++ownerValue; });
            if (mine <= 0) ++replyFailures;
        }
    });
    for (auto& thread : submitters) thread.join();
    CHECK(ownerValue == 256 && replyFailures == 0);
    bool commandError = false;
    try { pipeline.InvokeOwner([] { throw std::runtime_error("owner command failure"); }); }
    catch (std::runtime_error const&) { commandError = true; }
    CHECK(commandError);
    pipeline.Await([](auto done) { done(); done(); }); // immediate/idempotent
    bool dispatchError = false;
    try { pipeline.Await([](auto) { throw std::runtime_error("dispatch failed before acceptance"); }); }
    catch (std::runtime_error const&) { dispatchError = true; }
    CHECK(dispatchError);
    int rejectedReplies = 0;
    pipeline.PostCommand([&] {
        try { pipeline.InvokeOwner([] {}); }
        catch (std::logic_error const&) { ++rejectedReplies; }
        try { pipeline.Await([](auto done) { done(); }); }
        catch (std::logic_error const&) { ++rejectedReplies; }
    });
    pipeline.Drain(); CHECK(rejectedReplies == 2);

    // Invalidation from an owner command takes effect during that command,
    // not behind an already queued Finished message.
    uint64_t invalidated = 0;
    pipeline.InvokeOwner([&] {
        invalidated = pipeline.CancelPending();
        if (pipeline.AcceptedEpoch() != invalidated) ++replyFailures;
    });
    CHECK(invalidated && replyFailures == 0);

    int completedCommands = 0, cancelledCommands = 0;
    {
        Pipeline closing(runtime);
        for (int i=0;i<128;++i)
            CHECK(closing.PostCommand([&] { ++completedCommands; }, [&] { ++cancelledCommands; }));
    }
    CHECK(completedCommands + cancelledCommands == 128);
    return 0;
}
